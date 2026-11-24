"""Differential, boundary, mutation and rejection checks for 256-bit AVX2 packed integer minimum and maximum."""

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
WIDTHS = {"b": 1, "w": 2, "d": 4}
# (export, pick, width, signed)
BASE_EXPORTS = tuple(
    (f"op_vp{pick}{sign}{suffix}_ymm", pick, WIDTHS[suffix], sign == "s")
    for pick in ("max", "min") for sign in ("s", "u") for suffix in WIDTHS)
SPECIAL_EXPORTS = (
    "op_vpmaxsd_ymm_dst_src2", "op_vpminuw_ymm_dst_src1", "op_vpmaxub_ymm_then_xmm_minsb",
)
ALL_EXPORTS = tuple(name for name, _, _, _ in BASE_EXPORTS) + SPECIAL_EXPORTS


# --- Independent reference: plain Python integers, one element at a time ---

def lanes(buf, width):
    return struct.unpack(f"<{32 // width}{'BHI'[width.bit_length() - 1]}", buf[:32])


def pack(values, width):
    mask = (1 << (8 * width)) - 1
    return struct.pack(f"<{32 // width}{'BHI'[width.bit_length() - 1]}", *(v & mask for v in values))


def signed(value, width):
    bits = 8 * width
    return value - (1 << bits) if value >> (bits - 1) else value


def ref_minmax(pick, width, is_signed, a, b):
    out = []
    for x, y in zip(lanes(a, width), lanes(b, width)):
        kx, ky = (signed(x, width), signed(y, width)) if is_signed else (x, y)
        if pick == "max":
            out.append(x if kx > ky else y)
        else:
            out.append(x if kx < ky else y)
    return pack(out, width)


def ref_low128(buf):
    return buf[:16] + b"\x00" * 16


def reference(name, a, b):
    for export, pick, width, is_signed in BASE_EXPORTS:
        if export == name:
            return ref_minmax(pick, width, is_signed, a, b)
    if name == "op_vpmaxsd_ymm_dst_src2":
        return ref_minmax("max", 4, True, a, b)
    if name == "op_vpminuw_ymm_dst_src1":
        return ref_minmax("min", 2, False, a, b)
    if name == "op_vpmaxub_ymm_then_xmm_minsb":
        top = ref_minmax("max", 1, False, a, b)
        return ref_low128(ref_minmax("min", 1, True, top, a))
    raise KeyError(name)


# --- Inputs: the two 128-bit halves differ and every lane type meets its sign boundary ---

EDGES = {
    1: (0x00, 0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF),
    2: (0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF),
    4: (0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF),
}


def edge_pairs():
    pairs = []
    for width, edges in EDGES.items():
        count = 32 // width
        for shift in range(len(edges)):
            a = [edges[(i + shift) % len(edges)] for i in range(count)]
            b = [edges[(i * 3 + shift + 1) % len(edges)] for i in range(count)]
            pairs.append((pack(a, width), pack(b, width)))
            # The upper half sees the operands swapped, so the halves never agree with each other.
            half = count // 2
            pairs.append((pack(a, width), pack(b[:half] + a[half:], width)))
            pairs.append((pack(a[:half] + b[half:], width), pack(b[:half] + a[half:], width)))
    return pairs


def all_pairs():
    rng = random.Random(0x6D1A)
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


def check_controls(lifted, pairs):
    """Wrong references must be caught for every export, and a flipped bit at its own 64-bit word."""
    caught = 0
    for name, pick, width, is_signed in BASE_EXPORTS:
        # 1) Upper half ignored: differs only in words 2 or 3.
        words = set()
        for a, b in pairs:
            word = first_bad_word(invoke(lifted[name], a, b), ref_low128(ref_minmax(pick, width, is_signed, a, b)))
            if word is not None:
                words.add(word)
        if not words or min(words) < 2:
            raise AssertionError(f"{name}: low-half-only reference not caught in words 2/3 (saw {sorted(words)})")
        caught += 1
        # 2) Signedness swapped, 3) opposite pick, 4) other widths.
        wrong = [(pick, width, not is_signed), ("min" if pick == "max" else "max", width, is_signed)]
        wrong += [(pick, other, is_signed) for other in WIDTHS.values() if other != width]
        for wrong_pick, wrong_width, wrong_signed in wrong:
            if not any(invoke(lifted[name], a, b) != ref_minmax(wrong_pick, wrong_width, wrong_signed, a, b)
                       for a, b in pairs[:60]):
                raise AssertionError(f"{name}: wrong reference {wrong_pick}/{wrong_width}/{wrong_signed} was not caught")
            caught += 1
    # 5) The comparator reports the word of a single flipped bit.
    a, b = pairs[3]
    want = reference("op_vpmaxub_ymm_then_xmm_minsb", a, b)
    for word in range(4):
        mutated = bytearray(want)
        mutated[8 * word + 5] ^= 0x04
        if first_bad_word(bytes(mutated), want) != word:
            raise AssertionError(f"mutation in word {word} not reported at that word")
        caught += 1
    return caught


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
    for export, pick, width, is_signed in BASE_EXPORTS:
        name = export[3:-4].upper()
        code = getattr(Code, f"VEX_{name}_YMM_YMM_YMMM256")
        source, count = lift_code(name, 0x1000, encode_seq(
            *loads, Instruction.create_reg_reg_reg(code, R.YMM0, R.YMM1, R.YMM2), ret))
        assert f"wide128(op_{name.lower()}, ymm1, ymm2)" in source and count == 4, name
        accepted += 1

    memory_reason = "unsupported memory operand for register-domain AVX integer instruction"
    cases = [
        ("vpmaxsd_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPMAXSD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpminuw_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPMINUW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpmaxsw_xmm_still_carved_out", (*loads,
            Instruction.create_reg_reg_reg(Code.VEX_VPMAXSW_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpminsw_xmm_still_carved_out", (*loads,
            Instruction.create_reg_reg_reg(Code.VEX_VPMINSW_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpmulld_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMULLD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret),
         "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpmaxub_ymm_nonvolatile_dst", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMAXUB_YMM_YMM_YMMM256, R.YMM6, R.YMM1, R.YMM2), ret), "nonvolatile vector register"),
        ("vpminsd_ymm_undefined_src", (loads[0], Instruction.create_reg_reg_reg(
            Code.VEX_VPMINSD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM3), ret), "undefined vector register ymm3"),
        ("vpmaxsb_ymm_evex", (*loads, Instruction.create_reg_reg_reg(
            Code.EVEX_VPMAXSB_YMM_K1Z_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret), "unsupported EVEX instruction"),
        ("vpminud_ymm_missing_ret", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMINUD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2)), "missing terminal ret"),
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
    accepted, rejected = check_translator()
    print(f"{accepted} 256-bit forms accepted; {rejected} rejection controls passed")
    if not host_has_avx2():
        print("Host CPU/OS lacks AVX2; skipping native differential check.")
        sys.exit(77)
    lifted, pairs, total = check_differential(native_path, lifted_path)
    caught = check_controls(lifted, pairs)
    print(f"{total} native/lifted/reference cases across {len(ALL_EXPORTS)} exports matched bit for bit; "
          f"{caught} negative controls were caught")


if __name__ == "__main__":
    main(*sys.argv[1:3])
