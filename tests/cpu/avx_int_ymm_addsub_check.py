"""Differential, boundary, mutation and rejection checks for 256-bit AVX2 packed add, subtract and compare."""

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
WIDTHS = {"b": 1, "w": 2, "d": 4, "q": 8}
KINDS = {"add": "vpadd", "sub": "vpsub", "cmpeq": "vpcmpeq", "cmpgt": "vpcmpgt"}
BASE_EXPORTS = tuple(
    (f"op_{prefix}{suffix}_ymm", kind, width)
    for kind, prefix in KINDS.items() for suffix, width in WIDTHS.items())
SPECIAL_EXPORTS = (
    "op_vpsubq_ymm_dst_src1", "op_vpsubd_ymm_dst_src2", "op_vpcmpgtq_ymm_dst_src2",
    "op_vpaddw_ymm_all", "op_vpcmpeqd_ymm_all", "op_vpcmpgtb_ymm_all",
    "op_vpaddq_ymm_then_xmm_subb", "op_vpcmpeqw_ymm_then_xmm_addb", "op_chain_ymm",
)
ALL_EXPORTS = tuple(name for name, _, _ in BASE_EXPORTS) + SPECIAL_EXPORTS


# --- Independent reference: plain Python integers, one element at a time ---

def lanes(buf, width):
    return struct.unpack(f"<{32 // width}{'BHIQ'[width.bit_length() - 1]}", buf[:32])


def pack(values, width):
    mask = (1 << (8 * width)) - 1
    return struct.pack(f"<{32 // width}{'BHIQ'[width.bit_length() - 1]}", *(v & mask for v in values))


def signed(value, width):
    bits = 8 * width
    return value - (1 << bits) if value >> (bits - 1) else value


def ref_binary(kind, width, a, b):
    ones = (1 << (8 * width)) - 1
    out = []
    for x, y in zip(lanes(a, width), lanes(b, width)):
        if kind == "add":
            out.append(x + y)
        elif kind == "sub":
            out.append(x - y)
        elif kind == "cmpeq":
            out.append(ones if x == y else 0)
        else:
            out.append(ones if signed(x, width) > signed(y, width) else 0)
    return pack(out, width)


def ref_low128(buf):
    return buf[:16] + b"\x00" * 16


def reference(name, a, b):
    for export, kind, width in BASE_EXPORTS:
        if export == name:
            return ref_binary(kind, width, a, b)
    if name in ("op_vpsubq_ymm_dst_src1", "op_vpsubd_ymm_dst_src2"):
        return ref_binary("sub", 8 if name.startswith("op_vpsubq") else 4, a, b)
    if name == "op_vpcmpgtq_ymm_dst_src2":
        return ref_binary("cmpgt", 8, a, b)
    if name == "op_vpaddw_ymm_all":
        return ref_binary("add", 2, a, a)
    if name == "op_vpcmpeqd_ymm_all":
        return b"\xff" * 32
    if name == "op_vpcmpgtb_ymm_all":
        return b"\x00" * 32
    if name == "op_vpaddq_ymm_then_xmm_subb":
        total = ref_binary("add", 8, a, b)
        return ref_low128(ref_binary("sub", 1, total, a))
    if name == "op_vpcmpeqw_ymm_then_xmm_addb":
        equal = ref_binary("cmpeq", 2, a, b)
        return ref_low128(ref_binary("add", 1, equal, equal))
    if name == "op_chain_ymm":
        t0 = ref_binary("add", 4, a, b)
        t3 = ref_binary("sub", 2, t0, b)
        t4 = ref_binary("cmpgt", 4, t3, a)
        return ref_binary("add", 8, t4, t3)
    raise KeyError(name)


# --- Inputs: both halves differ, carries and sign boundaries sit on lane edges ---

EDGES = {
    1: (0x00, 0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF),
    2: (0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF),
    4: (0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF),
    8: (0, 1, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x8000000000000001,
        0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFF),
}


def edge_pairs():
    pairs = []
    for width, edges in EDGES.items():
        count = 32 // width
        for shift in range(len(edges)):
            a = [edges[(i + shift) % len(edges)] for i in range(count)]
            b = [edges[(i * 3 + shift + 1) % len(edges)] for i in range(count)]
            pairs.append((pack(a, width), pack(b, width)))
            # Equal lanes in the lower half only, so compares disagree across the halves.
            c = a[:count // 2] + b[count // 2:]
            pairs.append((pack(a, width), pack(c, width)))
    return pairs


def carry_pair():
    """The low half's top quadword carries on add; the upper half must not see it."""
    a = struct.pack("<4Q", 0x1111111111111111, 0xFFFFFFFFFFFFFFFF, 0, 0x2222222222222222)
    b = struct.pack("<4Q", 0x0101010101010101, 0x0000000000000001, 1, 0x0000000000000001)
    return a, b


def borrow_pair():
    """The low half's top quadword borrows on subtract; the upper half must not see it."""
    a = struct.pack("<4Q", 0x1111111111111111, 0x0000000000000000, 5, 0x2222222222222222)
    b = struct.pack("<4Q", 0x0101010101010101, 0x0000000000000001, 1, 0x0000000000000001)
    return a, b


def all_pairs():
    rng = random.Random(0x5EED)
    pairs = edge_pairs() + [carry_pair(), borrow_pair()]
    for _ in range(160):
        a = rng.randbytes(32)
        b = bytearray(rng.randbytes(32))
        for lane in range(32):
            if rng.random() < 0.35:
                b[lane] = a[lane]
        pairs.append((a, bytes(b)))
    pairs.append((bytes(32), bytes(32)))
    pairs.append((b"\xff" * 32, b"\xff" * 32))
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
                    word = first_bad_word(got, want)
                    raise AssertionError(
                        f"{label} {name} case {index} word {word}: got {got.hex()} expected {want.hex()}")
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
    """Wrong references and a mutated output must be caught, and at the expected 64-bit word."""
    caught = 0
    # 1) A reference that ignores the upper half differs only in words 2 and 3.
    for name, kind, width in BASE_EXPORTS:
        words = set()
        for a, b in pairs:
            got = invoke(lifted[name], a, b)
            word = first_bad_word(got, ref_low128(ref_binary(kind, width, a, b)))
            if word is not None:
                words.add(word)
        if not words or min(words) < 2:
            raise AssertionError(f"{name}: low-half-only reference not caught in words 2/3 (saw {sorted(words)})")
        caught += 1
    # 2) A 256-bit carry or borrow across the halves differs exactly at word 2.
    for name, kind, (a, b) in (("op_vpaddq_ymm", "add", carry_pair()), ("op_vpsubq_ymm", "sub", borrow_pair())):
        got = invoke(lifted[name], a, b)
        x = int.from_bytes(a, "little")
        y = int.from_bytes(b, "little")
        flat = ((x + y) if kind == "add" else (x - y)) & ((1 << 256) - 1)
        word = first_bad_word(got, flat.to_bytes(32, "little"))
        if word != 2:
            raise AssertionError(f"{name}: cross-half carry reference expected to differ at word 2, got {word}")
        caught += 1
    # 3) Sibling operations and neighbouring widths must disagree with the wrong reference.
    for name, kind, width in BASE_EXPORTS:
        wrong = []
        if kind in ("add", "sub"):
            wrong.append(("sub" if kind == "add" else "add", width))
        else:
            wrong.append(("cmpgt" if kind == "cmpeq" else "cmpeq", width))
        wrong.extend((kind, other) for other in WIDTHS.values() if other != width)
        for wrong_kind, wrong_width in wrong:
            if not any(invoke(lifted[name], a, b) != ref_binary(wrong_kind, wrong_width, a, b)
                       for a, b in pairs[:60]):
                raise AssertionError(f"{name}: wrong reference {wrong_kind}/{wrong_width} was not caught")
            caught += 1
    # 4) The comparator reports the word of a single flipped bit.
    a, b = pairs[3]
    want = reference("op_chain_ymm", a, b)
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
    for name in (
        "VPADDB", "VPADDW", "VPADDD", "VPADDQ", "VPSUBB", "VPSUBW", "VPSUBD", "VPSUBQ",
        "VPCMPEQB", "VPCMPEQW", "VPCMPEQD", "VPCMPEQQ", "VPCMPGTB", "VPCMPGTW", "VPCMPGTD", "VPCMPGTQ",
    ):
        code = getattr(Code, f"VEX_{name}_YMM_YMM_YMMM256")
        source, count = lift_code(name, 0x1000, encode_seq(
            *loads, Instruction.create_reg_reg_reg(code, R.YMM0, R.YMM1, R.YMM2), ret))
        assert f"wide128(op_{name.lower()}, ymm1, ymm2)" in source and count == 4, name
        accepted += 1

    memory_reason = "unsupported memory operand for register-domain AVX integer instruction"
    cases = [
        ("vpaddb_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPADDB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpsubw_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPSUBW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpcmpgtq_m256", (loads[0], Instruction.create_reg_reg_mem(
            Code.VEX_VPCMPGTQ_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), ret), memory_reason),
        ("vpaddw_xmm_still_carved_out", (load(R.YMM1, R.RCX), load(R.YMM2, R.RDX),
            Instruction.create_reg_reg_reg(Code.VEX_VPADDW_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpaddd_xmm_still_carved_out", (loads[0], loads[1],
            Instruction.create_reg_reg_reg(Code.VEX_VPADDD_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpsubw_xmm_still_carved_out", (loads[0], loads[1],
            Instruction.create_reg_reg_reg(Code.VEX_VPSUBW_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret),
         "unsupported instruction"),
        ("vpmulld_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMULLD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret),
         "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpmulhuw_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPMULHUW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret),
         "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpaddd_ymm_nonvolatile_dst", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPADDD_YMM_YMM_YMMM256, R.YMM6, R.YMM1, R.YMM2), ret), "nonvolatile vector register"),
        ("vpsubq_ymm_nonvolatile_src", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPSUBQ_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM7), ret), "nonvolatile vector register"),
        ("vpcmpeqb_ymm_undefined_src", (loads[0], Instruction.create_reg_reg_reg(
            Code.VEX_VPCMPEQB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM3), ret),
         "undefined vector register ymm3"),
        ("vpaddw_ymm_evex", (*loads, Instruction.create_reg_reg_reg(
            Code.EVEX_VPADDW_YMM_K1Z_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret), "unsupported EVEX instruction"),
        ("vpaddq_ymm_missing_ret", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPADDQ_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2)), "missing terminal ret"),
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
