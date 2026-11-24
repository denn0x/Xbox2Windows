"""Differential checks, mutated-C++ controls, ALTER_IMM controls, and rejection controls for LAHF via ffx_version_query."""

import ctypes
from pathlib import Path
import random
import re
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Decoder, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges
from ffx_version_query import INTEGER_HEADER, lower, region

MASK64 = 0xFFFFFFFFFFFFFFFF
GUARD_PRE = bytes(0xA0 + (i % 16) for i in range(32))
GUARD_POST = bytes(0xC0 + (i % 16) for i in range(32))
FILL = b"\x5A" * 32

DIFF_SPECS = {
    "op_add8_lahf": ("add", 8, None),
    "op_add16_lahf": ("add", 16, None),
    "op_add32_lahf": ("add", 32, None),
    "op_add64_lahf": ("add", 64, None),
    "op_sub8_lahf": ("sub", 8, None),
    "op_sub16_lahf": ("sub", 16, None),
    "op_sub32_lahf": ("sub", 32, None),
    "op_sub64_lahf": ("sub", 64, None),
    "op_cmp8_lahf": ("cmp", 8, None),
    "op_cmp16_lahf": ("cmp", 16, None),
    "op_cmp32_lahf": ("cmp", 32, None),
    "op_cmp64_lahf": ("cmp", 64, None),
    "op_add8_al_lahf": ("add_al", 8, 5),
    "op_add32_imm_lahf": ("add", 32, (17, 1)),
    "op_sub32_imm_lahf": ("sub", 32, (15, 1)),
    "op_cmp64_imm_lahf": ("cmp", 64, (17, 1)),
}

EXPORTS = tuple(DIFF_SPECS.keys())
ALL_EXPORTS = EXPORTS + ("op_discarded_read",)
ALTERED_EXPORTS = (
    "op_add8_al_lahf",
    "op_add32_imm_lahf",
    "op_sub32_imm_lahf",
    "op_cmp64_imm_lahf",
)

# LAHF/SAHF in 64-bit mode: CPUID.80000001H:ECX[0].
CPUID_HELPER = """#include <intrin.h>

extern "C" __declspec(dllexport) uint32_t host_has_lahf() {
    int regs[4]{};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<uint32_t>(regs[0]) < 0x80000001u) return 0u;
    __cpuid(regs, static_cast<int>(0x80000001u));
    return (static_cast<uint32_t>(regs[2]) & 1u) != 0u ? 1u : 0u;
}
"""

EXTRA_CONTROL_HELPERS = """
static inline bool calc_pf_full(uint64_t val) {
    val ^= val >> 32u;
    val ^= val >> 16u;
    val ^= val >> 8u;
    val ^= val >> 4u;
    val ^= val >> 2u;
    val ^= val >> 1u;
    return (val & 1ull) == 0ull;
}
"""

PARITY_V_RE = re.compile(
    r"parity = \(\(0x6996u >> \(\(static_cast<unsigned>\(static_cast<uint8_t>\(v\)\) \^ "
    r"\(static_cast<unsigned>\(static_cast<uint8_t>\(v\)\) >> 4\)\) & 0xfu\)\) & 1u\) == 0;"
)

# Mutated-C++ negative controls: (base_export, control_name, regex, replacement)
CPP_MUTATIONS = (
    ("op_add32_lahf", "ctrl_af_bit4", re.compile(r"& 0x10u\) != 0;"), "& 0x20u) != 0;"),
    ("op_sub32_lahf", "ctrl_pf_full_width", PARITY_V_RE, "parity = calc_pf_full(v);"),
    ("op_cmp32_lahf", "ctrl_lahf_bit1_cleared", re.compile(r" \| 0x200ull"), ""),
    ("op_add32_lahf", "ctrl_lahf_clobber_rax", re.compile(r"\(rax & ~0xff00ull\) \| "), ""),
    ("op_sub64_lahf", "ctrl_lahf_keep_old_ah", re.compile(r"\(rax & ~0xff00ull\)"), "rax"),
    (
        "op_cmp64_lahf",
        "ctrl_lahf_clobber_flags",
        re.compile(r"\(word\(carry\) << 8\);"),
        "(word(carry) << 8); carry = !carry; zero = !zero; sign = !sign; overflow = !overflow;",
    ),
)


def _wrap(symbol: str, code: str) -> str:
    return f'extern "C" __declspec(dllexport) word {symbol}(word a, word b, word c) {{\n{code}\n}}'


def _lower_export(pe: pefile.PE, name: str) -> str:
    ranges = function_ranges(pe, export_rva(pe, name))
    body = [ins for lo, hi in ranges for ins in region(pe, lo, hi)]
    return lower(body, ["rcx = a; rdx = b; r8 = c;"], {}, {}, {})


def emit(native_path: Path, output_path: Path) -> None:
    """Lower all fixture exports through tools/ffx_version_query.py."""
    parts = [INTEGER_HEADER, CPUID_HELPER]
    with pefile.PE(str(native_path), fast_load=True) as pe:
        for name in ALL_EXPORTS:
            parts.append(_wrap(f"lifted_{name}", _lower_export(pe, name)))
    output_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def emit_controls(original_path: Path, output_path: Path) -> None:
    """Emit mutated-C++ negative control exports into output_path."""
    parts = [INTEGER_HEADER, EXTRA_CONTROL_HELPERS]
    with pefile.PE(str(original_path), fast_load=True) as pe:
        for base_name, ctrl_name, pattern, replacement in CPP_MUTATIONS:
            src = _lower_export(pe, base_name)
            mutated, count = pattern.subn(replacement, src)
            if count != 1:
                raise AssertionError(f"mutation {ctrl_name} changed {count} occurrences in {base_name}, expected 1")
            parts.append(_wrap(ctrl_name, mutated))
    output_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


# --- Independent Python ISA reference model ---

def calc_pf(val: int) -> int:
    return int(((val & 0xFF).bit_count() % 2) == 0)


def add_flags(bits: int, a: int, b: int):
    mask = (1 << bits) - 1
    ua = a & mask
    ub = b & mask
    total = ua + ub
    res = total & mask
    cf = int((total >> bits) != 0)
    af = int(((ua ^ ub ^ res) & 0x10) != 0)
    pf = calc_pf(res)
    zf = int(res == 0)
    sf = int(((res >> (bits - 1)) & 1) != 0)
    of = int((((ua ^ res) & (ub ^ res)) >> (bits - 1)) & 1)
    return res, (cf, pf, af, zf, sf, of)


def sub_flags(bits: int, a: int, b: int):
    mask = (1 << bits) - 1
    ua = a & mask
    ub = b & mask
    diff = ua - ub
    res = diff & mask
    cf = int(diff < 0)
    af = int(((ua ^ ub ^ res) & 0x10) != 0)
    pf = calc_pf(res)
    zf = int(res == 0)
    sf = int(((res >> (bits - 1)) & 1) != 0)
    of = int((((ua ^ ub) & (ua ^ res)) >> (bits - 1)) & 1)
    return res, (cf, pf, af, zf, sf, of)


def pack_lahf(flags) -> int:
    cf, pf, af, zf, sf = flags[:5]
    return (sf << 7) | (zf << 6) | (af << 4) | (pf << 2) | 0x02 | cf


def apply_lahf(rax: int, flags) -> int:
    return (rax & ~0xFF00) | (pack_lahf(flags) << 8)


def pack_lahf_and_flags(rax: int, flags) -> bytes:
    cf, _pf, _af, zf, sf, of = flags
    out = bytearray(FILL)
    struct.pack_into("<Q", out, 0, apply_lahf(rax, flags) & MASK64)
    out[8:13] = bytes((cf, int(sf != of), zf, sf, of))
    return bytes(out)


def u64s(buf: bytes):
    return struct.unpack("<4Q", buf)


def pack_u64s(*vals) -> bytes:
    return struct.pack("<4Q", *(v & MASK64 for v in vals))


def pack_out(*qwords) -> bytes:
    out = bytearray(FILL)
    for i, val in enumerate(qwords):
        struct.pack_into("<Q", out, i * 8, val & MASK64)
    return bytes(out)


def reference(name: str, a_bytes: bytes, b_bytes: bytes, altered: bool = False):
    a0, _, _, _ = u64s(a_bytes)
    b0, b1, _, _ = u64s(b_bytes)
    kind, bits, imm_spec = DIFF_SPECS[name]
    if kind == "add_al":
        rhs = imm_spec if altered else (b0 & 0xFF)
        al_res, flags = add_flags(8, a0, rhs)
        rax = (a0 & ~0xFF) | al_res
        return pack_lahf_and_flags(rax, flags), flags
    rhs = (imm_spec[1] if altered else imm_spec[0]) if imm_spec is not None else b1
    if kind == "add":
        _, flags = add_flags(bits, b0, rhs)
    else:
        _, flags = sub_flags(bits, b0, rhs)
    return pack_lahf_and_flags(a0, flags), flags


# --- Test vectors ---

RAX_PATTERNS = (
    0x123456789ABCDEF0,
    0xFFFFFFFFFFFF000F,
    0x000000000000FF80,
    0xDEADBEEFCAFE2855,
)


def pairs_for_width(bits: int):
    mask = (1 << bits) - 1
    sign = 1 << (bits - 1)
    edges = [
        # AF=1 without carry out of top bit
        (0x0F, 0x01),
        (0x08, 0x08),
        (0x10, 0x01),
        (0x20, 0x01),
        # AF=1 with carry/borrow out of top bit
        (mask, 0x01),
        (mask - 7, 0x08),
        (0x00, 0x01),
        (0x0F, mask),
        # AF=0 with carry/borrow out of bit 4 (catches bit-4 AF mutation)
        (0x10, 0x10),
        (0x11, 0x11),
        (0x20, 0x10),
        (0x30, 0x10),
        # Zero, sign, signed overflow, parity edges
        (0x00, 0x00),
        (0x01, 0x01),
        (sign - 1, 0x01),
        (sign, 0x01),
        (sign, sign),
        (mask, 0x00),
        (mask, mask),
    ]
    if bits >= 16:
        # Low-byte parity diverges from full-width parity
        edges.extend([
            (0x0100, 0x0000),
            (0x0103, 0x0000),
            (0x0101, 0x0000),
            (0x0100, 0x0001),
            (0x0301, 0x0100),
        ])
    rng = random.Random(0x1A4F00 + bits)
    for _ in range(6):
        edges.append((rng.getrandbits(bits), rng.getrandbits(bits)))
    return [
        (pack_u64s(RAX_PATTERNS[i % len(RAX_PATTERNS)], 0, 0, 0), pack_u64s(a, b, 0, 0))
        for i, (a, b) in enumerate(edges)
    ]


def inputs_for(name: str):
    _, bits, _ = DIFF_SPECS[name]
    return pairs_for_width(bits)


# --- 32-byte guarded buffer helpers ---

def alloc_aligned_buffer(payload: bytes, align: int = 32):
    total = len(GUARD_PRE) + len(payload) + len(GUARD_POST) + align
    buf = ctypes.create_string_buffer(total)
    base_addr = ctypes.addressof(buf)
    start_after_guard = base_addr + len(GUARD_PRE)
    pad = (align - start_after_guard % align) % align
    payload_off = len(GUARD_PRE) + pad
    pre_off = payload_off - len(GUARD_PRE)
    post_off = payload_off + len(payload)
    buf[pre_off:payload_off] = GUARD_PRE
    buf[payload_off:post_off] = payload
    buf[post_off : post_off + len(GUARD_POST)] = GUARD_POST
    return buf, base_addr + payload_off, pre_off, payload_off, post_off


def check_guards_and_read(buf, pre_off: int, payload_off: int, post_off: int, label: str) -> bytes:
    raw = buf.raw
    if raw[pre_off:payload_off] != GUARD_PRE or raw[post_off : post_off + len(GUARD_POST)] != GUARD_POST:
        raise AssertionError(f"32-byte guard bytes corrupted around {label}")
    return raw[payload_off:post_off]


def invoke(fn, a: bytes, b: bytes):
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(a)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(b)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL)
    ret_rax = fn(ptr_a, ptr_b, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "input_a")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "input_b")
    return ret_rax, check_guards_and_read(buf_out, o_pre, o_off, o_post, "output")


def assert_equal_output(label: str, name: str, idx, got: bytes, want: bytes, got_rax: int | None = None) -> None:
    if got != want:
        raise AssertionError(f"{label} {name} case {idx}: got {got.hex()} != expected {want.hex()}")
    if got_rax is not None:
        want_rax = struct.unpack_from("<Q", want, 0)[0]
        if got_rax != want_rax:
            raise AssertionError(f"{label} {name} case {idx}: returned RAX {got_rax:#018x} != {want_rax:#018x}")


def bind_exports(dll_path: Path, prefix: str = "", names=EXPORTS):
    lib = ctypes.CDLL(str(dll_path.resolve()))
    funcs = {}
    for name in names:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = ctypes.c_uint64
        funcs[name] = fn
    return lib, funcs


def check_discarded_read(native_path: Path, lifted_path: Path) -> None:
    payload = pack_u64s(0x1122334455667788, 0, 0, 0)
    for label, dll, prefix in (("native", native_path, ""), ("lifted", lifted_path, "lifted_")):
        _, fns = bind_exports(dll, prefix, ("op_discarded_read",))
        fn = fns["op_discarded_read"]
        source, ptr, _, _, _ = alloc_aligned_buffer(payload)
        for readable in (True, False):
            result, result_ptr, pre, off, post = alloc_aligned_buffer(FILL)
            ret_rax = None
            try:
                ret_rax = fn(ptr, ptr if readable else 0, result_ptr)
            except OSError as error:
                assert not readable and "access violation reading" in str(error).lower(), (label, error)
            else:
                assert readable, f"{label} op_discarded_read discarded a faulting read"
            expected = pack_out(0x1122334455667788, 0x1122334455667788) if readable else pack_out(0x1122334455667788)
            assert_equal_output(
                label,
                "op_discarded_read",
                readable,
                check_guards_and_read(result, pre, off, post, label),
                expected,
                ret_rax if readable else None,
            )
        _ = source


def check_differential(native_path: Path, lifted_path: Path, altered_path: Path, controls_path: Path) -> None:
    lifted_lib, lifted = bind_exports(lifted_path, "lifted_")
    lifted_lib.host_has_lahf.argtypes = []
    lifted_lib.host_has_lahf.restype = ctypes.c_uint32
    if not lifted_lib.host_has_lahf():
        print("Host CPU lacks LAHF/SAHF in 64-bit mode (CPUID 80000001H:ECX[0]); skipping native differential check.")
        raise SystemExit(77)

    check_discarded_read(native_path, lifted_path)

    _, native = bind_exports(native_path)
    _, altered = bind_exports(altered_path, "lifted_")
    _, controls = bind_exports(controls_path, "", tuple(c[1] for c in CPP_MUTATIONS))

    total = 0
    seen = {name: set() for name in ("CF", "PF", "AF", "ZF", "SF", "OF")}
    for name in EXPORTS:
        for idx, (a, b) in enumerate(inputs_for(name)):
            want, flags = reference(name, a, b)
            rax_n, got_n = invoke(native[name], a, b)
            rax_l, got_l = invoke(lifted[name], a, b)
            assert_equal_output("native", name, idx, got_n, want, rax_n)
            assert_equal_output("lifted", name, idx, got_l, want, rax_l)
            for key, bit in zip(("CF", "PF", "AF", "ZF", "SF", "OF"), flags):
                seen[key].add(bool(bit))
            total += 1

    for key, vals in seen.items():
        if vals != {False, True}:
            raise AssertionError(f"flag {key} never took both values: {vals}")

    # ALTER_IMM controls
    diverged = {}
    for name in ALTERED_EXPORTS:
        diffs = 0
        for idx, (a, b) in enumerate(inputs_for(name)):
            want_alt, _ = reference(name, a, b, altered=True)
            want_orig, _ = reference(name, a, b)
            rax_alt, got_alt = invoke(altered[name], a, b)
            assert_equal_output("altered", name, idx, got_alt, want_alt, rax_alt)
            if got_alt != want_orig:
                diffs += 1
        if not diffs:
            raise AssertionError(f"altered control {name} did not diverge from original")
        diverged[name] = diffs

    # Mutated-C++ controls (bit-4 AF, full-width PF, bit-1 cleared, RAX clobber, old AH kept, flag clobber)
    ctrl_diverged = {}
    for base_name, ctrl_name, _, _ in CPP_MUTATIONS:
        diffs = 0
        for a, b in inputs_for(base_name):
            want_orig, _ = reference(base_name, a, b)
            _, got_ctrl = invoke(controls[ctrl_name], a, b)
            if got_ctrl != want_orig:
                diffs += 1
        if not diffs:
            raise AssertionError(f"mutated-C++ control {ctrl_name} did not diverge from {base_name}")
        ctrl_diverged[ctrl_name] = diffs

    # Output mutation negative control
    sample_a, sample_b = inputs_for("op_add32_lahf")[0]
    expected, _ = reference("op_add32_lahf", sample_a, sample_b)
    mutated = bytearray(expected)
    mutated[1] ^= 0x10
    try:
        assert_equal_output("mutation", "op_add32_lahf", 0, bytes(mutated), expected)
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    print(
        f"{total} native/lifted/reference LAHF cases across {len(EXPORTS)} exports matched bit-for-bit "
        f"(full RAX + live SETcc, with CF/PF/AF/ZF/SF/OF each seen both ways); "
        f"ALTER_IMM diverged on {diverged}; C++ mutations diverged on {ctrl_diverged}; "
        f"dead-read and output mutation controls passed"
    )


# --- Alternate-encoding acceptance & rejection controls ---

def encode_seq(*instructions) -> bytes:
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_alt_encodings_and_rejections(original_path: Path) -> None:
    ld_rax = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.RCX))
    ld_r9 = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.R9, MemoryOperand(R.RDX))
    st_r8_rax = Instruction.create_mem_reg(Code.MOV_RM64_R64, MemoryOperand(R.R8), R.RAX)
    st_r8_8_setb = Instruction.create_mem(Code.SETB_RM8, MemoryOperand(R.R8, displ=8, displ_size=1))
    ret_ins = Instruction.create(Code.RETNQ)

    alt_accept = [
        ("add_ah_imm8_lahf", (
            ld_rax,
            Instruction.create_reg_i32(Code.ADD_RM8_IMM8, R.AH, 1),
            Instruction.create(Code.LAHF),
            st_r8_rax,
            st_r8_8_setb,
            ret_ins)),
        ("mov_neg_disp_r8_read", (
            Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.R8, displ=-8, displ_size=1)),
            Instruction.create_reg_i32(Code.ADD_EAX_IMM32, R.EAX, 1),
            Instruction.create(Code.LAHF),
            Instruction.create_reg_i32(Code.MOV_R32_IMM32, R.R9D, 0),
            Instruction.create_reg(Code.SETB_RM8, R.R9L),
            st_r8_rax,
            ret_ins)),
        ("cmp_al_imm8_setg", (
            ld_rax,
            Instruction.create_reg_i32(Code.CMP_AL_IMM8, R.AL, 5),
            Instruction.create(Code.LAHF),
            st_r8_rax,
            Instruction.create_mem(Code.SETG_RM8, MemoryOperand(R.R8, displ=8, displ_size=1)),
            ret_ins)),
        ("lahf_moves_between", (
            ld_rax,
            Instruction.create_reg_reg(Code.SUB_R64_RM64, R.RCX, R.RDX),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R9, R.RCX),
            Instruction.create_reg_mem(Code.LEA_R64_M, R.R10, MemoryOperand(R.RCX, displ=1, displ_size=1)),
            Instruction.create(Code.LAHF),
            st_r8_rax,
            ret_ins)),
    ]
    alt_expected = {
        "add_ah_imm8_lahf": "aux = ((a ^ b ^ v) & 0x10u) != 0;",
        "mov_neg_disp_r8_read": "load_always<uint64_t>(r8 + 18446744073709551608ull)",
        "cmp_al_imm8_setg": "(!zero && (sign == overflow))",
        "lahf_moves_between": "(word(aux) << 12)",
    }
    for label, seq in alt_accept:
        raw = encode_seq(*seq)
        decoded = list(Decoder(64, raw, ip=0x1000))
        assert len(decoded) == len(seq) and not any(ins.is_invalid for ins in decoded), label
        src = lower(decoded, ["rcx = a; rdx = b; r8 = c;"], {}, {}, {})
        if alt_expected[label] not in src:
            raise AssertionError(f"{label}: expected {alt_expected[label]!r} in generated C++:\n{src}")

    seg_mov = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.RCX))
    seg_mov.segment_prefix = R.GS

    unit_cases = [
        # Out-of-scope flag-stack and flag-mutating instructions
        ("out_of_scope_pushf", encode_seq(Instruction.create(Code.PUSHFW), ret_ins), "unsupported query operation"),
        ("out_of_scope_popf", encode_seq(Instruction.create(Code.POPFW), ret_ins), "unsupported query operation"),
        ("out_of_scope_pushfq", encode_seq(Instruction.create(Code.PUSHFQ), ret_ins), "unsupported query operation"),
        ("out_of_scope_popfq", encode_seq(Instruction.create(Code.POPFQ), ret_ins), "unsupported query operation"),
        ("out_of_scope_sahf", encode_seq(ld_rax, Instruction.create(Code.SAHF), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("out_of_scope_push_rax", encode_seq(ld_rax, Instruction.create_reg(Code.PUSH_R64, R.RAX), ret_ins),
         "unsupported query operation"),
        ("out_of_scope_pop_rax", encode_seq(Instruction.create_reg(Code.POP_R64, R.RAX), ret_ins),
         "unsupported query operation"),
        # SETP reads PF, which an ADD that no LAHF reads does not assign (#1153 models PF only where it is assigned).
        ("out_of_scope_setp", encode_seq(
            ld_rax, Instruction.create_reg_i32(Code.ADD_EAX_IMM32, R.EAX, 1),
            Instruction.create_mem(Code.SETP_RM8, MemoryOperand(R.R8)), ret_ins),
         "reads PF"),
        # #1022 LAHF source-discipline rejections (after other flag writers / memory arith / entry / control flow)
        ("refuse_lahf_after_inc", encode_seq(
            ld_rax, Instruction.create_reg(Code.INC_RM32, R.EAX), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_dec", encode_seq(
            ld_rax, Instruction.create_reg(Code.DEC_RM32, R.EAX), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_neg", encode_seq(
            ld_rax, Instruction.create_reg(Code.NEG_RM32, R.EAX), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_adc", encode_seq(
            ld_rax, Instruction.create_reg_i32(Code.ADC_EAX_IMM32, R.EAX, 1), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_sbb", encode_seq(
            ld_rax, Instruction.create_reg_i32(Code.SBB_EAX_IMM32, R.EAX, 1), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_clc", encode_seq(
            ld_rax, Instruction.create(Code.CLC), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_stc", encode_seq(
            ld_rax, Instruction.create(Code.STC), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_cmc", encode_seq(
            ld_rax, Instruction.create(Code.CMC), Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_after_and", encode_seq(
            ld_rax, ld_r9, Instruction.create_reg_reg(Code.AND_R64_RM64, R.RAX, R.R9),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_or", encode_seq(
            ld_rax, ld_r9, Instruction.create_reg_reg(Code.OR_R64_RM64, R.RAX, R.R9),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_xor", encode_seq(
            ld_rax, Instruction.create_reg_reg(Code.XOR_R32_RM32, R.EAX, R.EAX),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_test", encode_seq(
            ld_rax, ld_r9, Instruction.create_reg_reg(Code.TEST_RM64_R64, R.RAX, R.R9),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_shl", encode_seq(
            ld_rax, Instruction.create_reg_i32(Code.SHL_RM32_IMM8, R.EAX, 1),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_imul", encode_seq(
            ld_rax, ld_r9, Instruction.create_reg_reg(Code.IMUL_R64_RM64, R.RAX, R.R9),
            Instruction.create(Code.LAHF), ret_ins), "unsupported query operation"),
        ("refuse_lahf_after_add_mem_dst", bytes.fromhex("48011a9fc3"), "unsupported query operation"),
        ("refuse_lahf_after_cmp_mem_src", bytes.fromhex("483b0a9fc3"), "unsupported query operation"),
        ("refuse_lahf_entry", bytes.fromhex("9fc3"), "unsupported query operation"),
        ("refuse_lahf_after_mov_only", encode_seq(ld_rax, Instruction.create(Code.LAHF), ret_ins),
         "unsupported query operation"),
        ("refuse_lahf_across_call", bytes.fromhex("4801d1e8000000009fc3"), "unsupported query operation"),
        ("refuse_lahf_at_branch_target", bytes.fromhex("4801d174009fc3"), "unsupported query operation"),
        ("refuse_lahf_across_branch_target", bytes.fromhex("4801d14889c89f75fac3"), "unsupported query operation"),
        ("refuse_lahf_across_cond_branch", bytes.fromhex("4801d174019f90c3"), "unsupported query operation"),
        ("refuse_lahf_indirect_branch", encode_seq(
            ld_rax, Instruction.create_reg_reg(Code.ADD_R64_RM64, R.RCX, R.RDX),
            Instruction.create(Code.LAHF), Instruction.create_reg(Code.JMP_RM64, R.RAX)),
         "unsupported query operation"),
        # Prefixes, VEX, addressing, and stream-boundary rejections
        ("segment_prefix", encode_seq(seg_mov, ret_ins), "unsupported instruction prefix"),
        ("lock_prefix", bytes.fromhex("f00fb109") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("repne_prefix", bytes.fromhex("f2a4") + encode_seq(ret_ins), "unsupported REPNE string prefix"),
        ("evex_vaddps", bytes.fromhex("62f17c4858c2") + encode_seq(ret_ins), "unsupported"),
        ("eip_rel_mem", encode_seq(Instruction.create_reg_mem(
            Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.EIP, displ=0x1020, displ_size=4)), ret_ins),
         "unsupported EIP-relative operand"),
        ("retf_far", encode_seq(Instruction.create(Code.RETFQ)), "unsupported query operation"),
        ("missing_ret", encode_seq(ld_rax), "unbound fallthrough"),
    ]

    for label, raw, expected_reason in unit_cases:
        body = list(Decoder(64, raw, ip=0x1000))
        if not body or any(ins.is_invalid for ins in body):
            raise AssertionError(f"{label}: invalid test encoding {raw.hex()}")
        try:
            lower(body, [], {}, {}, {})
        except ValueError as exc:
            if expected_reason not in str(exc):
                raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator unexpectedly accepted rejected control")

    bound_writer = list(Decoder(64, bytes.fromhex("83c0019fc3"), ip=0x1000))
    try:
        lower(bound_writer, ["rax = 0xf;"], {}, {}, {}, host_ops={0x1000: ";"})
    except ValueError as exc:
        if "host binding at 0x1000" not in str(exc):
            raise AssertionError(f"wrong diagnostic for replaced LAHF flag writer: {exc}") from exc
    else:
        raise AssertionError("LAHF accepted the flags from a replaced ADD")

    # End-to-end PE patching controls on a copy of the built native DLL.
    pe_bytes = original_path.read_bytes()
    with pefile.PE(data=pe_bytes, fast_load=True) as pe:
        entry = export_rva(pe, "op_add64_lahf")
        file_off = pe.get_offset_from_rva(entry)
        pdata = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        records = pe.get_data(pdata.VirtualAddress, pdata.Size)
        rec_idx = next(
            i for i in range(0, len(records), 12)
            if struct.unpack_from("<I", records, i)[0] == entry
        )
        pdata_end_off = pe.get_offset_from_rva(pdata.VirtualAddress + rec_idx + 4)
        orig_end = struct.unpack_from("<I", records, rec_idx + 4)[0]
        max_len = orig_end - entry

    unit_map = {lbl: raw for lbl, raw, _ in unit_cases}
    pe_patches = [
        ("pe_out_of_scope_pushf", unit_map["out_of_scope_pushf"], "unsupported query operation"),
        ("pe_out_of_scope_popfq", unit_map["out_of_scope_popfq"], "unsupported query operation"),
        ("pe_refuse_test_lahf", unit_map["refuse_lahf_after_test"], "unsupported query operation"),
        ("pe_refuse_inc_lahf", unit_map["refuse_lahf_after_inc"], "unsupported query operation"),
        ("pe_refuse_and_lahf", unit_map["refuse_lahf_after_and"], "unsupported query operation"),
        ("pe_refuse_entry_lahf", unit_map["refuse_lahf_entry"], "unsupported query operation"),
        ("pe_refuse_add_mem_dst_lahf", unit_map["refuse_lahf_after_add_mem_dst"], "unsupported query operation"),
        ("pe_refuse_cmp_mem_src_lahf", unit_map["refuse_lahf_after_cmp_mem_src"], "unsupported query operation"),
        ("pe_refuse_branch_target_lahf", unit_map["refuse_lahf_at_branch_target"], "unsupported query operation"),
        ("pe_refuse_call_lahf", unit_map["refuse_lahf_across_call"], "unsupported query operation"),
        ("pe_invalid_bytes", bytes.fromhex("fffe") + encode_seq(ret_ins), "instruction boundary"),
    ]
    with TemporaryDirectory() as folder:
        patched_path = Path(folder) / "patched.dll"
        for label, patch, expected_reason in pe_patches:
            if len(patch) > max_len:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds op_add64_lahf ({max_len})")
            damaged = bytearray(pe_bytes)
            damaged[file_off : file_off + len(patch)] = patch
            struct.pack_into("<I", damaged, pdata_end_off, entry + len(patch))
            patched_path.write_bytes(damaged)
            try:
                with pefile.PE(str(patched_path), fast_load=True) as pe:
                    _lower_export(pe, "op_add64_lahf")
            except ValueError as exc:
                if expected_reason not in str(exc):
                    raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
            else:
                raise AssertionError(f"{label}: PE translate accepted rejected patch")

    print(
        f"{len(alt_accept)} alternate-encoding lift checks, {len(unit_cases)} unit rejection controls, "
        f"and {len(pe_patches)} PE-patched rejection controls passed"
    )


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "emit":
        emit(Path(sys.argv[2]), Path(sys.argv[3]))
    elif mode == "emit-controls":
        emit_controls(Path(sys.argv[2]), Path(sys.argv[3]))
    elif mode == "check":
        orig, lifted, alt, ctrls = (Path(p) for p in sys.argv[2:6])
        check_alt_encodings_and_rejections(orig)
        check_differential(orig, lifted, alt, ctrls)
    else:
        raise SystemExit(f"unknown mode {mode}")
