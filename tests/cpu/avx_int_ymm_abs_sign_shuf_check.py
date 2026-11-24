"""Differential, boundary, mutation and rejection checks for 256-bit AVX2 packed integer abs, sign and word shuffle."""

import ctypes
from pathlib import Path
import random
import struct
import sys

from iced_x86 import Code, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from avx_int_ymm_minmax_check import encode_seq, first_bad_word, host_has_avx2, invoke, load
from cpu_avx_int_aot import lift_code

WIDTHS = {"b": 1, "w": 2, "d": 4}
FMT = {1: "B", 2: "H", 4: "I"}
IDENTITY_IMM = 0xE4


# --- Independent reference: plain Python integers on each 16-byte half of the register ---

def unpack(half, width):
    return struct.unpack(f"<{16 // width}{FMT[width]}", half)


def pack(values, width):
    return struct.pack(f"<{16 // width}{FMT[width]}", *(v & ((1 << (8 * width)) - 1) for v in values))


def signed(value, width):
    return value - (1 << (8 * width)) if value >> (8 * width - 1) else value


def halves(fn, a, b):
    return fn(a[:16], b[:16]) + fn(a[16:], b[16:])


def abs_half(width, saturate=False):
    def fn(a, _b):
        out = []
        for x in unpack(a, width):
            v = abs(signed(x, width))
            out.append(min(v, (1 << (8 * width - 1)) - 1) if saturate else v)
        return pack(out, width)
    return fn


def sign_half(width, flip=False):
    def fn(a, b):
        out = []
        for x, y in zip(unpack(a, width), unpack(b, width)):
            c = signed(y, width)
            c = -c if flip else c
            out.append(0 if c == 0 else -x if c < 0 else x)
        return pack(out, width)
    return fn


def shuf_half(high, imm):
    def fn(a, _b):
        words = list(unpack(a, 2))
        base = 4 if high else 0
        for i in range(4):
            words[base + i] = unpack(a, 2)[base + ((imm >> (2 * i)) & 3)]
        return pack(words, 2)
    return fn


def chain_half(a, b):
    return sign_half(1)(abs_half(1)(a, b), b)


SPEC = {}
for suffix, width in WIDTHS.items():
    SPEC[f"op_vpabs{suffix}_ymm"] = (abs_half(width), "abs", width)
    SPEC[f"op_vpsign{suffix}_ymm"] = (sign_half(width), "sign", width)
for name, high in (("hw", True), ("lw", False)):
    for imm in (0x1B, 0x93):
        SPEC[f"op_vpshuf{name}_ymm_{imm:02x}"] = (shuf_half(high, imm), "shuf", (high, imm))
SPEC["op_vpabsd_ymm_dst_src1"] = (abs_half(4), "abs", 4)
SPEC["op_vpsignw_ymm_dst_src2"] = (sign_half(2), "sign", 2)
SPEC["op_vpsignb_ymm_all"] = (lambda a, _b: sign_half(1)(a, a), "all", 1)
SPEC["op_vpshufhw_ymm_dst_src1"] = (shuf_half(True, 0x1B), "shuf", (True, 0x1B))
ALL_EXPORTS = tuple(SPEC) + ("op_vpabsb_ymm_then_xmm_vpsignb",)


def reference(name, a, b):
    if name == "op_vpabsb_ymm_then_xmm_vpsignb":
        return chain_half(a[:16], b[:16]) + b"\x00" * 16
    if name == "op_vpsignb_ymm_all":
        return halves(sign_half(1), a, a)
    return halves(SPEC[name][0], a, b)


# --- Inputs: halves differ, signed boundaries and all three sign classes meet in the same register ---

EDGES = {
    1: (0x00, 0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF),
    2: (0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF),
    4: (0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF),
}


def all_pairs():
    pairs = []
    for width, edges in EDGES.items():
        count = 32 // width
        for shift in range(len(edges)):
            a = [edges[(i + shift) % len(edges)] for i in range(count)]
            b = [edges[(i * 3 + shift + 1) % len(edges)] for i in range(count)]
            pairs.append((pack(a[:count // 2], width) + pack(a[count // 2:], width),
                          pack(b[:count // 2], width) + pack(b[count // 2:], width)))
    rng = random.Random(0x5A17)
    pairs.append((struct.pack("<16H", *range(0x101, 0x111)), struct.pack("<16H", *range(0x201, 0x211))))
    pairs += [(rng.randbytes(32), rng.randbytes(32)) for _ in range(160)]
    pairs += [(bytes(32), bytes(32)), (b"\xff" * 32, b"\xff" * 32), (b"\x80" * 32, b"\xff" * 32)]
    return pairs


def bind(path, prefix):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    funcs = {}
    for name in ALL_EXPORTS:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64] * 3
        fn.restype = None
        funcs[name] = fn
    return funcs


def check_differential(native_path, lifted_path):
    native, lifted = bind(native_path, ""), bind(lifted_path, "lifted_")
    pairs = all_pairs()
    total = 0
    for name in ALL_EXPORTS:
        for index, (a, b) in enumerate(pairs):
            want = reference(name, a, b)
            for label, funcs in (("native", native), ("lifted", lifted)):
                got = invoke(funcs[name], a, b)
                if got != want:
                    raise AssertionError(f"{label} {name} case {index} word {first_bad_word(got, want)}: "
                                         f"got {got.hex()} expected {want.hex()}")
            total += 1
    a, b = pairs[len(pairs) // 2]
    for name in ALL_EXPORTS:  # unaligned input and output pointers; the guards must stay intact
        for a_offset, out_offset in ((1, 0), (0, 3), (7, 5)):
            for label, funcs in (("native", native), ("lifted", lifted)):
                if invoke(funcs[name], a, b, a_offset, out_offset) != reference(name, a, b):
                    raise AssertionError(f"{label} {name} offsets {a_offset}/{out_offset}")
            total += 1
    return lifted, pairs, total


def differing_words(lifted, name, pairs, wrong):
    words = set()
    for a, b in pairs:
        got, want = invoke(lifted[name], a, b), wrong(a, b)
        words |= {w for w in range(4) if got[8 * w:8 * w + 8] != want[8 * w:8 * w + 8]}
    return words


def check_controls(lifted, pairs):
    """Every wrong model must be caught, in the 64-bit words where the mistake lives."""
    caught = 0
    zero = b"\x00" * 16

    def expect(name, wrong, allowed):
        nonlocal caught
        words = differing_words(lifted, name, pairs, wrong)
        if not words or not words <= allowed:
            raise AssertionError(f"{name}: wrong model differs in words {sorted(words)}, expected a non-empty subset of {sorted(allowed)}")
        caught += 1

    for name, (fn, kind, param) in SPEC.items():
        if name.endswith(("dst_src1", "dst_src2", "_all")):
            continue
        expect(name, lambda a, b, fn=fn: fn(a[:16], b[:16]) + zero, {2, 3})            # upper half dropped
        expect(name, lambda a, b, fn=fn: fn(a[:16], b[:16]) + fn(a[:16], b[:16]), {2, 3})  # lower half's input reused
        expect(name, lambda a, b, fn=fn: fn(a[16:], b[16:]) + fn(a[16:], b[16:]), {0, 1})  # upper half's input reused
        if kind == "shuf":
            high, imm = param
            low_only = shuf_half(high, imm)
            ident = shuf_half(high, IDENTITY_IMM)
            expect(name, lambda a, b, low_only=low_only, ident=ident: low_only(a[:16], b[:16]) + ident(a[16:], b[16:]), {2, 3})
            expect(name, lambda a, b, low_only=low_only, ident=ident: ident(a[:16], b[:16]) + low_only(a[16:], b[16:]), {0, 1})
            expect(name, lambda a, b: shuf_half(not high, imm)(a[:16], b[:16]) + shuf_half(not high, imm)(a[16:], b[16:]), {0, 1, 2, 3})
        if kind == "abs":
            expect(name, lambda a, b, w=param: halves(abs_half(w, saturate=True), a, b), {0, 1, 2, 3})
            expect(name, lambda a, b, w=param: halves(abs_half({1: 2, 2: 4, 4: 1}[w]), a, b), {0, 1, 2, 3})
        if kind == "sign":
            expect(name, lambda a, b, w=param: halves(sign_half(w, flip=True), a, b), {0, 1, 2, 3})
            expect(name, lambda a, b, w=param: halves(sign_half({1: 2, 2: 4, 4: 1}[w]), a, b), {0, 1, 2, 3})
    # vpabsb of 0x80 stays 0x80 in both halves (the absolute value of the minimum is the minimum).
    got = invoke(lifted["op_vpabsb_ymm"], b"\x80" * 32, bytes(32))
    if got != b"\x80" * 32:
        raise AssertionError(f"vpabsb 0x80 must stay 0x80, got {got.hex()}")
    # The comparator reports a single flipped bit at its own 64-bit word.
    want = reference("op_vpabsb_ymm_then_xmm_vpsignb", *pairs[3])
    for word in range(4):
        mutated = bytearray(want)
        mutated[8 * word + 5] ^= 0x04
        if first_bad_word(bytes(mutated), want) != word:
            raise AssertionError(f"mutation in word {word} not reported at that word")
        caught += 1
    return caught


# --- Translator acceptance and rejection (host independent) ---

def check_translator():
    ret = Instruction.create(Code.RETNQ)
    loads = (load(R.YMM1, R.RCX), load(R.YMM2, R.RDX))
    ymm_unary = {f"VPABS{s}": f"op_vpabs{s.lower()}" for s in "BWD"}
    accepted = 0
    for name, helper in ymm_unary.items():
        code = getattr(Code, f"VEX_{name}_YMM_YMMM256")
        source, _ = lift_code(name, 0x1000, encode_seq(*loads[:1], Instruction.create_reg_reg(code, R.YMM0, R.YMM1), ret))
        assert f"wide128_unary({helper}, ymm1)" in source, name
        accepted += 1
    for s in "BWD":
        code = getattr(Code, f"VEX_VPSIGN{s}_YMM_YMM_YMMM256")
        source, _ = lift_code(s, 0x1000, encode_seq(*loads, Instruction.create_reg_reg_reg(code, R.YMM0, R.YMM1, R.YMM2), ret))
        assert f"wide128(op_vpsign{s.lower()}, ymm1, ymm2)" in source, s
        accepted += 1
    for name in ("VPSHUFHW", "VPSHUFLW"):
        code = getattr(Code, f"VEX_{name}_YMM_YMMM256_IMM8")
        source, _ = lift_code(name, 0x1000, encode_seq(*loads[:1], Instruction.create_reg_reg_u32(code, R.YMM0, R.YMM1, 0x93), ret))
        assert f"wide128_unary_imm(op_{name.lower()}, ymm1, 0x93u)" in source, name
        accepted += 1
    # The 128-bit encodings keep their own single-helper path.
    source, _ = lift_code("x", 0x1000, encode_seq(Instruction.create_reg_mem(
        Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RCX)), Instruction.create_reg_reg(
        Code.VEX_VPABSB_XMM_XMMM128, R.XMM0, R.XMM1), ret))
    assert "ymm0 = op_vpabsb(ymm1);" in source and "wide128_unary" not in source

    memory = "unsupported memory operand for register-domain AVX integer instruction"
    mem = MemoryOperand(R.RDX)
    cases = [
        ("vpabsb_m256", (loads[0], Instruction.create_reg_mem(Code.VEX_VPABSB_YMM_YMMM256, R.YMM0, mem), ret), memory),
        ("vpsignd_m256", (loads[0], Instruction.create_reg_reg_mem(Code.VEX_VPSIGND_YMM_YMM_YMMM256, R.YMM0, R.YMM1, mem), ret), memory),
        ("vpshuflw_m256", (Instruction.create_reg_mem_u32(Code.VEX_VPSHUFLW_YMM_YMMM256_IMM8, R.YMM0, mem, 0x1B), ret), memory),
        ("vpabsw_evex", (loads[0], Instruction.create_reg_reg(Code.EVEX_VPABSW_YMM_K1Z_YMMM256, R.YMM0, R.YMM1), ret),
         "unsupported EVEX instruction"),
        ("vpshufhw_evex", (loads[0], Instruction.create_reg_reg_u32(
            Code.EVEX_VPSHUFHW_YMM_K1Z_YMMM256_IMM8, R.YMM0, R.YMM1, 0x1B), ret), "unsupported EVEX instruction"),
        ("vpmovsxbw_ymm_xmm_source", (loads[0], Instruction.create_reg_reg(
            Code.VEX_VPMOVSXBW_YMM_XMMM128, R.YMM0, R.XMM1), ret), "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpshufb_ymm_other_form", (*loads, Instruction.create_reg_reg_reg(
            Code.VEX_VPSHUFB_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret), "unsupported instruction"),
        ("vpalignr_ymm_other_form", (*loads, Instruction.create_reg_reg_reg_u32(
            Code.VEX_VPALIGNR_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, R.YMM2, 4), ret), "unsupported VEX.256 (L=1) packed-integer encoding"),
        ("vpabsd_nonvolatile_dst", (loads[0], Instruction.create_reg_reg(Code.VEX_VPABSD_YMM_YMMM256, R.YMM6, R.YMM1), ret),
         "nonvolatile vector register"),
        ("vpshufhw_nonvolatile_dst", (loads[0], Instruction.create_reg_reg_u32(
            Code.VEX_VPSHUFHW_YMM_YMMM256_IMM8, R.YMM7, R.YMM1, 0x1B), ret), "nonvolatile vector register"),
        ("vpsignw_undefined_src", (loads[0], Instruction.create_reg_reg_reg(
            Code.VEX_VPSIGNW_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM3), ret), "undefined vector register ymm3"),
        ("vpabsb_missing_ret", (loads[0], Instruction.create_reg_reg(Code.VEX_VPABSB_YMM_YMMM256, R.YMM0, R.YMM1)),
         "missing terminal ret"),
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
