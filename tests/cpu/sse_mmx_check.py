"""Differential checks, mutation controls and rejection controls for legacy SSE/MMX AOT."""

import ctypes
from ctypes import wintypes
import math
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Decoder, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import export_rva
from cpu_sse_mmx_aot import lift_code, translate

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
INDEFINITE_I32 = 0x80000000
GUARD_PRE = bytes(0xA0 + (i % 16) for i in range(32))
GUARD_POST = bytes(0xC0 + (i % 16) for i in range(32))
FILL = b"\x5A" * 32

EXPORTS = (
    # cvtpi2ps (3)
    "op_cvtpi2ps_reg", "op_cvtpi2ps_mem", "op_cvtpi2ps_mem_plus1",
    # cvtps2pi (3)
    "op_cvtps2pi_reg", "op_cvtps2pi_mem", "op_cvtps2pi_mem_plus1",
    # cvttps2pi (2)
    "op_cvttps2pi_reg", "op_cvttps2pi_mem",
    # pshufw (6)
    "op_pshufw_reg_e4", "op_pshufw_reg_1b", "op_pshufw_reg_00",
    "op_pshufw_reg_ff", "op_pshufw_mem_4e", "op_pshufw_mem_39",
    # movntq, GPR bridge & chained pipeline (4)
    "op_movntq", "op_movntq_disp", "op_movq_gpr_bridge", "op_pipeline",
    # maskmovq, maskmovdqu & chained maskmov pipeline (3)
    "op_maskmovq", "op_maskmovdqu", "op_maskmov_pipeline",
)

ALTERED_EXPORTS = (
    "op_cvtps2pi_reg",
    "op_cvttps2pi_reg",
    "op_pshufw_reg_e4",
    "op_pshufw_mem_4e",
    "op_maskmovq",
    "op_maskmovdqu",
)


# --- Independent Python IEEE-754 / MMX reference model ---

def u32(buf):
    return struct.unpack("<I", buf)[0]


def i32s(buf):
    return struct.unpack("<" + "i" * (len(buf) // 4), buf)


def u32s(buf):
    return struct.unpack("<" + "I" * (len(buf) // 4), buf)


def u16s(buf):
    return struct.unpack("<" + "H" * (len(buf) // 2), buf)


def u64(buf):
    return struct.unpack("<Q", buf)[0]


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits & MASK32))[0]


def pk_d(*vals):
    return struct.pack("<" + "I" * len(vals), *(v & MASK32 for v in vals))


def pk_w(*vals):
    return struct.pack("<" + "H" * len(vals), *(v & 0xFFFF for v in vals))


def pk_q(*vals):
    return struct.pack("<" + "Q" * len(vals), *(v & MASK64 for v in vals))


def ref_cvtpi2ps(xmm_bytes, mm_bytes):
    i0, i1 = i32s(mm_bytes[:8])
    lo = struct.pack("<ff", float(i0), float(i1))
    return lo + xmm_bytes[8:16]


def cvt_f32_to_i32(bits, truncate=False):
    val = f32(bits)
    if math.isnan(val) or math.isinf(val):
        return INDEFINITE_I32
    rounded = math.trunc(val) if truncate else round(val)
    if rounded < -0x80000000 or rounded > 0x7FFFFFFF:
        return INDEFINITE_I32
    return rounded & MASK32


def ref_cvtps2pi(src_bytes, truncate=False):
    d0, d1 = u32s(src_bytes[:8])
    return pk_d(cvt_f32_to_i32(d0, truncate=truncate), cvt_f32_to_i32(d1, truncate=truncate))


def ref_pshufw(src_bytes, imm8):
    words = u16s(src_bytes[:8])
    return pk_w(*(words[(imm8 >> (i * 2)) & 3] for i in range(4)))


def ref_maskmov(src, mask, base, width):
    out = bytearray(base[:width])
    for i in range(width):
        if mask[i] & 0x80:
            out[i] = src[i]
    return bytes(out)


def ref_maskmovq(src, mask, base):
    return ref_maskmov(src, mask, base, 8)


def ref_maskmovdqu(src, mask, base):
    return ref_maskmov(src, mask, base, 16)


def reference(name, a, b, altered=False):
    """Expected 32-byte output region; unwritten bytes keep the FILL pattern."""
    out = bytearray(FILL)
    if name == "op_cvtpi2ps_reg":
        out[0:16] = ref_cvtpi2ps(a[0:16], b[0:8])
    elif name == "op_cvtpi2ps_mem":
        out[0:16] = ref_cvtpi2ps(a[0:16], b[0:8])
    elif name == "op_cvtpi2ps_mem_plus1":
        out[0:16] = ref_cvtpi2ps(a[16:32], b[1:9])
    elif name == "op_cvtps2pi_reg":
        out[0:8] = ref_cvtps2pi(a[0:8], truncate=altered)
    elif name == "op_cvtps2pi_mem":
        out[0:8] = ref_cvtps2pi(a[0:8], truncate=False)
    elif name == "op_cvtps2pi_mem_plus1":
        out[8:16] = ref_cvtps2pi(b[1:9], truncate=False)
    elif name == "op_cvttps2pi_reg":
        out[0:8] = ref_cvtps2pi(a[0:8], truncate=not altered)
    elif name == "op_cvttps2pi_mem":
        out[0:8] = ref_cvtps2pi(a[8:16], truncate=False)
    elif name == "op_pshufw_reg_e4":
        imm = 0x1B if altered else 0xE4
        out[0:8] = ref_pshufw(a[0:8], imm)
    elif name == "op_pshufw_reg_1b":
        out[0:8] = ref_pshufw(a[0:8], 0x1B)
    elif name == "op_pshufw_reg_00":
        out[0:8] = ref_pshufw(a[0:8], 0x00)
    elif name == "op_pshufw_reg_ff":
        out[0:8] = ref_pshufw(a[0:8], 0xFF)
    elif name == "op_pshufw_mem_4e":
        imm = 0xB1 if altered else 0x4E
        out[0:8] = ref_pshufw(b[1:9], imm)
    elif name == "op_pshufw_mem_39":
        out[0:8] = ref_pshufw(b[8:16], 0x39)
    elif name in ("op_movntq", "op_movq_gpr_bridge"):
        out[0:8] = a[0:8]
    elif name == "op_movntq_disp":
        out[9:17] = a[1:9]
    elif name == "op_pipeline":
        mm0 = ref_cvtps2pi(a[0:8], truncate=False)
        mm1 = ref_cvtps2pi(b[1:9], truncate=True)
        mm2 = ref_pshufw(mm0, 0x4E)
        xmm0 = ref_cvtpi2ps(a[0:16], mm2)
        mm4 = ref_pshufw(b[16:24], 0xB1)
        out[0:8] = mm1
        out[8:16] = mm4
        out[16:32] = xmm0
    elif name == "op_maskmovq":
        src, mask = (b[:8], a[:8]) if altered else (a[:8], b[:8])
        out[0:8] = ref_maskmovq(src, mask, FILL[:8])
    elif name == "op_maskmovdqu":
        src, mask = (b[:16], a[:16]) if altered else (a[:16], b[:16])
        out[0:16] = ref_maskmovdqu(src, mask, FILL[:16])
    elif name == "op_maskmov_pipeline":
        out[0:16] = ref_maskmovdqu(b[1:17], a[0:16], a[0:16])
        mm0 = ref_cvtps2pi(a[0:8], truncate=False)
        mm1 = ref_cvtps2pi(b[1:9], truncate=True)
        out[0:8] = ref_maskmovq(mm1, mm0, out[0:8])
        out[16:24] = mm0
    else:
        raise AssertionError(f"unhandled export {name}")
    return bytes(out)


FLOAT_BITS = (
    0x00000000,  # +0.0
    0x80000000,  # -0.0
    0x00000001,  # min positive subnormal
    0x007FFFFF,  # max positive subnormal
    0x80000001,  # min negative subnormal
    0x807FFFFF,  # max negative subnormal
    0x3EFFFFFF,  # just below 0.5
    0x3F000000,  # 0.5 (tie to even -> 0)
    0x3F000001,  # just above 0.5
    0xBF000000,  # -0.5 (tie to even -> 0)
    0x3FC00000,  # 1.5 (tie to even -> 2, trunc -> 1)
    0xBFC00000,  # -1.5 (tie to even -> -2, trunc -> -1)
    0x40200000,  # 2.5 (tie to even -> 2, trunc -> 2)
    0xC0200000,  # -2.5 (tie to even -> -2, trunc -> -2)
    0x40600000,  # 3.5 (tie to even -> 4, trunc -> 3)
    0xC0600000,  # -3.5 (tie to even -> -4, trunc -> -3)
    0x4EFFFFFE,  # 2147483392.0
    0x4EFFFFFF,  # 2147483520.0 (largest float32 < 2^31)
    0x4F000000,  # 2147483648.0 (2^31, out of int32_t range -> 0x80000000)
    0xCEFFFFFE,  # -2147483392.0
    0xCF000000,  # -2147483648.0 (-2^31, exact INT32_MIN -> 0x80000000)
    0xCF000001,  # -2147483904.0 (< -2^31, out of int32_t range -> 0x80000000)
    0x7F800000,  # +inf
    0xFF800000,  # -inf
    0x7FC00000,  # canonical qNaN
    0x7FC00001,  # qNaN with payload
    0xFFC00000,  # negative qNaN
    0x7F800001,  # sNaN positive payload
    0xFF800001,  # sNaN negative payload
    0x7FBFFFFF,  # sNaN max payload
)

INT32_BITS = (
    0x00000000,  # 0
    0x00000001,  # 1
    0xFFFFFFFF,  # -1
    0x7FFFFFFF,  # INT32_MAX (rounds to 2^31 = 0x4f000000)
    0x80000000,  # INT32_MIN (-2^31 = 0xcf000000)
    0x80000001,  # INT32_MIN + 1
    0x7FFFFF80,  # 2147483520 (exact)
    0x7FFFFFBF,  # 2147483583 (rounds down to 2147483520)
    0x7FFFFFC0,  # 2147483584 (tie to even -> 2147483648)
    0x01000001,  # 2^24 + 1 (tie to even -> 2^24)
    0x01000003,  # 2^24 + 3 (tie to even -> 2^24 + 4)
    0xFEFFFFFF,  # -(2^24 + 1)
    0xFEFFFFFD,  # -(2^24 + 3)
    0x00FFFFFF,  # 16777215 (exact)
    0xFF000001,  # -16777215 (exact)
    0x12345678,
)


def float_pairs():
    """32-byte input pairs sweeping float32->int32 boundaries, ties, subnormals, Inf/NaN."""
    pairs = []
    n = len(FLOAT_BITS)
    for i in range(0, n, 2):
        f0 = FLOAT_BITS[i]
        f1 = FLOAT_BITS[(i + 1) % n]
        f2 = FLOAT_BITS[(i + 7) % n]
        f3 = FLOAT_BITS[(i + 13) % n]
        # Keep non-zero upper 64 bits a[8:16] so xmm[127:64] preservation is verified.
        a = pk_d(f0, f1, 0xDEADBEEF ^ f2, 0xCAFEBABE ^ f3, f2, f3, f0, f1)
        b_shifted = b"\xAA" + pk_d(f2, f3, f0, f1, f1, f0, f3) + b"\x55\x33\x11"
        pairs.append((a, b_shifted))
    rng = random.Random(0x55E01)
    for _ in range(6):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def int_pairs():
    """32-byte input pairs sweeping int32->float32 edges and preserving xmm[127:64]."""
    pairs = []
    n = len(INT32_BITS)
    for i in range(0, n, 2):
        i0 = INT32_BITS[i]
        i1 = INT32_BITS[(i + 1) % n]
        # Non-zero upper 64 bits in both vectors[0:16] and [16:32].
        a = pk_d(i0, i1, 0xA1B2C3D4, 0x5E6F7788, 0x13579BDF, 0x2468ACE0, 0xFEEDFACE, 0x8BADF00D)
        b = pk_d(i0, i1, i1, i0) + b"\x77" + pk_d(i1, i0, i0, i1)[:15]
        pairs.append((a, b))
    # Explicit unaligned +1 byte test for op_cvtpi2ps_mem_plus1.
    pairs.append((
        pk_d(0, 1, 0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00, 0xCAFEBABE, 0xDEADBEEF),
        b"\xAA" + pk_d(0x7FFFFFC0, 0x80000000, 0x01000001, 0x01000003) + b"\x11" * 15,
    ))
    rng = random.Random(0x55E02)
    for _ in range(4):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def shuffle_pairs():
    """32-byte input pairs sweeping 16-bit word patterns and unaligned offsets."""
    pairs = [
        (pk_w(0x1122, 0x3344, 0x5566, 0x7788, 0x99AA, 0xBBCC, 0xDDEE, 0xFF00,
              0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x0001, 0x0002, 0x0003, 0x0004),
         b"\xA5" + pk_w(0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF, 0x0123, 0x4567,
                        0x89AB, 0xCDEF, 0x1357, 0x2468, 0xACE0, 0xBDF1, 0xDEAD) + b"\x5A"),
        (b"\x00" * 32, b"\xFF" * 32),
        (bytes(range(0x10, 0x30)), bytes(range(0x80, 0xA0))),
        (bytes((0x17 * i) & 0xFF for i in range(32)), bytes((0x3B * i) & 0xFF for i in range(32))),
    ]
    rng = random.Random(0x55E03)
    for _ in range(4):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def maskmov_pairs():
    """32-byte input pairs sweeping byte-MSB mask patterns across all 16 lanes."""
    src = bytes(range(0x10, 0x30))
    masks = [
        b"\x00" * 32,
        b"\x7F" * 32,
        b"\x80" * 32,
        b"\xFF" * 32,
        bytes((0x7F if i % 2 == 0 else 0x80) for i in range(32)),
        bytes((0x81 if i % 2 == 0 else 0x01) for i in range(32)),
    ]
    for lane in range(16):
        m = bytearray(b"\x7F" * 32)
        m[lane] = 0x80
        masks.append(bytes(m))
    rng = random.Random(0x55E04)
    pairs = [(src, m) for m in masks]
    for _ in range(10):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def inputs_for(name):
    if name.startswith("op_cvtpi2ps"):
        return int_pairs()
    if name.startswith(("op_cvtps2pi", "op_cvttps2pi")) or name == "op_pipeline":
        return float_pairs()
    if name in ("op_maskmovq", "op_maskmovdqu"):
        return maskmov_pairs()
    if name == "op_maskmov_pipeline":
        return maskmov_pairs() + float_pairs()[:6]
    return shuffle_pairs()


# --- Guarded buffer helpers ---

def alloc_aligned_buffer(payload, align=16):
    """Allocate a guarded buffer whose payload starts at an aligned address."""
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
            f"{label} {name} case {idx}: got {got.hex()} != expected {want.hex()}")


def bind_exports(dll_path, prefix="", names=EXPORTS):
    lib = ctypes.CDLL(str(Path(dll_path).resolve()))
    funcs = {}
    for name in names:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = None
        funcs[name] = fn
    return funcs


def check_discarded_read(native_path, lifted_path):
    payload = bytes(range(32))
    for label, dll, prefix in (("native", native_path, ""), ("lifted", lifted_path, "lifted_")):
        name = "op_discarded_read"
        fn = bind_exports(dll, prefix, (name,))[name]
        source, ptr, _, _, _ = alloc_aligned_buffer(payload)
        for readable in (True, False):
            result, result_ptr, pre, off, post = alloc_aligned_buffer(FILL)
            try:
                fn(ptr, ptr if readable else 0, result_ptr)
            except OSError as error:
                assert not readable and "access violation reading" in str(error).lower(), (label, error)
            else:
                assert readable, f"{label} discarded a faulting CVTPS2PI read"
            expected = payload[:16] + (payload[:16] if readable else FILL[16:])
            assert_equal_output(
                label, name, readable, check_guards_and_read(result, pre, off, post, label), expected
            )


def check_maskmov_page_edge(native_path, lifted_path):
    """Compare native/lifted on k=8/k=16 page-abutting writes and observe native k<width edges."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.VirtualAlloc.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualProtect.argtypes = [
        wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
    ]
    k32.VirtualProtect.restype = wintypes.BOOL
    k32.VirtualFree.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
    k32.VirtualFree.restype = wintypes.BOOL

    MEM_COMMIT = 0x1000
    MEM_RESERVE = 0x2000
    MEM_RELEASE = 0x8000
    PAGE_NOACCESS = 0x01
    PAGE_READONLY = 0x02
    PAGE_READWRITE = 0x04

    specs = (
        ("op_maskmovq", 8, ref_maskmovq, (7, 4, 1, 0)),
        ("op_maskmovdqu", 16, ref_maskmovdqu, (15, 8, 7, 1, 0)),
    )
    native_fns = bind_exports(native_path, "", tuple(name for name, _, _, _ in specs))
    lifted_fns = bind_exports(lifted_path, "lifted_", tuple(name for name, _, _, _ in specs))
    src_bytes = bytes(range(0x10, 0x30))
    buf_src, ptr_src, _, _, _ = alloc_aligned_buffer(src_bytes)

    abutting_cases = 0
    native_obs_cases = 0
    for export_name, width, ref_fn, partial_ks in specs:
        sparse_mask = (
            bytes(0x80 if i in (0, width // 2 - 1, width - 1) else (0x7F if i % 2 else 0x00)
                  for i in range(width))
            + b"\x00" * (32 - width)
        )
        for guard_prot in (PAGE_NOACCESS, PAGE_READONLY):
            base = k32.VirtualAlloc(None, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
            if not base:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                old_prot = wintypes.DWORD()
                if not k32.VirtualProtect(base + 4096, 4096, guard_prot, ctypes.byref(old_prot)):
                    raise ctypes.WinError(ctypes.get_last_error())

                # Page-abutting writable check (k=8 / k=16): both native and lifted must succeed and match.
                ptr_dst = base + 4096 - width
                for mask_name, mask_bytes in (
                    ("full_mask", b"\x80" * width + b"\x00" * (32 - width)),
                    ("all_zero_mask", b"\x7F" * width + b"\x00" * (32 - width)),
                    ("sparse_mask", sparse_mask),
                ):
                    buf_mask, ptr_mask, _, _, _ = alloc_aligned_buffer(mask_bytes)
                    want = ref_fn(src_bytes[:width], mask_bytes[:width], b"\x5A" * width)
                    for label, fn in (("native", native_fns[export_name]), ("lifted", lifted_fns[export_name])):
                        ctypes.memset(base + 4096 - 32, 0x5A, 32)
                        fn(ptr_src, ptr_mask, ptr_dst)
                        pre_pad = bytes((ctypes.c_ubyte * (32 - width)).from_address(base + 4096 - 32))
                        assert pre_pad == b"\x5A" * (32 - width), (
                            f"{label} {export_name} corrupted bytes preceding destination ({mask_name})"
                        )
                        after = bytes((ctypes.c_ubyte * width).from_address(ptr_dst))
                        assert after == want, (
                            f"{label} {export_name} k={width} page-abutting mismatch ({mask_name}): "
                            f"{after.hex()} != {want.hex()}"
                        )
                        abutting_cases += 1
                    _ = buf_mask

                # Partial-vector page-edge probe (k<width): strictly native observation (impl-dependent).
                for k in partial_ks:
                    ptr_edge = base + 4096 - k
                    masks = (
                        ("only_writable_masked",
                         bytes(0x80 if i < k else 0x7F for i in range(width)) + b"\x00" * (32 - width)),
                        ("all_zero_mask", b"\x7F" * width + b"\x00" * (32 - width)),
                        ("edge_byte_masked",
                         bytes(0x80 if i in (0, k) else 0x00 for i in range(width)) + b"\x00" * (32 - width)),
                    )
                    for mask_name, mask_bytes in masks:
                        buf_mask, ptr_mask, _, _, _ = alloc_aligned_buffer(mask_bytes)
                        ctypes.memset(base + 4096 - 32, 0x5A, 32)
                        faulted = False
                        try:
                            native_fns[export_name](ptr_src, ptr_mask, ptr_edge)
                        except OSError as error:
                            assert "access violation" in str(error).lower(), (
                                export_name, guard_prot, k, mask_name, error
                            )
                            faulted = True
                        selects_guard = any(byte & 0x80 for byte in mask_bytes[k:width])
                        assert faulted or not selects_guard, (export_name, guard_prot, k, mask_name)
                        after = bytes((ctypes.c_ubyte * k).from_address(ptr_edge))
                        expected = ref_fn(src_bytes[:width], mask_bytes[:width], b"\x5A" * width)[:k]
                        if faulted:
                            assert all(actual in (0x5A, wanted) for actual, wanted in zip(after, expected)), (
                                export_name, guard_prot, k, mask_name, after.hex(), expected.hex()
                            )
                        else:
                            assert after == expected, (
                                export_name, guard_prot, k, mask_name, after.hex(), expected.hex()
                            )
                        print(f"native page edge {export_name} prot={guard_prot:#x} k={k} {mask_name}: "
                              f"faulted={int(faulted)}, prefix={after.hex()}")
                        native_obs_cases += 1
                        _ = buf_mask
            finally:
                k32.VirtualFree(base, 0, MEM_RELEASE)
    _ = buf_src
    return abutting_cases, native_obs_cases


def check_differential(native_path, lifted_path, altered_path):
    abutting_cases, native_obs_cases = check_maskmov_page_edge(native_path, lifted_path)
    native = bind_exports(native_path)
    lifted = bind_exports(lifted_path, "lifted_")
    altered = bind_exports(altered_path, "lifted_")

    total = 0
    for name in EXPORTS:
        for idx, (a, b) in enumerate(inputs_for(name)):
            want = reference(name, a, b)
            got_n = invoke(native[name], a, b)
            got_l = invoke(lifted[name], a, b)
            assert_equal_output("native", name, idx, got_n, want)
            assert_equal_output("lifted", name, idx, got_l, want)
            total += 1

    # Altered-immediate / altered-mnemonic divergence controls.
    changed = {}
    for name in ALTERED_EXPORTS:
        diffs = 0
        for idx, (a, b) in enumerate(inputs_for(name)):
            want_alt = reference(name, a, b, altered=True)
            want_orig = reference(name, a, b)
            got_alt = invoke(altered[name], a, b)
            assert_equal_output("altered", name, idx, got_alt, want_alt)
            if got_alt != want_orig:
                diffs += 1
        if not diffs:
            raise AssertionError(f"altered control {name} did not diverge from original")
        changed[name] = diffs

    # Output mutation negative control.
    sample_a, sample_b = float_pairs()[0]
    expected = reference("op_pipeline", sample_a, sample_b)
    mutated = bytearray(expected)
    mutated[5] ^= 0x01
    try:
        assert_equal_output("mutation", "op_pipeline", 0, bytes(mutated), expected)
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    print(f"{total} native/lifted/reference SSE/MMX cases across {len(EXPORTS)} exports matched "
          f"bit-for-bit; {abutting_cases} k=8/k=16 page-abutting checks and "
          f"{native_obs_cases} native partial-edge observations; altered controls diverged on "
          f"{changed}; output mutation control caught")


# --- Alternative-encoding acceptance & translator rejection controls ---

def encode_seq(*instructions):
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_alt_encodings_and_rejections(original_path):
    """Verify alternate encodings lift cleanly and all out-of-scope/illegal forms reject."""
    ld_xmm0 = Instruction.create_reg_mem(Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    ld_xmm4 = Instruction.create_reg_mem(Code.MOVDQA_XMM_XMMM128, R.XMM4, MemoryOperand(R.RCX))
    ld_xmm5 = Instruction.create_reg_mem(Code.MOVDQU_XMM_XMMM128, R.XMM5, MemoryOperand(R.RDX, displ=3, displ_size=1))
    ld_mm0 = Instruction.create_reg_mem(Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.RCX))
    ld_mm1 = Instruction.create_reg_mem(Code.MOVQ_MM_MMM64, R.MM1, MemoryOperand(R.RDX))
    ld_mm5 = Instruction.create_reg_mem(Code.MOVQ_MM_MMM64, R.MM5, MemoryOperand(R.RDX))
    st_xmm4 = Instruction.create_mem_reg(Code.MOVAPS_XMMM128_XMM, MemoryOperand(R.R8), R.XMM4)
    st_mm5 = Instruction.create_mem_reg(Code.MOVNTQ_M64_MM, MemoryOperand(R.R8), R.MM5)
    emms_ins = Instruction.create(Code.EMMS)
    ret_ins = Instruction.create(Code.RETNQ)

    alt_accept = [
        ("cvtpi2ps_xmm4_mm5", (ld_xmm4, ld_mm5, Instruction.create_reg_reg(
            Code.CVTPI2PS_XMM_MMM64, R.XMM4, R.MM5), st_xmm4, emms_ins, ret_ins)),
        ("cvtps2pi_mm6_xmm5", (ld_xmm5, Instruction.create_reg_reg(
            Code.CVTPS2PI_MM_XMMM64, R.MM6, R.XMM5),
            Instruction.create_mem_reg(Code.MOVNTQ_M64_MM, MemoryOperand(R.R8), R.MM6), emms_ins, ret_ins)),
        ("cvttps2pi_mm7_neg_disp", (Instruction.create_reg_mem(
            Code.CVTTPS2PI_MM_XMMM64, R.MM7, MemoryOperand(R.RDX, displ=-8, displ_size=1)),
            Instruction.create_mem_reg(Code.MOVQ_MMM64_MM, MemoryOperand(R.R8), R.MM7), emms_ins, ret_ins)),
        ("pshufw_r8_read", (Instruction.create_reg_mem_u32(
            Code.PSHUFW_MM_MMM64_IMM8, R.MM4, MemoryOperand(R.R8, displ=8, displ_size=1), 0x93),
            Instruction.create_mem_reg(Code.MOVNTQ_M64_MM, MemoryOperand(R.R8), R.MM4), emms_ins, ret_ins)),
        ("movq_mm_alt_store_opcode", (ld_mm0, Instruction.create_reg_reg(
            Code.MOVQ_MMM64_MM, R.MM4, R.MM0),
            Instruction.create_mem_reg(Code.MOVQ_RM64_MM, MemoryOperand(R.R8, displ=16, displ_size=1), R.MM4),
            emms_ins, ret_ins)),
        ("movq_r11_bridge", (Instruction.create_reg_mem(
            Code.MOVQ_MM_RM64, R.MM2, MemoryOperand(R.RCX)),
            Instruction.create_reg_reg(Code.MOVQ_RM64_MM, R.R11, R.MM2),
            Instruction.create_reg_reg(Code.MOVQ_MM_RM64, R.MM5, R.R11),
            st_mm5, emms_ins, ret_ins)),
        ("movaps_movdqa_alt_reg", (ld_xmm0, Instruction.create_reg_reg(
            Code.MOVAPS_XMMM128_XMM, R.XMM3, R.XMM0),
            Instruction.create_reg_reg(Code.MOVDQA_XMMM128_XMM, R.XMM4, R.XMM3),
            st_xmm4, ret_ins)),
        ("maskmovq_r9_save", (ld_mm0, ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R9, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R9),
            emms_ins, ret_ins)),
        ("maskmovdqu_rax_rm64_save", (ld_xmm0, ld_xmm4,
            Instruction.create_reg_reg(Code.MOV_RM64_R64, R.RAX, R.RDI),
            Instruction.create_reg_reg(Code.MOV_RM64_R64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.MOV_RM64_R64, R.RDI, R.RAX),
            ret_ins)),
    ]
    alt_expected = {
        "cvtpi2ps_xmm4_mm5": "xmm4.q[0] = op_cvtpi2ps(mm[5]);",
        "cvtps2pi_mm6_xmm5": "mm[6] = op_cvtps2pi(xmm5.q[0]);",
        "cvttps2pi_mm7_neg_disp": "load64((rdx + 0xfffffffffffffff8ull))",
        "pshufw_r8_read": "op_pshufw(load64((r8 + 0x8ull)), 0x93u)",
        "movq_mm_alt_store_opcode": "store64((r8 + 0x10ull), mm[4]);",
        "movq_r11_bridge": "r11 = mm[2];",
        "movaps_movdqa_alt_reg": "xmm4 = xmm3;",
        "maskmovq_r9_save": "op_maskmovq(r8, mm[0], mm[1]);",
        "maskmovdqu_rax_rm64_save": "op_maskmovdqu(r8, xmm0, xmm4);",
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

    # 1) Non-legacy VEX and EVEX encodings must reject.
    nonlegacy_cases = [
        ("vmovq_vex_load", (Instruction.create_reg_mem(
            Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovq_vex_store", (ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVQ_XMMM64_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins), "non-legacy encoding"),
        ("vmovaps_vex", (Instruction.create_reg_mem(
            Code.VEX_VMOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovdqu_vex", (Instruction.create_reg_mem(
            Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vpshufb_vex", (ld_xmm0, ld_xmm4, Instruction.create_reg_reg_reg(
            Code.VEX_VPSHUFB_XMM_XMM_XMMM128, R.XMM2, R.XMM0, R.XMM4), ret_ins), "non-legacy encoding"),
        ("vmovaps_evex", (Instruction.create_reg_mem(
            Code.EVEX_VMOVAPS_XMM_K1Z_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmovdqa32_evex", (Instruction.create_reg_mem(
            Code.EVEX_VMOVDQA32_XMM_K1Z_XMMM128, R.XMM0, MemoryOperand(R.RCX)), ret_ins), "non-legacy encoding"),
        ("vmaskmovdqu_vex", (ld_xmm0, ld_xmm4, Instruction.create_vmaskmovdqu(
            64, R.XMM0, R.XMM4), ret_ins), "non-legacy encoding"),
    ]

    # 2) Unsupported instructions (other SSE/SSE2/MMX/3DNow!/GPR ops).
    unsupported_insns = [
        ("cvtpd2pi", (ld_xmm0, Instruction.create_reg_reg(Code.CVTPD2PI_MM_XMMM128, R.MM0, R.XMM0), emms_ins, ret_ins)),
        ("cvttpd2pi", (ld_xmm0, Instruction.create_reg_reg(Code.CVTTPD2PI_MM_XMMM128, R.MM0, R.XMM0), emms_ins, ret_ins)),
        ("cvtpi2pd", (ld_mm0, Instruction.create_reg_reg(Code.CVTPI2PD_XMM_MMM64, R.XMM0, R.MM0), emms_ins, ret_ins)),
        ("movdq2q", (ld_xmm0, Instruction.create_reg_reg(Code.MOVDQ2Q_MM_XMM, R.MM0, R.XMM0), emms_ins, ret_ins)),
        ("movq2dq", (ld_mm0, Instruction.create_reg_reg(Code.MOVQ2DQ_XMM_MM, R.XMM0, R.MM0), emms_ins, ret_ins)),
        ("movntdq", (ld_xmm0, Instruction.create_mem_reg(Code.MOVNTDQ_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins)),
        ("movntps", (ld_xmm0, Instruction.create_mem_reg(Code.MOVNTPS_M128_XMM, MemoryOperand(R.R8), R.XMM0), ret_ins)),
        ("cmpps", (ld_xmm0, Instruction.create_reg_reg_u32(Code.CMPPS_XMM_XMMM128_IMM8, R.XMM0, R.XMM0, 0), ret_ins)),
        ("addps", (ld_xmm0, Instruction.create_reg_reg(Code.ADDPS_XMM_XMMM128, R.XMM0, R.XMM0), ret_ins)),
        ("pshufd", (ld_xmm0, Instruction.create_reg_reg_u32(Code.PSHUFD_XMM_XMMM128_IMM8, R.XMM0, R.XMM0, 0), ret_ins)),
        ("femms", (Instruction.create(Code.FEMMS), ret_ins)),
        ("mov_gpr", (Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RCX), ret_ins)),
    ]

    # 3) Register policy, MMX/EMMS state, undefined regs, addressing, alignment, prefixes, stream.
    seg_load = Instruction.create_reg_mem(Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.RCX))
    seg_load.segment_prefix = R.GS
    other_cases = [
        ("nonvolatile_xmm6_load", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm15_cvtps2pi", (ld_mm0, Instruction.create_reg_reg(
            Code.CVTPS2PI_MM_XMMM64, R.MM0, R.XMM15), emms_ins, ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm8_cvtpi2ps", (ld_mm0, Instruction.create_reg_reg(
            Code.CVTPI2PS_XMM_MMM64, R.XMM8, R.MM0), emms_ins, ret_ins), "nonvolatile vector register"),
        ("nonvolatile_gpr_movq_rbx", (ld_mm0, Instruction.create_reg_reg(
            Code.MOVQ_RM64_MM, R.RBX, R.MM0), emms_ins, ret_ins), "nonvolatile GPR"),
        ("nonvolatile_gpr_movq_r12", (Instruction.create_reg_reg(
            Code.MOVQ_MM_RM64, R.MM0, R.R12), emms_ins, ret_ins), "nonvolatile GPR"),
        ("arg_gpr_movq_rcx", (Instruction.create_reg_reg(
            Code.MOVQ_MM_RM64, R.MM0, R.RCX), emms_ins, ret_ins), "unsupported argument GPR operand"),
        ("arg_gpr_movq_r8", (ld_mm0, Instruction.create_reg_reg(
            Code.MOVQ_RM64_MM, R.R8, R.MM0), emms_ins, ret_ins), "unsupported argument GPR operand"),
        ("other_gpr_movq_rsi", (Instruction.create_reg_reg(
            Code.MOVQ_MM_RM64, R.MM0, R.RSI), emms_ins, ret_ins), "nonvolatile GPR"),
        ("other_gpr_movq_rdx", (ld_mm0, Instruction.create_reg_reg(
            Code.MOVQ_RM64_MM, R.RDX, R.MM0), emms_ins, ret_ins), "unsupported argument GPR operand"),
        ("missing_emms_after_cvtps2pi", (ld_xmm0, Instruction.create_reg_reg(
            Code.CVTPS2PI_MM_XMMM64, R.MM0, R.XMM0), ret_ins), "missing emms before ret"),
        ("missing_emms_after_cvttps2pi", (Instruction.create_reg_mem(
            Code.CVTTPS2PI_MM_XMMM64, R.MM1, MemoryOperand(R.RCX)), ret_ins), "missing emms before ret"),
        ("missing_emms_after_pshufw", (Instruction.create_reg_mem_u32(
            Code.PSHUFW_MM_MMM64_IMM8, R.MM2, MemoryOperand(R.RCX), 0xE4), ret_ins), "missing emms before ret"),
        ("missing_emms_after_reenter", (ld_mm0, emms_ins, Instruction.create_reg_mem(
            Code.MOVQ_MM_MMM64, R.MM3, MemoryOperand(R.RDX)), ret_ins), "missing emms before ret"),
        ("missing_emms_after_maskmovq", (ld_mm0, ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "missing emms before ret"),
        ("maskmovq_no_r8_rdi", (ld_mm0, ld_mm1,
            Instruction.create_maskmovq(64, R.MM0, R.MM1), emms_ins, ret_ins), "maskmovq requires r8-rooted rdi"),
        ("maskmovdqu_no_r8_rdi", (ld_xmm0, ld_xmm4,
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4), ret_ins), "maskmovdqu requires r8-rooted rdi"),
        ("maskmovq_unsaved_rdi", (ld_mm0, ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1), emms_ins, ret_ins), "rdi modified without saving"),
        ("maskmovdqu_unsaved_rdi", (ld_xmm0, ld_xmm4,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4), ret_ins), "rdi modified without saving"),
        ("maskmovq_unrestored_rdi", (ld_mm0, ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1), emms_ins, ret_ins), "unrestored nonvolatile GPR rdi"),
        ("maskmovdqu_unrestored_rdi", (ld_xmm0, ld_xmm4,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4), ret_ins), "unrestored nonvolatile GPR rdi"),
        ("maskmov_non_r8_rdi_rcx", (
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.RCX),
            ret_ins), "non-r8 or unsaved write to nonvolatile GPR rdi"),
        ("maskmov_clobbered_save_slot", (ld_mm0,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_reg_reg(Code.MOVQ_RM64_MM, R.R10, R.MM0),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            emms_ins, ret_ins), "non-r8 or unsaved write to nonvolatile GPR rdi"),
        ("rdi_save_clobbers_prior_def", (ld_mm0,
            Instruction.create_reg_reg(Code.MOVQ_RM64_MM, R.RAX, R.MM0),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RDI),
            Instruction.create_reg_reg(Code.MOVQ_MM_RM64, R.MM1, R.RAX),
            emms_ins, ret_ins), "GPR rax holds saved rdi"),
        ("rdi_save_post_restore_still_unavail", (
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.RAX),
            Instruction.create_reg_reg(Code.MOVQ_MM_RM64, R.MM0, R.RAX),
            emms_ins, ret_ins), "GPR rax holds saved rdi"),
        ("rdi_save_twice_after_clobber", (
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R11, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "cannot save clobbered rdi"),
        ("rdi_save_nonvolatile_rbx", (
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RBX, R.RDI),
            ret_ins), "nonvolatile GPR"),
        ("rdi_save_arg_rcx", (
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RCX, R.RDI),
            ret_ins), "unsupported argument GPR operand"),
        ("rdi_32bit_mov", (
            Instruction.create_reg_reg(Code.MOV_R32_RM32, R.EDI, R.R8D),
            ret_ins), "unsupported instruction"),
        ("maskmovq_edi_override", (ld_mm0, ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(32, R.MM0, R.MM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            emms_ins, ret_ins), "unsupported maskmovq target"),
        ("maskmovdqu_edi_override", (ld_xmm0, ld_xmm4,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(32, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "unsupported maskmovdqu target"),
        ("undefined_mm_maskmovq_src", (ld_mm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            emms_ins, ret_ins), "undefined MMX register mm0"),
        ("undefined_mm_maskmovq_mask", (ld_mm0,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovq(64, R.MM0, R.MM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            emms_ins, ret_ins), "undefined MMX register mm1"),
        ("undefined_xmm_maskmovdqu_src", (ld_xmm4,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "undefined vector register xmm0"),
        ("undefined_xmm_maskmovdqu_mask", (ld_xmm0,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_maskmovdqu(64, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "undefined vector register xmm4"),
        ("undefined_xmm_cvtpi2ps_dst", (ld_mm0, Instruction.create_reg_reg(
            Code.CVTPI2PS_XMM_MMM64, R.XMM0, R.MM0), emms_ins, ret_ins), "undefined vector register xmm0"),
        ("undefined_xmm_cvtps2pi_src", (Instruction.create_reg_reg(
            Code.CVTPS2PI_MM_XMMM64, R.MM0, R.XMM2), emms_ins, ret_ins), "undefined vector register xmm2"),
        ("undefined_mm_cvtpi2ps_src", (ld_xmm0, Instruction.create_reg_reg(
            Code.CVTPI2PS_XMM_MMM64, R.XMM0, R.MM3), emms_ins, ret_ins), "undefined MMX register mm3"),
        ("undefined_mm_pshufw_src", (Instruction.create_reg_reg_u32(
            Code.PSHUFW_MM_MMM64_IMM8, R.MM0, R.MM4, 0), emms_ins, ret_ins), "undefined MMX register mm4"),
        ("undefined_mm_movntq_src", (Instruction.create_mem_reg(
            Code.MOVNTQ_M64_MM, MemoryOperand(R.R8), R.MM1), emms_ins, ret_ins), "undefined MMX register mm1"),
        ("undefined_mm_after_emms", (ld_mm0, emms_ins, Instruction.create_mem_reg(
            Code.MOVNTQ_M64_MM, MemoryOperand(R.R8), R.MM0), emms_ins, ret_ins), "undefined MMX register mm0"),
        ("undefined_gpr_rax", (Instruction.create_reg_reg(
            Code.MOVQ_MM_RM64, R.MM0, R.RAX), emms_ins, ret_ins), "undefined GPR rax"),
        ("non_r8_store_movntq_rcx", (ld_mm0, Instruction.create_mem_reg(
            Code.MOVNTQ_M64_MM, MemoryOperand(R.RCX), R.MM0), emms_ins, ret_ins), "non-r8 store memory operand"),
        ("non_r8_store_movntq_rdx", (ld_mm0, Instruction.create_mem_reg(
            Code.MOVNTQ_M64_MM, MemoryOperand(R.RDX), R.MM0), emms_ins, ret_ins), "non-r8 store memory operand"),
        ("non_r8_store_movq_rcx", (ld_mm0, Instruction.create_mem_reg(
            Code.MOVQ_MMM64_MM, MemoryOperand(R.RCX), R.MM0), emms_ins, ret_ins), "non-r8 store memory operand"),
        ("non_r8_store_movaps_rdx", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVAPS_XMMM128_XMM, MemoryOperand(R.RDX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("unaligned_movaps_load", (Instruction.create_reg_mem(
            Code.MOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, displ=8, displ_size=1)), ret_ins),
            "unaligned 16-byte memory operand"),
        ("unaligned_movdqa_store", (ld_xmm0, Instruction.create_mem_reg(
            Code.MOVDQA_XMMM128_XMM, MemoryOperand(R.R8, displ=4, displ_size=1), R.XMM0), ret_ins),
            "unaligned 16-byte memory operand"),
        ("nonvolatile_gpr_base_rbx", (Instruction.create_reg_mem(
            Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.RBX)), emms_ins, ret_ins), "nonvolatile GPR base"),
        ("non_arg_base_rax", (Instruction.create_reg_mem(
            Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.RAX)), emms_ins, ret_ins), "unsupported memory base"),
        ("rip_rel_mem", (Instruction.create_reg_mem(
            Code.CVTPS2PI_MM_XMMM64, R.MM0, MemoryOperand(R.RIP, displ=0x1010, displ_size=1)), emms_ins, ret_ins),
            "unsupported memory operand"),
        ("rsp_mem", (Instruction.create_reg_mem_u32(
            Code.PSHUFW_MM_MMM64_IMM8, R.MM0, MemoryOperand(R.RSP), 0), emms_ins, ret_ins),
            "unsupported memory operand"),
        ("addr32_mem", (Instruction.create_reg_mem(
            Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.ECX)), emms_ins, ret_ins),
            "unsupported memory operand"),
        ("indexed_mem", (Instruction.create_reg_mem(
            Code.CVTTPS2PI_MM_XMMM64, R.MM0, MemoryOperand(R.RCX, R.RDX, 4)), emms_ins, ret_ins),
            "unsupported memory operand"),
        ("segment_prefix", (seg_load, emms_ins, ret_ins), "unsupported instruction prefix"),
        ("missing_ret", (ld_xmm0,), "missing terminal ret"),
        ("after_ret", (ret_ins, ld_xmm0), "unreachable instruction after ret"),
    ]

    raw_cases = [
        ("lock_cmpxchg", bytes.fromhex("f00fb109") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("rep_movsb", bytes.fromhex("f3a4") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("invalid_ud_bytes", bytes.fromhex("fffe") + encode_seq(ret_ins), "invalid instruction"),
        ("truncated_insn", encode_seq(ld_xmm0)[:-1], "invalid instruction"),
        ("empty_stream", b"", "truncated instruction stream"),
    ]

    all_unit = (
        nonlegacy_cases
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
        ("pe_vmovq_vex", encode_seq(*nonlegacy_cases[0][1]), "non-legacy encoding"),
        ("pe_cvtpd2pi", encode_seq(*unsupported_insns[0][1]), "unsupported instruction"),
        ("pe_nonvolatile_xmm6", encode_seq(*other_sequences["nonvolatile_xmm6_load"]), "nonvolatile vector register"),
        ("pe_missing_emms", encode_seq(*other_sequences["missing_emms_after_cvtps2pi"]), "missing emms before ret"),
        ("pe_undef_cvtpi2ps", encode_seq(*other_sequences["undefined_xmm_cvtpi2ps_dst"]), "undefined vector register xmm0"),
        ("pe_undef_mm_emms", encode_seq(*other_sequences["undefined_mm_after_emms"]), "undefined MMX register mm0"),
        ("pe_non_r8_movntq", encode_seq(*other_sequences["non_r8_store_movntq_rcx"]), "non-r8 store memory operand"),
        ("pe_unaligned_movaps", encode_seq(*other_sequences["unaligned_movaps_load"]), "unaligned 16-byte memory operand"),
        ("pe_nonvolatile_rbx", encode_seq(*other_sequences["nonvolatile_gpr_base_rbx"]), "nonvolatile GPR base"),
        ("pe_missing_ret", encode_seq(ld_xmm0), "missing terminal ret"),
        ("pe_vmaskmovdqu_vex", encode_seq(*nonlegacy_cases[-1][1]), "non-legacy encoding"),
        ("pe_missing_emms_maskmovq", encode_seq(*other_sequences["missing_emms_after_maskmovq"]), "missing emms before ret"),
        ("pe_unrestored_rdi_maskmovdqu", encode_seq(*other_sequences["maskmovdqu_unrestored_rdi"]), "unrestored nonvolatile GPR rdi"),
        ("pe_rdi_save_leak", encode_seq(*other_sequences["rdi_save_clobbers_prior_def"]), "GPR rax holds saved rdi"),
        ("pe_maskmovq_edi", encode_seq(*other_sequences["maskmovq_edi_override"]), "unsupported maskmovq target"),
        ("pe_maskmovdqu_undef_xmm", encode_seq(*other_sequences["undefined_xmm_maskmovdqu_mask"]), "undefined vector register xmm4"),
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
          f"({len(nonlegacy_cases)} VEX/EVEX, {len(unsupported_insns)} unsupported instructions, "
          f"{len(other_cases) + len(raw_cases)} ABI/state/addressing/prefix/stream) and "
          f"{len(pe_patches)} PE-patched rejection controls passed")


if __name__ == "__main__":
    check_discarded_read(*sys.argv[1:3])
    check_differential(*sys.argv[1:4])
    check_alt_encodings_and_rejections(sys.argv[1])
