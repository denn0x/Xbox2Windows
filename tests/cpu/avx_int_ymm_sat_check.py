"""Differential, boundary, mutation and rejection checks for 256-bit AVX2 packed integer saturating add and subtract."""

import ctypes
from pathlib import Path
import random
import struct
import sys

from iced_x86 import Code, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_avx_int_aot import lift_code

GUARD_PRE = bytes(range(0xA0, 0xB0))
GUARD_POST = bytes(range(0xC0, 0xD0))
FILL = b"\x5A" * 32
WIDTHS = {"b": 1, "w": 2}
# (export, kind, signed saturation, width): the destination is src1 +/- src2 with saturation.
BASE_EXPORTS = tuple(
    (f"op_vp{kind}{sat}{suffix}_ymm", kind, sat == "s", WIDTHS[suffix])
    for kind in ("add", "sub") for sat in ("s", "us") for suffix in WIDTHS)
BASE_BY_NAME = {name: (kind, is_signed, width) for name, kind, is_signed, width in BASE_EXPORTS}
# The aliased subtract exports reuse one destination register as the first or the second source.
ALIAS_EXPORTS = tuple(
    f"{name}_{alias}" for name, kind, _, _ in BASE_EXPORTS if kind == "sub" for alias in ("dst_src1", "dst_src2"))
SPECIAL_EXPORTS = ("op_vpaddsb_ymm_all", "op_vpaddusb_ymm_then_xmm_subsb")
ALL_EXPORTS = tuple(name for name, _, _, _ in BASE_EXPORTS) + ALIAS_EXPORTS + SPECIAL_EXPORTS


# --- Independent reference: plain Python integers, one element at a time ---

def lanes(buf, width):
    return struct.unpack(f"<{32 // width}{'BH'[width - 1]}", buf[:32])


def pack(values, width):
    mask = (1 << (8 * width)) - 1
    return struct.pack(f"<{32 // width}{'BH'[width - 1]}", *(v & mask for v in values))


def signed(value, width):
    bits = 8 * width
    return value - (1 << bits) if value >> (bits - 1) else value


def saturate(kind, is_signed, width, x, y):
    """One element: signed saturation to [-2^(n-1), 2^(n-1)-1], unsigned saturation to [0, 2^n-1]."""
    bits = 8 * width
    if is_signed:
        x, y = signed(x, width), signed(y, width)
        low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    else:
        low, high = 0, (1 << bits) - 1
    exact = x + y if kind == "add" else x - y
    if exact > high:
        return high
    if exact < low:
        return low
    return exact


def ref_sat(kind, is_signed, width, a, b):
    return pack([saturate(kind, is_signed, width, x, y)
                 for x, y in zip(lanes(a, width), lanes(b, width))], width)


def ref_wrap(kind, width, a, b):
    return pack([x + y if kind == "add" else x - y for x, y in zip(lanes(a, width), lanes(b, width))], width)


def ref_low128(buf):
    return buf[:16] + b"\x00" * 16


def ref_swap_halves(buf):
    return buf[16:32] + buf[:16]


def reference(name, a, b):
    base = name
    for alias in ("_dst_src1", "_dst_src2"):
        if name.endswith(alias):
            base = name[:-len(alias)]
    if base in BASE_BY_NAME:
        kind, is_signed, width = BASE_BY_NAME[base]
        return ref_sat(kind, is_signed, width, a, b)
    if name == "op_vpaddsb_ymm_all":
        return ref_sat("add", True, 1, a, a)
    if name == "op_vpaddusb_ymm_then_xmm_subsb":
        top = ref_sat("add", False, 1, a, b)
        return ref_low128(ref_sat("sub", True, 1, top, a))
    raise KeyError(name)


# --- Inputs: the two 128-bit halves differ and every lane meets every saturation boundary pair ---

EDGES = {
    1: (0x00, 0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF),
    2: (0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF),
}


def edge_pairs():
    pairs = []
    for width, edges in EDGES.items():
        count = 32 // width
        combos = [(x, y) for x in edges for y in edges]
        for shift in range(len(combos)):
            # Over all shifts every (x, y) pair visits every lane; the upper half sees other pairs
            # than the lower half, so a swapped or ignored half cannot match.
            chosen = [combos[(i + shift) % len(combos)] for i in range(count)]
            pairs.append((pack([x for x, _ in chosen], width), pack([y for _, y in chosen], width)))
            # The uniform case: every lane holds the same pair, the upper half holds the reverse.
            x, y = combos[shift]
            half = count // 2
            pairs.append((pack([x] * half + [y] * half, width), pack([y] * half + [x] * half, width)))
    return pairs


def all_pairs():
    rng = random.Random(0x5A71)
    pairs = edge_pairs()
    for _ in range(160):
        a = rng.randbytes(32)
        b = bytearray(rng.randbytes(32))
        for lane in range(32):
            if rng.random() < 0.35:
                b[lane] = a[lane]
        pairs.append((a, bytes(b)))
    pairs.append((bytes(32), bytes(32)))
    pairs.append((b"\xff" * 32, b"\xff" * 32))
    pairs.append((bytes(32), b"\xff" * 32))
    pairs.append((b"\xff" * 32, bytes(32)))
    pairs.append((b"\x7f" * 32, b"\x01" * 32))
    pairs.append((b"\x80" * 32, b"\x01" * 32))
    return pairs


# --- Execution and comparison ---

def first_bad_word(got, want):
    for word in range(4):
        if got[8 * word:8 * word + 8] != want[8 * word:8 * word + 8]:
            return word
    return None


def invoke(fn, a, b, a_offset=0, out_offset=0):
    """Call one export on offset buffers; inputs must stay unchanged and the guards intact."""
    buf_a = ctypes.create_string_buffer(b"\x00" * a_offset + a + b"\x00" * 8, a_offset + 40)
    buf_b = ctypes.create_string_buffer(b + b"\x00" * 8, 40)
    guarded = ctypes.create_string_buffer(
        GUARD_PRE + b"\x77" * out_offset + FILL + b"\x77" * (8 - out_offset) + GUARD_POST, 32 + 8 + 32)
    out = ctypes.addressof(guarded) + len(GUARD_PRE) + out_offset
    fn(ctypes.addressof(buf_a) + a_offset, ctypes.addressof(buf_b), out)
    raw = guarded.raw
    if raw[:16] != GUARD_PRE or raw[-16:] != GUARD_POST:
        raise AssertionError("guard bytes corrupted around output buffer")
    if raw[16:16 + out_offset] != b"\x77" * out_offset or raw[16 + out_offset + 32:-16] != b"\x77" * (8 - out_offset):
        raise AssertionError("bytes next to the 32-byte output window were written")
    if buf_a.raw[a_offset:a_offset + 32] != a or buf_b.raw[:32] != b:
        raise AssertionError("input buffers were modified")
    return raw[16 + out_offset:16 + out_offset + 32]


def bind(path, prefix):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    funcs = {}
    for name in ALL_EXPORTS:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64] * 3
        fn.restype = None
        funcs[name] = fn
    return funcs


def host_has_avx2():
    if sys.platform == "win32":
        return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))  # PF_AVX2_INSTRUCTIONS_AVAILABLE
    try:
        return " avx2" in Path("/proc/cpuinfo").read_text(encoding="ascii", errors="replace")
    except OSError:
        return False


def check_differential(native_path, lifted_path):
    native = bind(native_path, "")
    lifted = bind(lifted_path, "lifted_")
    pairs = all_pairs()
    total = 0
    for name in ALL_EXPORTS:
        for index, (a, b) in enumerate(pairs):
            want = reference(name, a, b)
            for label, funcs in (("native", native), ("lifted", lifted)):
                got = invoke(funcs[name], a, b)
                if got != want:
                    raise AssertionError(
                        f"{label} {name} case {index} word {first_bad_word(got, want)}: "
                        f"got {got.hex()} expected {want.hex()}")
            total += 1
    # Unaligned inputs and outputs: vmovdqu has no alignment requirement and writes exactly 32 bytes.
    a, b = pairs[len(pairs) // 2]
    for name in ALL_EXPORTS:
        for a_offset, out_offset in ((1, 0), (0, 3), (7, 5)):
            want = reference(name, a, b)
            for label, funcs in (("native", native), ("lifted", lifted)):
                got = invoke(funcs[name], a, b, a_offset, out_offset)
                if got != want:
                    raise AssertionError(f"{label} {name} offsets {a_offset}/{out_offset}: {got.hex()} != {want.hex()}")
            total += 1
    return lifted, pairs, total


def differs_somewhere(fn, pairs, wrong):
    return any(invoke(fn, a, b) != wrong(a, b) for a, b in pairs)


def check_controls(lifted, pairs):
    """Wrong references must be caught for every export, and a flipped bit at its own 64-bit word."""
    caught = 0
    for name, kind, is_signed, width in BASE_EXPORTS:
        fn = lifted[name]
        # 1) Upper half ignored: differs only in words 2 or 3.
        words = set()
        for a, b in pairs:
            word = first_bad_word(invoke(fn, a, b), ref_low128(ref_sat(kind, is_signed, width, a, b)))
            if word is not None:
                words.add(word)
        if not words or min(words) < 2:
            raise AssertionError(f"{name}: low-half-only reference not caught in words 2/3 (saw {sorted(words)})")
        caught += 1
        other_kind = "sub" if kind == "add" else "add"
        wrong = {
            "halves swapped": lambda a, b: ref_swap_halves(ref_sat(kind, is_signed, width, a, b)),
            "wrapping arithmetic": lambda a, b: ref_wrap(kind, width, a, b),
            "opposite saturation": lambda a, b: ref_sat(kind, not is_signed, width, a, b),
            "opposite operation": lambda a, b: ref_sat(other_kind, is_signed, width, a, b),
            "other width": lambda a, b: ref_sat(kind, is_signed, 3 - width, a, b),
        }
        if kind == "sub":
            # src2 - src1 is the classic operand-order bug of the subtract forms.
            wrong["operands swapped"] = lambda a, b: ref_sat(kind, is_signed, width, b, a)
        for label, model in wrong.items():
            if not differs_somewhere(fn, pairs, model):
                raise AssertionError(f"{name}: wrong reference '{label}' was not caught")
            caught += 1
    # The aliased subtract exports must keep the register order, not the encoding order of the sources.
    for name in ALIAS_EXPORTS:
        base = name.rsplit("_dst_", 1)[0]
        kind, is_signed, width = BASE_BY_NAME[base]
        if not differs_somewhere(lifted[name], pairs, lambda a, b: ref_sat(kind, is_signed, width, b, a)):
            raise AssertionError(f"{name}: swapped operand order was not caught")
        caught += 1
    # The comparator reports the word of a single flipped bit.
    a, b = pairs[3]
    want = reference("op_vpaddusb_ymm_then_xmm_subsb", a, b)
    for word in range(4):
        mutated = bytearray(want)
        mutated[8 * word + 5] ^= 0x04
        if first_bad_word(bytes(mutated), want) != word:
            raise AssertionError(f"mutation in word {word} not reported at that word")
        caught += 1
    return caught


# --- Model self-test: spelled-out saturation values from the instruction set manual ---

def check_model():
    cases = [
        ("add", True, 1, 0x7F, 0x01, 0x7F), ("add", True, 1, 0x80, 0xFF, 0x80), ("add", True, 1, 0x01, 0x01, 0x02),
        ("add", False, 1, 0xFF, 0x01, 0xFF), ("add", False, 1, 0x80, 0x7F, 0xFF),
        ("sub", True, 1, 0x80, 0x01, 0x80), ("sub", True, 1, 0x7F, 0xFF, 0x7F), ("sub", True, 1, 0x00, 0x01, 0xFF),
        ("sub", False, 1, 0x00, 0x01, 0x00), ("sub", False, 1, 0x05, 0x02, 0x03), ("sub", False, 1, 0x02, 0x05, 0x00),
        ("add", True, 2, 0x7FFF, 0x0001, 0x7FFF), ("add", True, 2, 0x8000, 0xFFFF, 0x8000),
        ("add", False, 2, 0xFFFF, 0x0001, 0xFFFF), ("add", False, 2, 0x8000, 0x7FFF, 0xFFFF),
        ("sub", True, 2, 0x8000, 0x0001, 0x8000), ("sub", True, 2, 0x7FFF, 0xFFFF, 0x7FFF),
        ("sub", False, 2, 0x0000, 0x0001, 0x0000), ("sub", False, 2, 0x0005, 0x0002, 0x0003),
    ]
    for kind, is_signed, width, x, y, want in cases:
        got = saturate(kind, is_signed, width, x, y) & ((1 << (8 * width)) - 1)
        if got != want:
            raise AssertionError(f"model {kind} signed={is_signed} width={width} {x:#x},{y:#x}: {got:#x} != {want:#x}")
    return len(cases)


# --- Translator rejection and acceptance controls (host independent) ---

def encode_seq(*instructions):
    encoder = Encoder(64)
    ip = 0x1000
    for instruction in instructions:
        ip += encoder.encode(instruction, ip)
    return encoder.take_buffer()


def load(register, base):
    return Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, register, MemoryOperand(base))


def check_translator():
    ret = Instruction.create(Code.RETNQ)
    loads = (load(R.YMM1, R.RCX), load(R.YMM2, R.RDX))
    accepted = 0
    for export, _, _, _ in BASE_EXPORTS:
        name = export[3:-4].upper()
        code = getattr(Code, f"VEX_{name}_YMM_YMM_YMMM256")
        source, count = lift_code(name, 0x1000, encode_seq(
            *loads, Instruction.create_reg_reg_reg(code, R.YMM0, R.YMM1, R.YMM2), ret))
        assert f"wide128(op_{name.lower()}, ymm1, ymm2)" in source and count == 4, name
        accepted += 1
    # The aliased forms keep the source order of the instruction, whichever register is the destination.
    source, _ = lift_code("alias", 0x1000, encode_seq(
        *loads, Instruction.create_reg_reg_reg(Code.VEX_VPSUBUSB_YMM_YMM_YMMM256, R.YMM2, R.YMM1, R.YMM2), ret))
    assert "ymm2 = wide128(op_vpsubusb, ymm1, ymm2);" in source
    accepted += 1
    # The 128-bit encodings that are lowered here keep their own helper and no wide path.
    xmm_loads = (Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RCX)),
                 Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM2, MemoryOperand(R.RDX)))
    for export, _, _, _ in BASE_EXPORTS:
        name = export[3:-4].upper()
        code = getattr(Code, f"VEX_{name}_XMM_XMM_XMMM128")
        if name == "VPADDSW":
            continue
        source, _ = lift_code(name, 0x1000, encode_seq(
            *xmm_loads, Instruction.create_reg_reg_reg(code, R.XMM0, R.XMM1, R.XMM2), ret))
        assert f"ymm0 = op_{name.lower()}(ymm1, ymm2);" in source and "wide128(op_" + name.lower() not in source, name
        accepted += 1

    memory_reason = "unsupported memory operand for register-domain AVX integer instruction"
    cases = [
        ("vpaddsb_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPADDSB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpsubusw_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPSUBUSW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpaddsw_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPADDSW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpsubsb_m256_base_index", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPSUBSB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX, R.RCX, 2, 0x20)), ret),
         memory_reason),
        ("vpaddsw_xmm_still_carved_out", (*xmm_loads,
            Instruction.create_reg_reg_reg(Code.VEX_VPADDSW_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpavgb_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPAVGB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret),
         "unsupported instruction"),
        ("vpmulld_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMULLD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret),
         "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpaddusb_ymm_nonvolatile_dst", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPADDUSB_YMM_YMM_YMMM256, R.YMM6, R.YMM1, R.YMM2), ret), "nonvolatile vector register"),
        ("vpsubsw_ymm_undefined_src", (loads[0], Instruction.create_reg_reg_reg(
            Code.VEX_VPSUBSW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM3), ret), "undefined vector register ymm3"),
        ("vpaddsb_ymm_evex", (*loads, Instruction.create_reg_reg_reg(
            Code.EVEX_VPADDSB_YMM_K1Z_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret), "unsupported EVEX instruction"),
        ("vpsubusb_ymm_evex", (*loads, Instruction.create_reg_reg_reg(
            Code.EVEX_VPSUBUSB_YMM_K1Z_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret), "unsupported EVEX instruction"),
        ("vpsubusw_ymm_missing_ret", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPSUBUSW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2)), "missing terminal ret"),
    ]
    for label, sequence, reason in cases:
        try:
            lift_code(label, 0x1000, encode_seq(*sequence))
        except ValueError as exc:
            if reason not in str(exc):
                raise AssertionError(f"{label}: expected {reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator accepted a rejected control")
    return accepted, len(cases)


def main(native_path, lifted_path):
    model_cases = check_model()
    accepted, rejected = check_translator()
    print(f"{model_cases} model spot checks; {accepted} forms accepted; {rejected} rejection controls passed")
    if not host_has_avx2():
        print("Host CPU/OS lacks AVX2; skipping native differential check.")
        sys.exit(77)
    lifted, pairs, total = check_differential(native_path, lifted_path)
    caught = check_controls(lifted, pairs)
    print(f"{total} native/lifted/reference cases across {len(ALL_EXPORTS)} exports matched bit for bit; "
          f"{caught} negative controls were caught")


if __name__ == "__main__":
    main(*sys.argv[1:3])
