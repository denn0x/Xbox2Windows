"""Differential checks, ALTER_IMM/mutation controls and rejection controls for 64-bit pcmpestr*64 AOT."""

import ctypes
import os
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Decoder, Encoder, Formatter, FormatterSyntax, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import bounded_code, export_rva
from cpu_pcmpstr64_aot import lift_code, translate

GUARD_PRE = bytes(range(0xA0, 0xB0))
GUARD_POST = bytes(range(0xC0, 0xD0))
SENTINEL_HI = bytes(range(0xB0, 0xC0))

# (name, orig_imm, alt_imm, is_mask, is_vex, is_word, is_alias_self, check_xmm1)
SPECS = (
    ("pcmpestri64_ub_eqany_pos_lsb", 0x00, 0x01, False, False, False, False, False),
    ("pcmpestri64_sw_ranges_neg_msb", 0x57, None, False, False, True, False, False),
    ("pcmpestri64_mem_sb_eqeach_mpos_msb", 0x6A, None, False, False, False, False, False),
    ("pcmpestri64_alias_xmm0", 0x9C, None, False, False, False, True, False),
    ("pcmpestrm64_ub_eqany_pos_bit", 0x00, None, True, False, False, False, True),
    ("pcmpestrm64_sb_ranges_neg_exp", 0x56, 0x16, True, False, False, False, False),
    ("pcmpestrm64_mem_uw_eqord_mneg_exp", 0x7D, None, True, False, True, False, False),
    ("pcmpestrm64_xmm0_self", 0x40, None, True, False, False, False, False),
    ("vpcmpestri64_ub_eqany_pos_lsb", 0x00, 0x01, False, True, False, False, False),
    ("vpcmpestri64_uw_eqord_mneg_lsb", 0x3D, None, False, True, True, False, False),
    ("vpcmpestri64_mem_sw_ranges_neg_msb", 0x57, None, False, True, True, False, False),
    ("vpcmpestri64_alias_xmm0", 0x9C, None, False, True, False, True, False),
    ("vpcmpestrm64_ub_eqany_pos_bit", 0x00, None, True, True, False, False, True),
    ("vpcmpestrm64_uw_eqeach_mpos_exp", 0x69, 0x29, True, True, True, False, True),
    ("vpcmpestrm64_mem_ub_eqany_exp", 0x40, None, True, True, False, False, False),
    ("vpcmpestrm64_xmm0_self", 0x40, None, True, True, False, False, False),
)

ALL_EXPORTS = tuple(row[0] for row in SPECS) + ("vpcmpestrm64_discarded_read",)

BYTE_PAIRS = (
    (b"aeiou\x00xxxxxxxxxx", b"hello world!\x00xyz"),
    (b"azAZ09\x80\x7f\x00abcdefg", b"mZ!\x80\x00hello\x7f\xff09ab"),
    (b"ab" + b"\x00" * 14, b"xxabxabyab" + b"\x00" * 6),
    (b"abc" + b"\x00" * 13, b"0123456789abcdab"),
    (b"\x00" * 16, b"\x00" * 16),
    (b"za" + b"\x80\x7f" + b"\x00" * 12, b"\x80\xff\x00\x01\x7f\x81azbcdefghi"),
    (b"0123456789abcdef", b"0123456789abcdef"),
    (b"0123456789abcdef", b"012345X789abcdef"),
)

WORD_PAIRS = (
    (
        struct.pack("<8H", 10, 20, 30, 0x8000, 0, 1, 2, 3),
        struct.pack("<8H", 99, 20, 0x8000, 10, 0, 5, 10, 30),
    ),
    (
        struct.pack("<8h", -100, -1, 10, 50, -32768, 32767, 0, 0),
        struct.pack("<8h", -101, -50, 0, 25, -32768, 32767, 100, -1),
    ),
    (
        struct.pack("<8H", 1, 2, 3, 0, 0, 0, 0, 0),
        struct.pack("<8H", 9, 1, 2, 3, 9, 9, 1, 2),
    ),
    (
        struct.pack("<8H", 0, 0, 0, 0, 0, 0, 0, 0),
        struct.pack("<8H", 1, 2, 3, 4, 5, 6, 7, 8),
    ),
    (
        struct.pack("<8H", 10, 10, 0, 0, 0, 0, 0, 0),
        struct.pack("<8H", 10, 0, 0, 0, 0, 0, 0, 0),
    ),
)

# Signed 64-bit length pairs, including cases that diverge from 32-bit clamping:
# - 0x1_0000_0000 (32-bit sees 0, 64-bit saturates to limit)
# - 0x0000_0000_FFFF_FFFF (32-bit sees -1 -> 1, 64-bit saturates to limit)
# - -0xFFFF_FFFb (0xFFFF_FFFF_0000_0005: 32-bit sees 5, 64-bit saturates to limit)
# - INT64_MIN (-0x8000_0000_0000_0000)
EXPLICIT_LEN64_PAIRS = (
    (0, 0),
    (0, 5),
    (1, 5),
    (1, 15),
    (3, 16),
    (4, 8),
    (8, 8),
    (16, 16),
    (17, 5),
    (-1, 5),
    (4, -5),
    (-17, -17),
    (0x1_0000_0000, 5),
    (5, 0x1_0000_0000),
    (0x0000_0000_FFFF_FFFF, 5),
    (-4294967291, 5),
    (-0x80000000, 6),
    (-0x8000000000000000, 6),
)


def sdm_pcmp64(a_bytes: bytes, b_bytes: bytes, imm: int, raw_la: int, raw_lb: int):
    """Independent Intel SDM reference model for 64-bit *PCMPESTRI64 / *PCMPESTRM64."""
    is_word = bool(imm & 1)
    is_signed = bool(imm & 2)
    limit = 8 if is_word else 16
    agg = (imm >> 2) & 3
    pol = (imm >> 4) & 3
    high_sel = bool(imm & 0x40)

    fmt = f"<{limit}{'h' if is_signed else 'H'}" if is_word else f"<{limit}{'b' if is_signed else 'B'}"
    elems_a = list(struct.unpack(fmt, a_bytes))
    elems_b = list(struct.unpack(fmt, b_bytes))

    def clamp_len64(raw: int) -> int:
        signed = struct.unpack("<q", struct.pack("<Q", raw & 0xFFFFFFFFFFFFFFFF))[0]
        return min(abs(signed), limit)

    len1 = clamp_len64(raw_la)
    len2 = clamp_len64(raw_lb)

    int_res1 = 0
    for i in range(limit):
        if agg == 0:
            bit = i < len2 and any(elems_a[j] == elems_b[i] for j in range(len1))
        elif agg == 1:
            bit = i < len2 and any(
                elems_a[j] <= elems_b[i] <= elems_a[j + 1]
                for j in range(0, len1 - 1, 2)
            )
        elif agg == 2:
            if i < len1 and i < len2:
                bit = elems_a[i] == elems_b[i]
            else:
                bit = i >= len1 and i >= len2
        else:
            bit = all(
                (i + j) < len2 and elems_a[j] == elems_b[i + j]
                for j in range(min(len1, limit - i))
            )
        if bit:
            int_res1 |= 1 << i

    if pol == 1:
        int_res2 = int_res1 ^ ((1 << limit) - 1)
    elif pol == 3:
        int_res2 = int_res1 ^ ((1 << len2) - 1)
    else:
        int_res2 = int_res1

    if int_res2 == 0:
        index = limit
    elif not high_sel:
        index = (int_res2 & -int_res2).bit_length() - 1
    else:
        index = int_res2.bit_length() - 1

    if not high_sel:
        mask_bytes = struct.pack("<H", int_res2) + b"\x00" * 14
    elif not is_word:
        mask_bytes = bytes(0xFF if ((int_res2 >> k) & 1) else 0x00 for k in range(16))
    else:
        mask_bytes = b"".join(struct.pack("<H", 0xFFFF if ((int_res2 >> k) & 1) else 0x0000) for k in range(8))

    # Packed EFLAGS: CF(0), ZF(1), SF(2), OF(3); PF(4) must always be cleared to 0.
    flags = (
        (1 if int_res2 != 0 else 0)
        | (2 if len2 < limit else 0)
        | (4 if len1 < limit else 0)
        | (8 if (int_res2 & 1) else 0)
    )
    return index, mask_bytes, flags


def bind_exports(dll_path: Path, prefix: str = ""):
    lib = ctypes.CDLL(str(dll_path.resolve()))
    funcs = {}
    for name in ALL_EXPORTS:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = ctypes.c_uint64
        funcs[name] = fn
    return lib, funcs


def invoke_case(fn, a_bytes: bytes, b_bytes: bytes, la: int, lb: int):
    a_buf = ctypes.create_string_buffer(a_bytes + SENTINEL_HI, 32)
    b_buf = ctypes.create_string_buffer(b_bytes, 16)
    header = struct.pack("<qq", la, lb) + SENTINEL_HI + b"\xAA" * 32
    guarded = ctypes.create_string_buffer(GUARD_PRE + header + GUARD_POST, 96)
    io_ptr = ctypes.addressof(guarded) + len(GUARD_PRE)
    flags = fn(ctypes.addressof(a_buf), ctypes.addressof(b_buf), io_ptr)
    raw = guarded.raw
    if raw[:16] != GUARD_PRE or raw[80:] != GUARD_POST:
        raise AssertionError("guard bytes corrupted around 64-byte io buffer")
    return flags, raw[16:80]


def verify_output(label: str, name: str, is_mask: bool, is_vex: bool, check_xmm1: bool,
                  a_bytes: bytes, b_bytes: bytes, imm: int, la: int, lb: int,
                  got_flags: int, io_bytes: bytes):
    b_eff = a_bytes if "alias_xmm0" in name else b_bytes
    exp_idx, exp_mask, exp_flags = sdm_pcmp64(a_bytes, b_eff, imm, la, lb)
    if got_flags != exp_flags:
        raise AssertionError(
            f"{label} {name} imm={imm:#x} la={la} lb={lb}: flags={got_flags:#x} != exp={exp_flags:#x}"
        )
    if is_mask:
        got_ymm = io_bytes[:32]
        exp_hi = b"\x00" * 16 if is_vex else SENTINEL_HI
        exp_ymm = exp_mask + exp_hi
        if got_ymm != exp_ymm:
            raise AssertionError(
                f"{label} {name} imm={imm:#x} la={la} lb={lb}: ymm0={got_ymm.hex()} != exp={exp_ymm.hex()}"
            )
        if check_xmm1 and io_bytes[32:48] != a_bytes:
            raise AssertionError(f"{label} {name}: xmm1 clobbered during implicit XMM0 write")
    else:
        got_rcx = struct.unpack("<Q", io_bytes[:8])[0]
        if got_rcx != exp_idx:
            raise AssertionError(
                f"{label} {name} imm={imm:#x} la={la} lb={lb}: rcx={got_rcx:#x} != exp={exp_idx:#x}"
            )


def record_covered_forms(original_path: Path) -> None:
    record = os.environ.get("FORM_COVERAGE_RECORD")
    if not record:
        return
    formatter = Formatter(FormatterSyntax.INTEL)
    forms = set()
    with pefile.PE(str(original_path), fast_load=True) as pe:
        for name in ALL_EXPORTS:
            entry = export_rva(pe, name)
            raw = bounded_code(pe, entry)
            for ins in Decoder(64, raw, ip=entry):
                forms.add(formatter.format_mnemonic(ins).lower())
    out_path = Path(record)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("".join(word + "\n" for word in sorted(forms)), encoding="utf-8")


def check_differential(native_path: Path, lifted_path: Path, altered_path: Path) -> None:
    lifted_lib, lifted = bind_exports(lifted_path, "lifted_")
    lifted_lib.host_has_pcmpstr64.argtypes = []
    lifted_lib.host_has_pcmpstr64.restype = ctypes.c_uint32
    if not lifted_lib.host_has_pcmpstr64():
        print("host lacks SSE4.2/AVX/OSXSAVE; skipping native differential check")
        raise SystemExit(77)

    _, native = bind_exports(native_path)
    _, altered = bind_exports(altered_path, "lifted_")

    for label, functions in (("native", native), ("lifted", lifted)):
        fn = functions["vpcmpestrm64_discarded_read"]
        source = ctypes.create_string_buffer(16)
        result = ctypes.c_uint32()
        ptr = ctypes.addressof(source)
        assert fn(ptr, ptr, ctypes.addressof(result)) == 0x55667788
        assert result.value == 0x55667788
        result.value = 0
        try:
            fn(ptr, 0, ctypes.addressof(result))
        except OSError as error:
            assert "access violation reading" in str(error).lower(), (label, error)
        else:
            raise AssertionError(f"{label} discarded the faulting memory compare")
        assert result.value == 0x11223344, (label, result.value)

    total = 0
    for name, imm, _, is_mask, is_vex, is_word, _, check_xmm1 in SPECS:
        vec_pairs = WORD_PAIRS if is_word else BYTE_PAIRS
        for a_bytes, b_bytes in vec_pairs:
            for la, lb in EXPLICIT_LEN64_PAIRS:
                fn_n, fn_l = native[name], lifted[name]
                flags_n, io_n = invoke_case(fn_n, a_bytes, b_bytes, la, lb)
                flags_l, io_l = invoke_case(fn_l, a_bytes, b_bytes, la, lb)
                verify_output("native", name, is_mask, is_vex, check_xmm1, a_bytes, b_bytes, imm, la, lb, flags_n, io_n)
                verify_output("lifted", name, is_mask, is_vex, check_xmm1, a_bytes, b_bytes, imm, la, lb, flags_l, io_l)
                total += 1

    # Verify ALTER_IMM exports match altered reference and diverge from original reference.
    diverged = {}
    for name, orig_imm, alt_imm, is_mask, is_vex, is_word, _, _ in SPECS:
        if alt_imm is None:
            continue
        vec_pairs = WORD_PAIRS if is_word else BYTE_PAIRS
        diffs = 0
        for a_bytes, b_bytes in vec_pairs:
            for la, lb in EXPLICIT_LEN64_PAIRS:
                flags_a, io_a = invoke_case(altered[name], a_bytes, b_bytes, la, lb)
                verify_output("altered", name, is_mask, is_vex, False, a_bytes, b_bytes, alt_imm, la, lb, flags_a, io_a)
                orig_idx, orig_mask, orig_flags = sdm_pcmp64(a_bytes, b_bytes, orig_imm, la, lb)
                alt_out = io_a[:16] if is_mask else struct.unpack("<Q", io_a[:8])[0]
                orig_out = orig_mask if is_mask else orig_idx
                if flags_a != orig_flags or alt_out != orig_out:
                    diffs += 1
        if not diffs:
            raise AssertionError(f"altered control {name} never diverged from original")
        diverged[name] = diffs

    # Negative comparator control: mutating one bit of expected output must raise AssertionError.
    sample_a, sample_b = BYTE_PAIRS[0]
    flags_n, io_n = invoke_case(native[ALL_EXPORTS[4]], sample_a, sample_b, 5, 12)
    mutated = bytearray(io_n)
    mutated[0] ^= 0x01
    try:
        verify_output("mutation", ALL_EXPORTS[4], True, False, False, sample_a, sample_b, 0x00, 5, 12, flags_n, bytes(mutated))
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    record_covered_forms(native_path)
    print(
        f"{total} 64-bit packed string compare cases across {len(SPECS)} exports matched native and reference; "
        f"ALTER_IMM diverged on {diverged}; output mutation control caught"
    )


def encode_seq(*instructions) -> bytes:
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_rejections(original_path: Path) -> None:
    ld_xmm0 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    ld_xmm1 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RCX))
    ld_xmm2 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM2, MemoryOperand(R.RDX))
    ld_ymm1 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM1, MemoryOperand(R.RCX))
    ld_ymm2 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM2, MemoryOperand(R.RDX))
    ld_rax = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.R8))
    ld_rdx = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RDX, MemoryOperand(R.R8, displ=8, displ_size=1))
    ret_ins = Instruction.create(Code.RETNQ)
    ld_preamble = encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx)
    ret_bytes = encode_seq(ret_ins)

    evex_ins = Instruction.create_reg_reg_reg(Code.EVEX_VPADDQ_XMM_K1Z_XMM_XMMM128B64, R.XMM0, R.XMM1, R.XMM2)
    evex_ins.op_mask = R.K1
    seg_ins = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RCX))
    seg_ins.segment_prefix = R.GS

    unit_cases = [
        ("undefined_rax", ret_bytes, "return with undefined RAX"),
        # 32-bit explicit-length forms (legacy & VEX.W=0) rejected
        ("legacy_32_pcmpestri", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.PCMPESTRI_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported 32-bit explicit-length packed string compare"),
        ("legacy_32_pcmpestrm", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.PCMPESTRM_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported 32-bit explicit-length packed string compare"),
        ("vex_w0_vpcmpestri", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPESTRI_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported 32-bit explicit-length packed string compare"),
        ("vex_w0_vpcmpestrm", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPESTRM_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported 32-bit explicit-length packed string compare"),
        # Implicit-length forms rejected
        ("legacy_pcmpistri", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_u32(
            Code.PCMPISTRI_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported implicit-length packed string compare"),
        ("legacy_pcmpistrm", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_u32(
            Code.PCMPISTRM_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported implicit-length packed string compare"),
        ("vex_vpcmpistri", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPISTRI_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported implicit-length packed string compare"),
        ("vex_vpcmpistrm", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPISTRM_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported implicit-length packed string compare"),
        # Invalid VEX.256 (L=1) W=1 encodings (#UD in x86-64) and out-of-scope VEX.256
        ("vex256_vpcmpestri64", ld_preamble + bytes.fromhex("c4e3fd61ca00") + ret_bytes, "invalid instruction"),
        ("vex256_vpcmpestrm64", ld_preamble + bytes.fromhex("c4e3fd60ca00") + ret_bytes, "invalid instruction"),
        ("vex256_vpaddq", encode_seq(ld_ymm1, ld_ymm2, Instruction.create_reg_reg_reg(
            Code.VEX_VPADDQ_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2), ret_ins), "unsupported VEX.256 (L=1)"),
        # EVEX
        ("evex_vpaddq", encode_seq(ld_xmm1, ld_xmm2, evex_ins, ret_ins), "unsupported EVEX instruction"),
        # Nonvolatile vector and GPR registers
        ("nonvolatile_xmm6", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_ymm6", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_rbx", encode_seq(Instruction.create_reg_mem(
            Code.MOV_R64_RM64, R.RBX, MemoryOperand(R.R8)), ret_ins), "nonvolatile GPR"),
        ("nonvolatile_rsp", encode_seq(Instruction.create_reg_mem(
            Code.MOV_R64_RM64, R.RSP, MemoryOperand(R.R8)), ret_ins), "nonvolatile GPR"),
        ("write_r8", encode_seq(Instruction.create_reg_i32(
            Code.MOV_R32_IMM32, R.R8D, 1), ret_ins), "unsupported write to output buffer pointer r8"),
        # Undefined registers, uninitialized YMM0 upper bits after legacy pcmpestrm64, undefined flags
        ("undef_xmm3", encode_seq(ld_xmm1, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.PCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, R.XMM3, 0), ret_ins), "undefined vector register ymm3"),
        ("undef_rax_est64", encode_seq(ld_xmm1, ld_xmm2, ld_rdx, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "undefined explicit length register"),
        ("undef_ymm0_hi_legacy", encode_seq(
            ld_xmm1, ld_xmm2, ld_rax, ld_rdx,
            Instruction.create_reg_reg_u32(Code.PCMPESTRM64_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0),
            Instruction.create_mem_reg(Code.VEX_VMOVDQU_YMMM256_YMM, MemoryOperand(R.R8), R.YMM0),
            ret_ins),
         "undefined vector register ymm0"),
        ("undef_flags_setb", encode_seq(Instruction.create_reg(Code.SETB_RM8, R.AL), ret_ins), "undefined flags"),
        # Bad addressing and clobbered pointer reads
        ("rip_relative", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RIP, displ=0x1010, displ_size=1)), ret_ins),
         "unsupported memory operand"),
        ("rsp_base", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RSP)), ret_ins),
         "unsupported memory operand"),
        ("indexed_mem", encode_seq(ld_xmm1, ld_rax, Instruction.create_reg_mem_u32(
            Code.PCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, MemoryOperand(R.RDX, R.RCX, 4), 0), ret_ins),
         "unsupported memory operand"),
        ("non_arg_base", encode_seq(ld_xmm1, ld_rax, Instruction.create_reg_mem_u32(
            Code.VEX_VPCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, MemoryOperand(R.R9), 0), ret_ins),
         "unsupported memory operand"),
        ("clobbered_rdx_mem", encode_seq(ld_xmm1, ld_rax, ld_rdx, Instruction.create_reg_mem_u32(
            Code.PCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, MemoryOperand(R.RDX), 0), ret_ins),
         "memory operand uses clobbered pointer rdx"),
        ("clobbered_rcx_mem", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.PCMPESTRI64_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ld_xmm0, ret_ins),
         "memory operand uses clobbered pointer rcx"),
        # Prefix, out-of-scope, control-flow / ret checks
        ("segment_prefix", encode_seq(seg_ins, ret_ins), "unsupported instruction prefix"),
        ("out_of_scope_vaddps", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_reg(
            Code.VEX_VADDPS_XMM_XMM_XMMM128, R.XMM0, R.XMM1, R.XMM2), ret_ins), "unsupported instruction"),
        ("out_of_scope_jmp", encode_seq(Instruction.create_branch(Code.JMP_REL8_64, 0x1020), ret_ins),
         "unsupported instruction"),
        ("missing_ret", encode_seq(ld_xmm0), "missing terminal ret"),
        ("after_ret", encode_seq(ld_rax, ret_ins, ld_xmm0), "unreachable instruction after ret"),
    ]

    for label, raw, expected_reason in unit_cases:
        try:
            lift_code(label, 0x1000, raw)
        except ValueError as exc:
            if expected_reason not in str(exc):
                raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator unexpectedly accepted rejected control")

    # End-to-end PE patching controls on a copy of the built native DLL.
    pe_bytes = original_path.read_bytes()
    with pefile.PE(data=pe_bytes, fast_load=True) as pe:
        entry = export_rva(pe, ALL_EXPORTS[0])
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

    pe_patches = [
        ("pe_vex_w0_vpcmpestri", encode_seq(ld_xmm1, ld_xmm2, ld_rax, ld_rdx, Instruction.create_reg_reg_u32(
            Code.VEX_VPCMPESTRI_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported 32-bit explicit-length packed string compare"),
        ("pe_legacy_pcmpistrm", encode_seq(ld_xmm1, ld_xmm2, Instruction.create_reg_reg_u32(
            Code.PCMPISTRM_XMM_XMMM128_IMM8, R.XMM1, R.XMM2, 0), ret_ins),
         "unsupported implicit-length packed string compare"),
        ("pe_evex_vpaddq", encode_seq(ld_xmm1, ld_xmm2, evex_ins, ret_ins), "unsupported EVEX instruction"),
        ("pe_nonvolatile_xmm6", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("pe_missing_ret", encode_seq(ld_xmm0), "missing terminal ret"),
    ]
    with TemporaryDirectory() as folder:
        patched_path = Path(folder) / "patched.dll"
        for label, patch, expected_reason in pe_patches:
            if len(patch) > max_len:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds export size ({max_len})")
            damaged = bytearray(pe_bytes)
            damaged[file_off : file_off + len(patch)] = patch
            struct.pack_into("<I", damaged, pdata_end_off, entry + len(patch))
            patched_path.write_bytes(damaged)
            try:
                translate(patched_path, [ALL_EXPORTS[0]])
            except ValueError as exc:
                if expected_reason not in str(exc):
                    raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
            else:
                raise AssertionError(f"{label}: PE translate accepted rejected patch")

    print(f"{len(unit_cases)} unit rejection controls and {len(pe_patches)} PE-patched rejection controls passed")


if __name__ == "__main__":
    if os.environ.get("FORM_COVERAGE_RECORD"):
        Path(os.environ["FORM_COVERAGE_RECORD"]).unlink(missing_ok=True)
    orig, lifted, alt = (Path(p) for p in sys.argv[1:4])
    check_rejections(orig)
    check_differential(orig, lifted, alt)
