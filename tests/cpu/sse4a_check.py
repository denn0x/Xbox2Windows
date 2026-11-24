"""Differential checks, ALTER_IMM/mutation controls and rejection controls for SSE4a AOT.

Verification always runs on both Intel and AMD hosts without skipping:
  - Lifted exports (cpu_sse4a_original_lifted) are compared bit-for-bit against both
    an independent Python AMD64 APM reference model and Win64 MASM baseline ref_*
    exports (cpu_sse4a_original).
  - When CPUID.80000001H:ECX[6] reports native SSE4a support, the native op_*
    exports in cpu_sse4a_original are also executed and cross-checked bit-for-bit.
"""

import ctypes
import os
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import (
    Code,
    Decoder,
    Encoder,
    Formatter,
    FormatterSyntax,
    Instruction,
    MemoryOperand,
    Register as R,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import bounded_code, export_rva
from cpu_sse4a_aot import lift_code, translate

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
GUARD_PRE = bytes(0xA0 + (i % 16) for i in range(32))
GUARD_POST = bytes(0xC0 + (i % 16) for i in range(32))
FILL = b"\x5A" * 32

DIFF_EXPORTS = (
    "extrq_imm_basic",
    "extrq_imm_zero_len",
    "extrq_imm_high_bit",
    "extrq_imm_mid",
    "extrq_imm_bits76",
    "extrq_reg_basic",
    "extrq_reg_self",
    "extrq_reg_xmm4_xmm5",
    "insertq_imm_basic",
    "insertq_imm_zero_len",
    "insertq_imm_high_bit",
    "insertq_imm_self",
    "insertq_reg_basic",
    "insertq_reg_self",
    "insertq_reg_xmm4_xmm5",
    "movntsd_aligned",
    "movntsd_disp8",
    "movntsd_unaligned",
    "movntss_aligned",
    "movntss_disp12",
    "movntss_unaligned",
    "pipeline",
)

OP_EXPORTS = tuple(f"op_{name}" for name in DIFF_EXPORTS)
REF_EXPORTS = tuple(f"ref_{name}" for name in DIFF_EXPORTS)
TRANSLATED_EXPORTS = OP_EXPORTS + ("op_discarded_read", "op_sse4a_discarded_read")

ALTERED_EXPORTS = (
    "op_extrq_imm_basic",
    "op_extrq_imm_zero_len",
    "op_insertq_imm_basic",
    "op_insertq_imm_zero_len",
    "op_movntsd_disp8",
    "op_movntss_disp12",
)


# --- Independent Python AMD64 APM reference model ---

def u64(buf):
    return struct.unpack("<Q", buf)[0]


def pk_q(val):
    return struct.pack("<Q", val & MASK64)


def pk_d(val):
    return struct.pack("<I", val & MASK32)


def apm_extrq(val, len_imm, idx_imm):
    """AMD64 APM Vol. 4 EXTRQ bit-field extraction."""
    width = (len_imm & 0x3F) or 64
    offset = idx_imm & 0x3F
    assert offset + width <= 64, (width, offset)
    mask = (1 << width) - 1
    return (val >> offset) & mask


def apm_insertq(dst, src, len_imm, idx_imm):
    """AMD64 APM Vol. 4 INSERTQ bit-field insertion."""
    width = (len_imm & 0x3F) or 64
    offset = idx_imm & 0x3F
    assert offset + width <= 64, (width, offset)
    mask = (1 << width) - 1
    return ((dst & ~(mask << offset)) | ((src & mask) << offset)) & MASK64


def reference(export_name, a, b, altered=False):
    """Expected 32-byte output region for an op_* or ref_* export."""
    stem = export_name[3:] if export_name.startswith(("op_", "ref_")) else export_name
    out = bytearray(FILL)

    if stem == "extrq_imm_basic":
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), 12, 24 if altered else 20))
    elif stem == "extrq_imm_zero_len":
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), 63 if altered else 0, 0))
    elif stem == "extrq_imm_high_bit":
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), 1, 63))
    elif stem == "extrq_imm_mid":
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), 16, 48))
    elif stem == "extrq_imm_bits76":
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), 0x48, 0x05))
    elif stem == "extrq_reg_basic":
        ctrl = u64(b[0:8])
        out[0:8] = pk_q(apm_extrq(u64(a[0:8]), ctrl & 0xFF, (ctrl >> 8) & 0xFF))
    elif stem == "extrq_reg_self":
        val = u64(a[0:8])
        out[0:8] = pk_q(apm_extrq(val, val & 0xFF, (val >> 8) & 0xFF))
    elif stem == "extrq_reg_xmm4_xmm5":
        ctrl = u64(b[1:9])
        out[0:8] = pk_q(apm_extrq(u64(a[16:24]), ctrl & 0xFF, (ctrl >> 8) & 0xFF))
    elif stem == "insertq_imm_basic":
        out[0:8] = pk_q(apm_insertq(u64(a[0:8]), u64(b[0:8]), 16, 20 if altered else 16))
    elif stem == "insertq_imm_zero_len":
        out[0:8] = pk_q(apm_insertq(u64(a[0:8]), u64(b[0:8]), 63 if altered else 0, 0))
    elif stem == "insertq_imm_high_bit":
        out[0:8] = pk_q(apm_insertq(u64(a[0:8]), u64(b[0:8]), 1, 63))
    elif stem == "insertq_imm_self":
        val = u64(a[0:8])
        out[0:8] = pk_q(apm_insertq(val, val, 12, 36))
    elif stem == "insertq_reg_basic":
        ctrl = u64(b[8:16])
        out[0:8] = pk_q(apm_insertq(u64(a[0:8]), u64(b[0:8]), ctrl & 0xFF, (ctrl >> 8) & 0xFF))
    elif stem == "insertq_reg_self":
        val = u64(a[0:8])
        ctrl = u64(a[8:16])
        out[0:8] = pk_q(apm_insertq(val, val, ctrl & 0xFF, (ctrl >> 8) & 0xFF))
    elif stem == "insertq_reg_xmm4_xmm5":
        ctrl = u64(b[9:17])
        out[0:8] = pk_q(apm_insertq(u64(a[16:24]), u64(b[1:9]), ctrl & 0xFF, (ctrl >> 8) & 0xFF))
    elif stem == "movntsd_aligned":
        out[0:8] = a[0:8]
    elif stem == "movntsd_disp8":
        if altered:
            out[16:24] = b[0:8]
        else:
            out[8:16] = b[0:8]
    elif stem == "movntsd_unaligned":
        out[1:9] = a[3:11]
    elif stem == "movntss_aligned":
        out[0:4] = a[0:4]
    elif stem == "movntss_disp12":
        if altered:
            out[16:20] = b[0:4]
        else:
            out[12:16] = b[0:4]
    elif stem == "movntss_unaligned":
        out[3:7] = a[5:9]
    elif stem == "pipeline":
        xmm0 = apm_extrq(u64(a[0:8]), 16, 8)
        xmm2 = apm_insertq(u64(a[16:24]), xmm0, 16, 24)
        ctrl_ins = u64(b[8:16])
        xmm2 = apm_insertq(xmm2, u64(b[0:8]), ctrl_ins & 0xFF, (ctrl_ins >> 8) & 0xFF)
        ctrl_ext = u64(b[0:8])
        xmm3 = apm_extrq(u64(b[16:24]), ctrl_ext & 0xFF, (ctrl_ext >> 8) & 0xFF)
        out[0:8] = pk_q(xmm2)
        out[8:16] = pk_q(xmm3)
        out[16:20] = pk_d(xmm0)
        out[21:25] = pk_d(xmm2)
    else:
        raise ValueError(f"unknown export {export_name}")
    return bytes(out)


# --- Test vector generators (strictly defined offset + width <= 64 for reg forms) ---

DEFINED_FIELDS = (
    (0, 0),      # width=64, offset=0
    (0x40, 0),   # bits[5:0]==0 -> width=64, bits[7:6] non-zero ignored
    (1, 0),
    (1, 63),
    (8, 56),
    (16, 0),
    (16, 16),
    (16, 48),
    (32, 0),
    (32, 32),
    (63, 1),
    (5, 11),
    (0xC5, 0x8B),  # bits[7:6] & [15:14] ignored -> width=5, offset=11
)

VALUES64 = (
    0x0123456789ABCDEF,
    0xFEDCBA9876543210,
    0x0000000000000000,
    0xFFFFFFFFFFFFFFFF,
    0x8000000000000001,
    0x5555AAAA5555AAAA,
    0xAAAA5555AAAA5555,
    0x1122334455667788,
)


def make_ctrl(width_byte, offset_byte, garbage_hi):
    return ((garbage_hi & 0xFFFFFFFFFFFF) << 16) | ((offset_byte & 0xFF) << 8) | (width_byte & 0xFF)


def pairs_for(export_name):
    rng = random.Random(0x55E4A)
    stem = export_name[3:]

    if stem == "extrq_reg_self":
        pairs = []
        for idx, (w, o) in enumerate(DEFINED_FIELDS):
            for hi in (0, 0x123456789ABC, 0xFFFFFFFFFFFF):
                val = make_ctrl(w, o, hi ^ (VALUES64[idx % len(VALUES64)] >> 16))
                pairs.append((pk_q(val) + os_pad(24), b"\xAA" * 32))
        return pairs

    if stem == "insertq_reg_self":
        pairs = []
        for idx, (w, o) in enumerate(DEFINED_FIELDS):
            val = VALUES64[idx % len(VALUES64)]
            ctrl = make_ctrl(w, o, 0xA5A55A5A1234 ^ idx)
            pairs.append((pk_q(val) + pk_q(ctrl) + os_pad(16), b"\x55" * 32))
        return pairs

    if stem == "extrq_reg_basic":
        pairs = []
        for w, o in DEFINED_FIELDS:
            for a_val in VALUES64[:4]:
                ctrl = make_ctrl(w, o, rng.getrandbits(48))
                pairs.append((pk_q(a_val) + os_pad(24), pk_q(ctrl) + os_pad(24)))
        return pairs

    if stem == "extrq_reg_xmm4_xmm5":
        pairs = []
        for w, o in DEFINED_FIELDS:
            for a_val in VALUES64[:4]:
                ctrl = make_ctrl(w, o, rng.getrandbits(48))
                a = os_pad(16) + pk_q(a_val) + os_pad(8)
                b = b"\xCC" + pk_q(ctrl) + os_pad(23)
                pairs.append((a, b))
        return pairs

    if stem == "insertq_reg_basic":
        pairs = []
        for w, o in DEFINED_FIELDS:
            for a_val, b_val in zip(VALUES64, reversed(VALUES64)):
                ctrl = make_ctrl(w, o, rng.getrandbits(48))
                a = pk_q(a_val) + os_pad(24)
                b = pk_q(b_val) + pk_q(ctrl) + os_pad(16)
                pairs.append((a, b))
        return pairs

    if stem == "insertq_reg_xmm4_xmm5":
        pairs = []
        for w, o in DEFINED_FIELDS:
            for a_val, b_val in zip(VALUES64[:4], reversed(VALUES64[:4])):
                ctrl = make_ctrl(w, o, rng.getrandbits(48))
                a = os_pad(16) + pk_q(a_val) + os_pad(8)
                b = b"\xDD" + pk_q(b_val) + pk_q(ctrl) + os_pad(15)
                pairs.append((a, b))
        return pairs

    if stem == "pipeline":
        pairs = []
        for i, (w1, o1) in enumerate(DEFINED_FIELDS):
            w2, o2 = DEFINED_FIELDS[(i * 5 + 3) % len(DEFINED_FIELDS)]
            a_lo = VALUES64[i % len(VALUES64)]
            a_hi = VALUES64[(i + 3) % len(VALUES64)]
            b_lo = make_ctrl(w1, o1, rng.getrandbits(48))
            b_hi = make_ctrl(w2, o2, rng.getrandbits(48))
            b_ext_src = VALUES64[(i + 5) % len(VALUES64)]
            a = pk_q(a_lo) + os_pad(8) + pk_q(a_hi) + os_pad(8)
            b = pk_q(b_lo) + pk_q(b_hi) + pk_q(b_ext_src) + os_pad(8)
            pairs.append((a, b))
        return pairs

    # Immediate extrq/insertq and movntsd/movntss forms.
    pairs = [
        (bytes(range(0x10, 0x30)), bytes(range(0x80, 0xA0))),
        (b"\x00" * 32, b"\xFF" * 32),
        (b"\xFF" * 32, b"\x00" * 32),
        (pk_q(VALUES64[0]) * 4, pk_q(VALUES64[1]) * 4),
        (pk_q(VALUES64[4]) * 4, pk_q(VALUES64[5]) * 4),
    ]
    for _ in range(12):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def os_pad(n):
    return bytes((0x37 + i * 17) & 0xFF for i in range(n))


# --- Guarded buffer helpers ---

def alloc_aligned_buffer(payload, align=16):
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
    buf[post_off:post_off + len(GUARD_POST)] = GUARD_POST
    return buf, base_addr + payload_off, pre_off, payload_off, post_off


def check_guards_and_read(buf, pre_off, payload_off, post_off, label):
    raw = buf.raw
    if raw[pre_off:payload_off] != GUARD_PRE or raw[post_off:post_off + len(GUARD_POST)] != GUARD_POST:
        raise AssertionError(f"guard bytes corrupted around {label}")
    return raw[payload_off:post_off]


def invoke(fn, a, b):
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(a)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(b)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL)
    fn(ptr_a, ptr_b, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "input_a")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "input_b")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "output")


def assert_equal_output(label, name, idx, got, want):
    if got != want:
        raise AssertionError(
            f"{label} {name} case {idx}: got {got.hex()} != expected {want.hex()}"
        )


def bind_exports(dll_path, prefix="", names=OP_EXPORTS):
    lib = ctypes.CDLL(str(Path(dll_path).resolve()))
    funcs = {}
    for name in names:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = None
        funcs[name] = fn
    return lib, funcs


def record_covered_forms(original_path):
    record = os.environ.get("FORM_COVERAGE_RECORD")
    if not record:
        return
    formatter = Formatter(FormatterSyntax.INTEL)
    forms = set()
    with pefile.PE(str(original_path), fast_load=True) as pe:
        for name in TRANSLATED_EXPORTS:
            entry = export_rva(pe, name)
            raw = bounded_code(pe, entry)
            for ins in Decoder(64, raw, ip=entry):
                forms.add(formatter.format_mnemonic(ins).lower())
    out_path = Path(record)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("".join(word + "\n" for word in sorted(forms)), encoding="utf-8")


# --- Discarded-read fault ordering ---

def check_discarded_read(native_path, lifted_path, native_sse4a):
    payload = bytes(range(32))

    # Universal op_discarded_read runs natively and lifted on both Intel and AMD.
    for label, dll, prefix in (("native", native_path, ""), ("lifted", lifted_path, "lifted_")):
        _, fns = bind_exports(dll, prefix, ("op_discarded_read",))
        fn = fns["op_discarded_read"]
        source, ptr, _, _, _ = alloc_aligned_buffer(payload)
        for readable in (True, False):
            result, result_ptr, pre, off, post = alloc_aligned_buffer(FILL)
            try:
                fn(ptr, ptr if readable else 0, result_ptr)
            except OSError as error:
                assert not readable and "access violation reading" in str(error).lower(), (label, error)
            else:
                assert readable, f"{label} discarded a faulting read"
            expected = payload[:16] + (payload[:16] if readable else FILL[16:])
            assert_equal_output(label, "op_discarded_read", readable,
                                check_guards_and_read(result, pre, off, post, label), expected)

    # SSE4a dead-read export always runs on lifted, and runs on native when host supports SSE4a.
    targets = [("lifted", lifted_path, "lifted_")]
    if native_sse4a:
        targets.insert(0, ("native", native_path, ""))
    for label, dll, prefix in targets:
        _, fns = bind_exports(dll, prefix, ("op_sse4a_discarded_read",))
        fn = fns["op_sse4a_discarded_read"]
        source, ptr, _, _, _ = alloc_aligned_buffer(payload)
        for readable in (True, False):
            result, result_ptr, pre, off, post = alloc_aligned_buffer(FILL)
            try:
                fn(ptr, ptr if readable else 0, result_ptr)
            except OSError as error:
                assert not readable and "access violation reading" in str(error).lower(), (label, error)
            else:
                assert readable, f"{label} discarded a faulting SSE4a read"
            expected = payload[:8] + (payload[:4] if readable else FILL[8:12]) + FILL[12:]
            assert_equal_output(label, "op_sse4a_discarded_read", readable,
                                check_guards_and_read(result, pre, off, post, label), expected)


# --- Differential verification & altered/mutation controls ---

def check_differential(native_path, lifted_path, altered_path):
    lifted_lib, lifted = bind_exports(lifted_path, "lifted_", OP_EXPORTS)
    lifted_lib.host_has_sse4a.argtypes = []
    lifted_lib.host_has_sse4a.restype = ctypes.c_uint32
    native_sse4a = bool(lifted_lib.host_has_sse4a())

    check_discarded_read(native_path, lifted_path, native_sse4a)

    _, ref_fns = bind_exports(native_path, "", REF_EXPORTS)
    _, native_ops = bind_exports(native_path, "", OP_EXPORTS) if native_sse4a else (None, None)
    _, altered = bind_exports(altered_path, "lifted_", ALTERED_EXPORTS)

    total = 0
    for op_name, ref_name in zip(OP_EXPORTS, REF_EXPORTS):
        for idx, (a, b) in enumerate(pairs_for(op_name)):
            want = reference(op_name, a, b)
            got_l = invoke(lifted[op_name], a, b)
            got_r = invoke(ref_fns[ref_name], a, b)
            assert_equal_output("lifted", op_name, idx, got_l, want)
            assert_equal_output("masm_ref", ref_name, idx, got_r, want)
            if native_ops is not None:
                got_n = invoke(native_ops[op_name], a, b)
                assert_equal_output("native_sse4a", op_name, idx, got_n, want)
            total += 1

    changed = {}
    for name in ALTERED_EXPORTS:
        diffs = 0
        for idx, (a, b) in enumerate(pairs_for(name)):
            want_alt = reference(name, a, b, altered=True)
            want_orig = reference(name, a, b)
            got_alt = invoke(altered[name], a, b)
            assert_equal_output("altered", name, idx, got_alt, want_alt)
            if got_alt != want_orig:
                diffs += 1
        if not diffs:
            raise AssertionError(f"altered control {name} did not diverge from original")
        changed[name] = diffs

    sample_a, sample_b = pairs_for("op_pipeline")[0]
    expected = reference("op_pipeline", sample_a, sample_b)
    mutated = bytearray(expected)
    mutated[5] ^= 0x01
    try:
        assert_equal_output("mutation", "op_pipeline", 0, bytes(mutated), expected)
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    mode_note = "native SSE4a + MASM ref + lifted + APM" if native_sse4a else "MASM ref + lifted + APM (host lacks CPUID SSE4a)"
    print(f"{total} cases across {len(OP_EXPORTS)} exports matched bit-for-bit ({mode_note}); "
          f"ALTER_IMM diverged on all {len(changed)} exports (min diffs {min(changed.values())}); "
          f"discarded-read and output-mutation controls passed")


# --- Alternate-encoding acceptance & strict rejection controls ---

def encode_seq(*instructions):
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_alt_encodings_and_rejections(original_path):
    ld_xmm0 = Instruction.create_reg_mem(Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    ld_xmm1 = Instruction.create_reg_mem(Code.MOVAPS_XMM_XMMM128, R.XMM1, MemoryOperand(R.RDX))
    ld_xmm4 = Instruction.create_reg_mem(Code.MOVDQA_XMM_XMMM128, R.XMM4, MemoryOperand(R.RCX))
    ld_xmm5 = Instruction.create_reg_mem(Code.MOVDQU_XMM_XMMM128, R.XMM5, MemoryOperand(R.RDX, displ=3, displ_size=1))
    st_xmm0 = Instruction.create_mem_reg(Code.MOVAPS_XMMM128_XMM, MemoryOperand(R.R8), R.XMM0)
    ret_ins = Instruction.create(Code.RETNQ)

    alt_accept = [
        ("extrq_imm_xmm4_64", (ld_xmm4, Instruction.create_reg_u32_u32(
            Code.EXTRQ_XMM_IMM8_IMM8, R.XMM4, 0x40, 0),
            Instruction.create_mem_reg(Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8), R.XMM4), ret_ins)),
        ("extrq_reg_xmm5_xmm4", (ld_xmm4, ld_xmm5, Instruction.create_reg_reg(
            Code.EXTRQ_XMM_XMM, R.XMM5, R.XMM4),
            Instruction.create_mem_reg(Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8, displ=7, displ_size=1), R.XMM5),
            ret_ins)),
        ("insertq_imm_xmm4_xmm5", (ld_xmm4, ld_xmm5, Instruction.create_reg_reg_u32_u32(
            Code.INSERTQ_XMM_XMM_IMM8_IMM8, R.XMM4, R.XMM5, 8, 56),
            Instruction.create_mem_reg(Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8, displ=-3, displ_size=1), R.XMM4),
            ret_ins)),
        ("insertq_reg_xmm4_xmm5", (ld_xmm4, ld_xmm5, Instruction.create_reg_reg(
            Code.INSERTQ_XMM_XMM, R.XMM4, R.XMM5),
            Instruction.create_mem_reg(Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8), R.XMM4), ret_ins)),
        ("movdqu_r8_source", (Instruction.create_reg_mem(
            Code.MOVDQU_XMM_XMMM128, R.XMM0, MemoryOperand(R.R8, displ=1, displ_size=1)),
            Instruction.create_mem_reg(Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8, displ=9, displ_size=1), R.XMM0),
            ret_ins)),
        ("movaps_alt_reg_reg", (ld_xmm0, Instruction.create_reg_reg(
            Code.MOVAPS_XMMM128_XMM, R.XMM2, R.XMM0),
            Instruction.create_mem_reg(Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8), R.XMM2), ret_ins)),
    ]
    alt_expected = {
        "extrq_imm_xmm4_64": "xmm4.q[0] = op_extrq_imm(xmm4.q[0], 0x40u, 0x0u);",
        "extrq_reg_xmm5_xmm4": "xmm5.q[0] = op_extrq_reg(xmm5.q[0], xmm4.q[0]);",
        "insertq_imm_xmm4_xmm5": "xmm4.q[0] = op_insertq_imm(xmm4.q[0], xmm5.q[0], 0x8u, 0x38u);",
        "insertq_reg_xmm4_xmm5": "xmm4.q[0] = op_insertq_reg(xmm4.q[0], xmm5);",
        "movdqu_r8_source": "load_always((r8 + 0x1ull))",
        "movaps_alt_reg_reg": "xmm2 = xmm0;",
    }
    for label, seq in alt_accept:
        raw = encode_seq(*seq)
        decoded = list(Decoder(64, raw, ip=0x1000))
        assert len(decoded) == len(seq) and not any(ins.is_invalid for ins in decoded), label
        src, count = lift_code(label, 0x1000, raw)
        assert f"lifted_{label}" in src and count == len(seq), (label, count)
        if alt_expected[label] not in src:
            raise AssertionError(
                f"alternate encoding {label} lowered without expected {alt_expected[label]!r}")

    # 1) Immediate offset + width > 64 rejections.
    oob_cases = [
        ("extrq_imm_oob_32_33", (ld_xmm0, Instruction.create_reg_u32_u32(
            Code.EXTRQ_XMM_IMM8_IMM8, R.XMM0, 32, 33), ret_ins), "undefined SSE4a bitfield"),
        ("extrq_imm_oob_0_1", (ld_xmm0, Instruction.create_reg_u32_u32(
            Code.EXTRQ_XMM_IMM8_IMM8, R.XMM0, 0, 1), ret_ins), "undefined SSE4a bitfield"),
        ("extrq_imm_oob_40_1", (ld_xmm0, Instruction.create_reg_u32_u32(
            Code.EXTRQ_XMM_IMM8_IMM8, R.XMM0, 0x40, 1), ret_ins), "undefined SSE4a bitfield"),
        ("insertq_imm_oob_48_48", (ld_xmm0, ld_xmm1, Instruction.create_reg_reg_u32_u32(
            Code.INSERTQ_XMM_XMM_IMM8_IMM8, R.XMM0, R.XMM1, 48, 48), ret_ins), "undefined SSE4a bitfield"),
        ("insertq_imm_oob_0_1", (ld_xmm0, ld_xmm1, Instruction.create_reg_reg_u32_u32(
            Code.INSERTQ_XMM_XMM_IMM8_IMM8, R.XMM0, R.XMM1, 0, 1), ret_ins), "undefined SSE4a bitfield"),
    ]

    # 2) Non-legacy VEX and EVEX encodings.
    nonlegacy_cases = [
        ("vmovaps_vex", (Instruction.create_reg_mem(
            Code.VEX_VMOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovdqu_vex", (Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovntps_vex", (ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVNTPS_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins), "non-legacy encoding"),
        ("vmovaps_evex", (Instruction.create_reg_mem(
            Code.EVEX_VMOVAPS_XMM_K1Z_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovntps_evex", (ld_xmm0, Instruction.create_mem_reg(
            Code.EVEX_VMOVNTPS_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins), "non-legacy encoding"),
    ]

    # 3) Unsupported instructions.
    unsupported_insns = [
        ("movntps", (ld_xmm0, Instruction.create_mem_reg(Code.MOVNTPS_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins)),
        ("movntpd", (ld_xmm0, Instruction.create_mem_reg(Code.MOVNTPD_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins)),
        ("movss", (ld_xmm0, ld_xmm1, Instruction.create_reg_reg(Code.MOVSS_XMM_XMMM32, R.XMM0, R.XMM1), ret_ins)),
        ("movsd", (ld_xmm0, ld_xmm1, Instruction.create_reg_reg(Code.MOVSD_XMM_XMMM64, R.XMM0, R.XMM1), ret_ins)),
        ("addps", (ld_xmm0, ld_xmm1, Instruction.create_reg_reg(Code.ADDPS_XMM_XMMM128, R.XMM0, R.XMM1), ret_ins)),
        ("pextrq", (ld_xmm0, Instruction.create_reg_reg_u32(Code.PEXTRQ_RM64_XMM_IMM8, R.RAX, R.XMM0, 0), ret_ins)),
        ("mov_gpr", (Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RBX, R.RCX), ret_ins)),
    ]

    # 4) Nonvolatile registers, unowned stores, undefined reads, alignment, prefixes, stream checks.
    seg_load = Instruction.create_reg_mem(Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    seg_load.segment_prefix = R.GS
    other_cases = [
        ("nonvolatile_xmm6_extrq_imm", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm8_extrq_reg", (ld_xmm0, Instruction.create_reg_reg(
            Code.EXTRQ_XMM_XMM, R.XMM0, R.XMM8), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm7_insertq_imm", (ld_xmm0, Instruction.create_reg_reg_u32_u32(
            Code.INSERTQ_XMM_XMM_IMM8_IMM8, R.XMM0, R.XMM7, 8, 8), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm10_insertq_reg", (ld_xmm0, Instruction.create_reg_reg(
            Code.INSERTQ_XMM_XMM, R.XMM0, R.XMM10), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm9_movntsd", (Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8), R.XMM9), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm15_movntss", (Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8), R.XMM15), ret_ins), "nonvolatile vector register"),
        ("undefined_extrq_imm", (Instruction.create_reg_u32_u32(
            Code.EXTRQ_XMM_IMM8_IMM8, R.XMM0, 8, 8), ret_ins), "undefined vector register xmm0"),
        ("undefined_extrq_reg_src", (ld_xmm0, Instruction.create_reg_reg(
            Code.EXTRQ_XMM_XMM, R.XMM0, R.XMM3), ret_ins), "undefined vector register xmm3"),
        ("undefined_insertq_imm_dst", (ld_xmm1, Instruction.create_reg_reg_u32_u32(
            Code.INSERTQ_XMM_XMM_IMM8_IMM8, R.XMM0, R.XMM1, 8, 8), ret_ins), "undefined vector register xmm0"),
        ("undefined_insertq_reg_src", (ld_xmm0, Instruction.create_reg_reg(
            Code.INSERTQ_XMM_XMM, R.XMM0, R.XMM2), ret_ins), "undefined vector register xmm2"),
        ("undefined_movntsd_src", (Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8), R.XMM4), ret_ins), "undefined vector register xmm4"),
        ("undefined_movntss_src", (Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8), R.XMM5), ret_ins), "undefined vector register xmm5"),
        ("non_r8_store_movntsd_rcx", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.RCX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("non_r8_store_movntsd_rdx", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.RDX, displ=8, displ_size=1), R.XMM0), ret_ins),
            "non-r8 store memory operand"),
        ("non_r8_store_movntss_rcx", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.RCX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("non_r8_store_movntss_rdx", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.RDX, displ=4, displ_size=1), R.XMM0), ret_ins),
            "non-r8 store memory operand"),
        ("nonvolatile_gpr_rbx", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RBX)), ret_ins), "nonvolatile GPR base"),
        ("nonvolatile_gpr_r12_movntsd", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.R12), R.XMM0), ret_ins), "nonvolatile GPR base"),
        ("nonvolatile_gpr_rsi_movntss", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.RSI), R.XMM0), ret_ins), "nonvolatile GPR base"),
        ("non_arg_base_rax", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RAX)), ret_ins), "unsupported memory base"),
        ("non_arg_base_r9_movntsd", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.R9), R.XMM0), ret_ins), "unsupported memory base"),
        ("rip_rel_movntsd", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.RIP, displ=0x1010, displ_size=1), R.XMM0), ret_ins),
            "unsupported memory operand"),
        ("rsp_movntss", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.RSP), R.XMM0), ret_ins), "unsupported memory operand"),
        ("addr32_movntss", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSS_M32_XMM, MemoryOperand(R.R8D), R.XMM0), ret_ins), "unsupported memory operand"),
        ("indexed_movntsd", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVNTSD_M64_XMM, MemoryOperand(R.R8, R.RCX, 4), R.XMM0), ret_ins),
            "unsupported memory operand"),
        ("unaligned_movaps_m128", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, displ=1, displ_size=1)), ret_ins),
            "unaligned 16-byte memory operand"),
        ("segment_prefix", (seg_load, ret_ins), "unsupported instruction prefix"),
        ("missing_ret", (ld_xmm0, st_xmm0), "missing terminal ret"),
        ("after_ret", (ret_ins, ld_xmm0), "unreachable instruction after ret"),
    ]

    raw_cases = [
        ("extrq_slash1", bytes.fromhex("660f78c81010") + encode_seq(ret_ins), "invalid instruction"),
        ("movntsd_reg_invalid", bytes.fromhex("f20f2bc0") + encode_seq(ret_ins), "invalid instruction"),
        ("movntss_reg_invalid", bytes.fromhex("f30f2bc0") + encode_seq(ret_ins), "invalid instruction"),
        ("lock_cmpxchg", bytes.fromhex("f00fb109") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("rep_movsb", bytes.fromhex("f3a4") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("invalid_ud_bytes", bytes.fromhex("fffe") + encode_seq(ret_ins), "invalid instruction"),
        ("truncated_insn", encode_seq(ld_xmm0)[:-1], "invalid instruction"),
        ("empty_stream", b"", "truncated instruction stream"),
    ]

    all_unit = (
        oob_cases
        + nonlegacy_cases
        + [(lbl, seq, "unsupported instruction") for lbl, seq in unsupported_insns]
        + other_cases
    )

    for label, seq, expected_reason in all_unit:
        raw = encode_seq(*seq)
        try:
            lift_code(label, 0x1000, raw)
        except ValueError as exc:
            if expected_reason not in str(exc):
                raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator unexpectedly accepted rejected control")

    for label, raw, expected_reason in raw_cases:
        try:
            lift_code(label, 0x1000, raw)
        except ValueError as exc:
            if expected_reason not in str(exc):
                raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator unexpectedly accepted rejected control")

    # End-to-end PE patching controls on a copy of the built native DLL.
    pe_bytes = Path(original_path).read_bytes()
    with pefile.PE(data=pe_bytes, fast_load=True) as pe:
        entry = export_rva(pe, "op_pipeline")
        file_off = pe.get_offset_from_rva(entry)
        pdata = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        records = pe.get_data(pdata.VirtualAddress, pdata.Size)
        rec_idx = next(i for i in range(0, len(records), 12)
                       if struct.unpack_from("<I", records, i)[0] == entry)
        pdata_end_off = pe.get_offset_from_rva(pdata.VirtualAddress + rec_idx + 4)
        orig_end = struct.unpack_from("<I", records, rec_idx + 4)[0]
        max_len = orig_end - entry

    other_sequences = {label: seq for label, seq, _ in other_cases}
    pe_patches = [
        ("pe_extrq_oob", encode_seq(*oob_cases[0][1]), "undefined SSE4a bitfield"),
        ("pe_insertq_oob", encode_seq(*oob_cases[3][1]), "undefined SSE4a bitfield"),
        ("pe_extrq_slash1", raw_cases[0][1], "invalid instruction"),
        ("pe_vmovaps_vex", encode_seq(*nonlegacy_cases[0][1]), "non-legacy encoding"),
        ("pe_movntps", encode_seq(*unsupported_insns[0][1]), "unsupported instruction"),
        ("pe_nonvolatile_xmm6", encode_seq(*other_sequences["nonvolatile_xmm6_extrq_imm"]),
            "nonvolatile vector register"),
        ("pe_nonvolatile_rbx", encode_seq(*other_sequences["nonvolatile_gpr_rbx"]), "nonvolatile GPR base"),
        ("pe_non_r8_movntsd", encode_seq(*other_sequences["non_r8_store_movntsd_rcx"]),
            "non-r8 store memory operand"),
        ("pe_non_r8_movntss", encode_seq(*other_sequences["non_r8_store_movntss_rdx"]),
            "non-r8 store memory operand"),
        ("pe_undef_insertq", encode_seq(*other_sequences["undefined_insertq_imm_dst"]),
            "undefined vector register xmm0"),
        ("pe_missing_ret", encode_seq(ld_xmm0, st_xmm0), "missing terminal ret"),
    ]
    with TemporaryDirectory() as folder:
        patched_path = Path(folder) / "patched.dll"
        for label, patch, expected_reason in pe_patches:
            if len(patch) > max_len:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds op_pipeline ({max_len})")
            damaged = bytearray(pe_bytes)
            damaged[file_off:file_off + len(patch)] = patch
            struct.pack_into("<I", damaged, pdata_end_off, entry + len(patch))
            patched_path.write_bytes(damaged)
            try:
                translate(patched_path, ["op_pipeline"])
            except ValueError as exc:
                if expected_reason not in str(exc):
                    raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
            else:
                raise AssertionError(f"{label}: PE translate accepted rejected patch")

    unit_total = len(all_unit) + len(raw_cases)
    print(f"{len(alt_accept)} alternate-encoding lift checks, {unit_total} unit rejection controls "
          f"({len(oob_cases)} bitfield OOB, {len(nonlegacy_cases)} VEX/EVEX, "
          f"{len(unsupported_insns)} unsupported instructions, {len(other_cases) + len(raw_cases)} "
          f"ABI/state/addressing/prefix/extrq-slash1/stream) and {len(pe_patches)} PE-patched rejection controls passed")


if __name__ == "__main__":
    if os.environ.get("FORM_COVERAGE_RECORD"):
        Path(os.environ["FORM_COVERAGE_RECORD"]).unlink(missing_ok=True)
    check_differential(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
    check_alt_encodings_and_rejections(Path(sys.argv[1]))
    record_covered_forms(Path(sys.argv[1]))
