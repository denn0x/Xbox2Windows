"""Differential checks, page-edge checks, fault-ordering controls, mutation controls and rejection controls for AVX quadword/masked-byte move AOT."""

import ctypes
from ctypes import wintypes
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Decoder, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_avx_fp_move_aot import lift_code, translate
from cpu_leaf_aot import export_rva

GUARD_PRE = bytes(0xA0 + (i % 16) for i in range(32))
GUARD_POST = bytes(0xC0 + (i % 16) for i in range(32))
FILL = b"\x5A" * 32

EXPORTS = (
    "op_vmovq_xmm_xmm",
    "op_vmovq_xmm_m64",
    "op_vmovq_m64_xmm",
    "op_vmovq_xmm_r64",
    "op_vmovq_r64_xmm",
    "op_vmaskmovdqu",
    "op_pipeline",
)

# vmovupd/vmovapd exports: op_<mnemonic>_<width>_<load|store|reg|regst>, plus a mixed pipeline.
PD_EXPORTS = (
    "op_vmovupd_xmm_load",
    "op_vmovupd_ymm_load",
    "op_vmovupd_xmm_store",
    "op_vmovupd_ymm_store",
    "op_vmovupd_xmm_reg",
    "op_vmovupd_ymm_reg",
    "op_vmovupd_xmm_regst",
    "op_vmovupd_ymm_regst",
    "op_vmovapd_xmm_load",
    "op_vmovapd_ymm_load",
    "op_vmovapd_xmm_store",
    "op_vmovapd_ymm_store",
    "op_vmovapd_xmm_reg",
    "op_vmovapd_ymm_reg",
    "op_vmovapd_xmm_regst",
    "op_vmovapd_ymm_regst",
    "op_pd_pipeline",
)

EXPORTS = EXPORTS + PD_EXPORTS

ALTERED_EXPORTS = (
    "op_vmovq_xmm_xmm",
    "op_vmovq_xmm_m64",
    "op_vmaskmovdqu",
    "op_vmovupd_xmm_load",
    "op_vmovupd_xmm_reg",
    "op_vmovapd_ymm_load",
)


def ref_vmaskmovdqu(src: bytes, mask: bytes, base: bytes) -> bytes:
    out = bytearray(base)
    for i in range(16):
        if mask[i] & 0x80:
            out[i] = src[i]
    return bytes(out)


def ref_pd(name: str, a: bytes, b: bytes, altered: bool) -> bytes:
    """Byte model of the vmovupd/vmovapd exports: raw bytes move, VEX.128 register writes zero [255:128]."""
    out = bytearray(FILL)
    if name == "op_pd_pipeline":
        out[0:16] = a[0:16]
        out[16:24] = b[1:9]
        out[24:32] = b"\x00" * 8
        return bytes(out)
    _, mnemonic, width, form = name.split("_")
    unaligned = mnemonic == "vmovupd"
    wide = width == "ymm"
    if form == "load":
        if wide and not (altered and not unaligned):
            out[:] = b
        elif wide:
            # ALTER_OP builds the aligned ymm load as a VEX.128 load.
            out[0:16] = b[0:16]
            out[16:32] = b"\x00" * 16
        else:
            offset = (9 if altered else 1) if unaligned else 16
            out[0:16] = b[offset : offset + 16]
            out[16:32] = b"\x00" * 16
    elif form == "store":
        if wide:
            out[:] = a
        else:
            offset = 5 if unaligned else 16
            out[offset : offset + 16] = a[0:16]
    elif form in ("reg", "regst"):
        if wide or (altered and unaligned and form == "reg"):
            out[:] = b
        else:
            out[0:16] = b[0:16]
            out[16:32] = b"\x00" * 16
    else:
        raise ValueError(f"unknown export {name}")
    return bytes(out)


def reference(name: str, a: bytes, b: bytes, altered: bool = False) -> bytes:
    """Expected 32-byte output region; unwritten bytes keep the FILL pattern."""
    if name in PD_EXPORTS:
        return ref_pd(name, a, b, altered)
    out = bytearray(FILL)
    if name == "op_vmovq_xmm_xmm":
        src = a[0:8] if altered else b[0:8]
        out[0:8] = src
        out[8:32] = b"\x00" * 24
    elif name == "op_vmovq_xmm_m64":
        off = 9 if altered else 1
        out[0:8] = b[off : off + 8]
        out[8:32] = b"\x00" * 24
    elif name == "op_vmovq_m64_xmm":
        out[9:17] = a[0:8]
    elif name == "op_vmovq_xmm_r64":
        out[0:8] = b[0:8]
        out[8:32] = b"\x00" * 24
    elif name == "op_vmovq_r64_xmm":
        out[8:16] = a[0:8]
    elif name == "op_vmaskmovdqu":
        src, mask = (b[:16], a[:16]) if altered else (a[:16], b[:16])
        out[:16] = ref_vmaskmovdqu(src, mask, FILL[:16])
    elif name == "op_pipeline":
        q_rdx16 = b[16:24]
        out[0:16] = ref_vmaskmovdqu(a[0:16], b[0:16], a[0:16])
        out[16:24] = q_rdx16
        out[24:32] = q_rdx16
    else:
        raise ValueError(f"unknown export {name}")
    return bytes(out)


# --- Test vectors ---

def move_pairs():
    pairs = [
        (bytes(range(0x10, 0x30)), bytes(range(0x80, 0xA0))),
        (b"\xFF" * 32, b"\x00" * 32),
        (b"\x00" * 32, b"\xFF" * 32),
        (bytes((0x17 * i + 3) & 0xFF for i in range(32)),
         bytes((0x2B * i + 7) & 0xFF for i in range(32))),
    ]
    rng = random.Random(0xA7F903)
    for _ in range(10):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def maskmov_pairs():
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
    rng = random.Random(0xA7F904)
    pairs = [(src, m) for m in masks]
    for _ in range(10):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def fp_pattern_pairs():
    """Double and float bit patterns that a value-converting move would change."""
    specials = [
        0x7FF0000000000001,  # signalling NaN with payload
        0xFFF8DEADBEEF0001,  # negative quiet NaN with payload
        0x8000000000000000,  # -0.0
        0x7FF0000000000000,  # +inf
        0xFFF0000000000000,  # -inf
        0x0000000000000001,  # smallest subnormal
        0x7FA000017FC00002,  # two single-precision NaNs
        0x3FF0000000000000,  # 1.0
    ]
    pairs = []
    for shift in range(len(specials)):
        a = b"".join(struct.pack("<Q", specials[(shift + i) % len(specials)]) for i in range(4))
        b = b"".join(struct.pack("<Q", specials[(shift * 3 + 5 + i) % len(specials)]) for i in range(4))
        pairs.append((a, b))
    return pairs


def inputs_for(name: str):
    if name in ("op_vmaskmovdqu", "op_pipeline"):
        return maskmov_pairs()
    if name in PD_EXPORTS:
        return move_pairs() + fp_pattern_pairs()
    return move_pairs()


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


def invoke(fn, a: bytes, b: bytes) -> bytes:
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(a)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(b)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL)
    fn(ptr_a, ptr_b, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "input_a")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "input_b")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "output")


def assert_equal_output(label: str, name: str, idx, got: bytes, want: bytes) -> None:
    if got != want:
        raise AssertionError(
            f"{label} {name} case {idx}: got {got.hex()} != expected {want.hex()}"
        )


def bind_exports(dll_path: Path, prefix: str = "", names=EXPORTS):
    lib = ctypes.CDLL(str(dll_path.resolve()))
    funcs = {}
    for name in names:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = None
        funcs[name] = fn
    return lib, funcs


def check_discarded_read(native_path: Path, lifted_path: Path) -> None:
    payload = bytes(range(32))
    for name in ("op_discarded_read", "op_discarded_read64"):
        for label, dll, prefix in (("native", native_path, ""), ("lifted", lifted_path, "lifted_")):
            _, fns = bind_exports(dll, prefix, (name,))
            fn = fns[name]
            source, ptr, _, _, _ = alloc_aligned_buffer(payload)
            for readable in (True, False):
                result, result_ptr, pre, off, post = alloc_aligned_buffer(FILL)
                try:
                    fn(ptr, ptr if readable else 0, result_ptr)
                except OSError as error:
                    assert not readable and "access violation reading" in str(error).lower(), (name, label, error)
                else:
                    assert readable, f"{label} {name} discarded a faulting read"
                expected = payload[:16] + (payload[:16] if readable else FILL[16:])
                assert_equal_output(
                    label, name, readable, check_guards_and_read(result, pre, off, post, label), expected
                )


def check_vmaskmovdqu_page_edge(native_path: Path, lifted_path: Path) -> tuple[int, int]:
    """Compare native/lifted on page-abutting writable vectors (k=16) and observe native k<16 page-edge faults."""
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

    _, native_fns = bind_exports(native_path, "", ("op_vmaskmovdqu",))
    _, lifted_fns = bind_exports(lifted_path, "lifted_", ("op_vmaskmovdqu",))
    src_bytes = bytes(range(0x10, 0x30))
    buf_src, ptr_src, _, _, _ = alloc_aligned_buffer(src_bytes)

    abutting_cases = 0
    native_obs_cases = 0
    sparse_mask = bytes(0x80 if i in (0, 7, 15) else (0x7F if i % 2 else 0x00) for i in range(16)) + b"\x00" * 16
    for guard_prot in (PAGE_NOACCESS, PAGE_READONLY):
        base = k32.VirtualAlloc(None, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
        if not base:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            old_prot = wintypes.DWORD()
            if not k32.VirtualProtect(base + 4096, 4096, guard_prot, ctypes.byref(old_prot)):
                raise ctypes.WinError(ctypes.get_last_error())

            # Page-abutting writable case (k=16): both native and lifted must succeed and match.
            ptr_dst = base + 4096 - 16
            for mask_name, mask_bytes in (
                ("full_mask", b"\x80" * 16 + b"\x00" * 16),
                ("all_zero_mask", b"\x7F" * 16 + b"\x00" * 16),
                ("sparse_mask", sparse_mask),
            ):
                buf_mask, ptr_mask, _, _, _ = alloc_aligned_buffer(mask_bytes)
                want = ref_vmaskmovdqu(src_bytes[:16], mask_bytes[:16], b"\x5A" * 16)
                for label, fn in (("native", native_fns["op_vmaskmovdqu"]), ("lifted", lifted_fns["op_vmaskmovdqu"])):
                    ctypes.memset(base + 4096 - 32, 0x5A, 32)
                    fn(ptr_src, ptr_mask, ptr_dst)
                    after = bytes((ctypes.c_ubyte * 16).from_address(ptr_dst))
                    assert after == want, (
                        f"{label} k=16 page-abutting mismatch ({mask_name}): {after.hex()} != {want.hex()}"
                    )
                    abutting_cases += 1
                _ = buf_mask

            # Partial-vector page-edge probe (k<16): strictly native observation (implementation-dependent).
            for k in (15, 8, 7, 1, 0):
                ptr_dst = base + 4096 - k
                masks = (
                    ("only_writable_masked", bytes(0x80 if i < k else 0x7F for i in range(16)) + b"\x00" * 16),
                    ("all_zero_mask", b"\x7F" * 16 + b"\x00" * 16),
                    ("edge_byte_masked", bytes(0x80 if i in (0, k) else 0x00 for i in range(16)) + b"\x00" * 16),
                )
                for mask_name, mask_bytes in masks:
                    buf_mask, ptr_mask, _, _, _ = alloc_aligned_buffer(mask_bytes)
                    ctypes.memset(base + 4096 - 32, 0x5A, 32)
                    faulted = False
                    try:
                        native_fns["op_vmaskmovdqu"](ptr_src, ptr_mask, ptr_dst)
                    except OSError as error:
                        assert "access violation" in str(error).lower(), (guard_prot, k, mask_name, error)
                        faulted = True
                    selects_guard = any(byte & 0x80 for byte in mask_bytes[k:16])
                    assert faulted or not selects_guard, (guard_prot, k, mask_name)
                    after = bytes((ctypes.c_ubyte * k).from_address(ptr_dst))
                    expected = ref_vmaskmovdqu(src_bytes[:16], mask_bytes[:16], b"\x5A" * 16)[:k]
                    if faulted:
                        assert all(actual in (0x5A, wanted) for actual, wanted in zip(after, expected)), (
                            guard_prot, k, mask_name, after.hex(), expected.hex()
                        )
                    else:
                        assert after == expected, (guard_prot, k, mask_name, after.hex(), expected.hex())
                    print(f"native page edge prot={guard_prot:#x} k={k} {mask_name}: "
                          f"faulted={int(faulted)}, prefix={after.hex()}")
                    native_obs_cases += 1
                    _ = buf_mask
        finally:
            k32.VirtualFree(base, 0, MEM_RELEASE)
    _ = buf_src
    return abutting_cases, native_obs_cases


def check_differential(native_path: Path, lifted_path: Path, altered_path: Path) -> None:
    lifted_lib, lifted = bind_exports(lifted_path, "lifted_")
    lifted_lib.host_has_avx.restype = ctypes.c_uint32
    if not lifted_lib.host_has_avx():
        print("host lacks AVX/OSXSAVE; skipping native differential check")
        raise SystemExit(77)

    check_discarded_read(native_path, lifted_path)
    abutting_cases, native_obs_cases = check_vmaskmovdqu_page_edge(native_path, lifted_path)

    _, native = bind_exports(native_path)
    _, altered = bind_exports(altered_path, "lifted_")

    total = 0
    for name in EXPORTS:
        for idx, (a, b) in enumerate(inputs_for(name)):
            want = reference(name, a, b)
            got_n = invoke(native[name], a, b)
            got_l = invoke(lifted[name], a, b)
            assert_equal_output("native", name, idx, got_n, want)
            assert_equal_output("lifted", name, idx, got_l, want)
            total += 1

    # Altered-op negative controls.
    diverged = {}
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
        diverged[name] = diffs

    # Output mutation negative control.
    sample_a, sample_b = maskmov_pairs()[4]
    expected = reference("op_pipeline", sample_a, sample_b)
    mutated = bytearray(expected)
    mutated[5] ^= 0x01
    try:
        assert_equal_output("mutation", "op_pipeline", 0, bytes(mutated), expected)
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    print(
        f"{total} native/lifted/reference AVX move cases across {len(EXPORTS)} exports matched bit-for-bit; "
        f"{abutting_cases} k=16 page-abutting checks and {native_obs_cases} native k<16 page-edge observations; "
        f"ALTER_OP diverged on {diverged}; dead-read and output mutation controls passed"
    )


# --- Alternate-encoding acceptance & translator rejection controls ---

def encode_seq(*instructions) -> bytes:
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_alt_encodings_and_rejections(original_path: Path) -> None:
    ld_xmm0 = Instruction.create_reg_mem(Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    ld_xmm1 = Instruction.create_reg_mem(Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM1, MemoryOperand(R.RDX))
    ld_ymm4 = Instruction.create_reg_mem(Code.VEX_VMOVUPS_YMM_YMMM256, R.YMM4, MemoryOperand(R.RCX))
    st_xmm0 = Instruction.create_mem_reg(Code.VEX_VMOVUPS_XMMM128_XMM, MemoryOperand(R.R8), R.XMM0)
    ret_ins = Instruction.create(Code.RETNQ)

    # Alternate encodings not emitted directly by MASM:
    # F3 0F 7E vmovq xmm, m64; 66 0F 6E W1 vmovq xmm, m64; 66 0F D6 vmovq xmm, xmm and m64, xmm;
    # store-form reg-reg vmovups/vmovaps/vmovdqu/vmovdqa; r8 reads; neg disp; r9 rdi save.
    alt_accept = [
        ("vmovq_f3_m64", (
            Instruction.create_reg_mem(Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, MemoryOperand(R.RDX, displ=8, displ_size=1)),
            st_xmm0, ret_ins)),
        ("vmovq_6e_m64", (
            Instruction.create_reg_mem(Code.VEX_VMOVQ_XMM_RM64, R.XMM0, MemoryOperand(R.RDX, displ=8, displ_size=1)),
            st_xmm0, ret_ins)),
        ("vmovq_d6_reg", (
            ld_xmm1, Instruction.create_reg_reg(Code.VEX_VMOVQ_XMMM64_XMM, R.XMM0, R.XMM1),
            st_xmm0, ret_ins)),
        ("vmovq_d6_m64", (
            ld_xmm0, Instruction.create_mem_reg(Code.VEX_VMOVQ_XMMM64_XMM, MemoryOperand(R.R8, displ=16, displ_size=1), R.XMM0),
            ret_ins)),
        ("vmovq_gpr_r10_r8", (
            Instruction.create_reg_reg(Code.VEX_VMOVQ_XMM_RM64, R.XMM0, R.R8),
            Instruction.create_reg_reg(Code.VEX_VMOVQ_RM64_XMM, R.R10, R.XMM0),
            Instruction.create_reg_reg(Code.VEX_VMOVQ_XMM_RM64, R.XMM2, R.R10),
            Instruction.create_mem_reg(Code.VEX_VMOVQ_RM64_XMM, MemoryOperand(R.R8), R.XMM2),
            ret_ins)),
        ("vmovq_r8_neg_disp", (
            Instruction.create_reg_mem(Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, MemoryOperand(R.R8, displ=-16, displ_size=1)),
            st_xmm0, ret_ins)),
        ("vmov_alt_reg_reg", (
            ld_ymm4,
            Instruction.create_reg_reg(Code.VEX_VMOVUPS_XMMM128_XMM, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.VEX_VMOVAPS_YMMM256_YMM, R.YMM1, R.YMM4),
            Instruction.create_reg_reg(Code.VEX_VMOVDQU_XMMM128_XMM, R.XMM2, R.XMM0),
            Instruction.create_reg_reg(Code.VEX_VMOVDQA_YMMM256_YMM, R.YMM3, R.YMM1),
            Instruction.create_mem_reg(Code.VEX_VMOVDQU_YMMM256_YMM, MemoryOperand(R.R8), R.YMM3),
            ret_ins)),
        ("vmaskmovdqu_r9_save", (
            ld_xmm0, ld_xmm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R9, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_vmaskmovdqu(64, R.XMM0, R.XMM1),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R9),
            ret_ins)),
    ]
    alt_accept += [
        # vmovupd/vmovapd: loads, stores, and the store-form (0F 11 / 0F 29) register copies.
        ("vmovpd_load_store", (
            Instruction.create_reg_mem(Code.VEX_VMOVUPD_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, displ=1, displ_size=1)),
            Instruction.create_reg_mem(Code.VEX_VMOVAPD_YMM_YMMM256, R.YMM1, MemoryOperand(R.RDX, displ=32, displ_size=1)),
            Instruction.create_mem_reg(Code.VEX_VMOVUPD_XMMM128_XMM, MemoryOperand(R.R8, displ=1, displ_size=1), R.XMM0),
            Instruction.create_mem_reg(Code.VEX_VMOVAPD_YMMM256_YMM, MemoryOperand(R.R8, displ=32, displ_size=1), R.YMM1),
            ret_ins)),
        ("vmovpd_alt_reg_reg", (
            ld_ymm4,
            Instruction.create_reg_reg(Code.VEX_VMOVUPD_XMMM128_XMM, R.XMM0, R.XMM4),
            Instruction.create_reg_reg(Code.VEX_VMOVAPD_YMMM256_YMM, R.YMM1, R.YMM4),
            Instruction.create_reg_reg(Code.VEX_VMOVAPD_XMMM128_XMM, R.XMM2, R.XMM0),
            Instruction.create_reg_reg(Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM3, R.YMM1),
            Instruction.create_mem_reg(Code.VEX_VMOVUPD_YMMM256_YMM, MemoryOperand(R.R8), R.YMM3),
            ret_ins)),
    ]
    alt_expected = {
        "vmovpd_load_store": "ymm0 = load128((rcx + 0x1ull));",
        "vmovpd_alt_reg_reg": "ymm2 = copy128(ymm0);",
        "vmovq_f3_m64": "ymm0 = {{load64((rdx + 0x8ull)), 0ull, 0ull, 0ull}};",
        "vmovq_6e_m64": "ymm0 = {{load64((rdx + 0x8ull)), 0ull, 0ull, 0ull}};",
        "vmovq_d6_reg": "ymm0 = {{ymm1.q[0], 0ull, 0ull, 0ull}};",
        "vmovq_d6_m64": "store64((r8 + 0x10ull), ymm0.q[0]);",
        "vmovq_gpr_r10_r8": "r10 = ymm0.q[0];",
        "vmovq_r8_neg_disp": "ymm0 = {{load64((r8 + 0xfffffffffffffff0ull)), 0ull, 0ull, 0ull}};",
        "vmov_alt_reg_reg": "ymm0 = copy128(ymm4);",
        "vmaskmovdqu_r9_save": "op_vmaskmovdqu(r8, ymm0, ymm1);",
    }
    for label, seq in alt_accept:
        raw = encode_seq(*seq)
        decoded = list(Decoder(64, raw, ip=0x1000))
        assert len(decoded) == len(seq) and not any(ins.is_invalid for ins in decoded), label
        src, count = lift_code(label, 0x1000, raw)
        assert f"lifted_{label}" in src and count == len(seq), (label, count)
        if alt_expected[label] not in src:
            raise AssertionError(f"{label}: expected {alt_expected[label]!r} in generated C++")

    evex_vmovq = Instruction.create_reg_reg(Code.EVEX_VMOVQ_XMM_RM64, R.XMM0, R.RAX)
    evex_vaddps = Instruction.create_reg_reg_reg(Code.EVEX_VADDPS_XMM_K1Z_XMM_XMMM128B32, R.XMM0, R.XMM1, R.XMM2)
    evex_vaddps.op_mask = R.K1
    seg_load = Instruction.create_reg_mem(Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))
    seg_load.segment_prefix = R.GS

    evex_vmovapd = Instruction.create_reg_mem(
        Code.EVEX_VMOVAPD_YMM_K1Z_YMMM256, R.YMM0, MemoryOperand(R.RCX))
    evex_vmovupd = Instruction.create_mem_reg(
        Code.EVEX_VMOVUPD_XMMM128_K1Z_XMM, MemoryOperand(R.R8), R.XMM0)

    unit_cases = [
        # vmovupd/vmovapd rejections mirror the vmovups/vmovaps ones
        ("evex_vmovapd", encode_seq(evex_vmovapd, ret_ins), "unsupported EVEX instruction"),
        ("evex_vmovupd_store", encode_seq(ld_xmm0, evex_vmovupd, ret_ins), "unsupported EVEX instruction"),
        ("nonvolatile_xmm6_vmovapd", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPD_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("store_outside_r8_vmovapd", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVAPD_YMMM256_YMM, MemoryOperand(R.RDX), R.YMM0), ret_ins), "non-r8 store memory operand"),
        ("store_outside_r8_vmovupd", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVUPD_XMMM128_XMM, MemoryOperand(R.RCX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("non_arg_base_rax_vmovapd", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPD_XMM_XMMM128, R.XMM0, MemoryOperand(R.RAX)), ret_ins), "unsupported memory base"),
        ("nonvolatile_base_rbx_vmovupd", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.RBX)), ret_ins), "nonvolatile GPR base"),
        ("rip_rel_vmovapd", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPD_XMM_XMMM128, R.XMM0, MemoryOperand(R.RIP, displ=0x1020, displ_size=1)),
            ret_ins), "unsupported memory operand"),
        ("indexed_vmovupd", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, R.RDX, 4)), ret_ins), "unsupported memory operand"),
        ("indexed_vmovapd_store", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVAPD_XMMM128_XMM, MemoryOperand(R.R8, R.RDX, 2), R.XMM0), ret_ins), "unsupported memory operand"),
        ("unaligned_vmovapd_xmm_load", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPD_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, displ=1, displ_size=1)), ret_ins),
            "unaligned 16-byte memory operand"),
        ("unaligned_vmovapd_ymm_load", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX, displ=16, displ_size=1)), ret_ins),
            "unaligned 32-byte memory operand"),
        ("unaligned_vmovapd_xmm_store", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVAPD_XMMM128_XMM, MemoryOperand(R.R8, displ=8, displ_size=1), R.XMM0), ret_ins),
            "unaligned 16-byte memory operand"),
        ("unaligned_vmovapd_ymm_store", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVAPD_YMMM256_YMM, MemoryOperand(R.R8, displ=16, displ_size=1), R.YMM0), ret_ins),
            "unaligned 32-byte memory operand"),
        # Legacy SSE rejections
        ("legacy_movq_xmm", encode_seq(ld_xmm1, Instruction.create_reg_reg(
            Code.MOVQ_XMM_XMMM64, R.XMM0, R.XMM1), ret_ins), "unsupported legacy SSE encoding"),
        ("legacy_movq_r64", encode_seq(ld_xmm0, Instruction.create_reg_reg(
            Code.MOVQ_RM64_XMM, R.RAX, R.XMM0), ret_ins), "unsupported legacy SSE encoding"),
        ("legacy_movq_mm", encode_seq(Instruction.create_reg_mem(
            Code.MOVQ_MM_MMM64, R.MM0, MemoryOperand(R.RCX)), ret_ins), "unsupported legacy SSE encoding"),
        ("legacy_maskmovdqu", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_maskmovdqu(
            64, R.XMM0, R.XMM1), ret_ins), "unsupported legacy SSE encoding"),
        # EVEX rejections
        ("evex_vmovq", encode_seq(Instruction.create_reg_mem(
            Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.RCX)), evex_vmovq, ret_ins), "unsupported EVEX instruction"),
        ("evex_vaddps", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_reg_reg(
            Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM2, R.XMM0), evex_vaddps, ret_ins), "unsupported EVEX instruction"),
        # Nonvolatile XMM6..15 / YMM6..15 and GPRs
        ("nonvolatile_xmm6", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_ymm15", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPS_YMM_YMMM256, R.YMM15, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_rbx_vmovq", encode_seq(ld_xmm0, Instruction.create_reg_reg(
            Code.VEX_VMOVQ_RM64_XMM, R.RBX, R.XMM0), ret_ins), "nonvolatile GPR"),
        ("nonvolatile_rdi_vmovq", encode_seq(ld_xmm0, Instruction.create_reg_reg(
            Code.VEX_VMOVQ_RM64_XMM, R.RDI, R.XMM0), ret_ins), "nonvolatile GPR"),
        # RDI save/restore, definedness leak, and vmaskmovdqu policy
        ("rdi_save_clobbers_prior_def", encode_seq(
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RCX),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RDI),
            Instruction.create_reg_reg(Code.VEX_VMOVQ_XMM_RM64, R.XMM0, R.RAX),
            st_xmm0, ret_ins), "GPR rax holds saved rdi"),
        ("rdi_save_post_restore_still_unavail", encode_seq(
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RAX, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.RAX),
            Instruction.create_reg_reg(Code.VEX_VMOVQ_XMM_RM64, R.XMM0, R.RAX),
            st_xmm0, ret_ins), "GPR rax holds saved rdi"),
        ("vmaskmovdqu_no_r8_rdi", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_vmaskmovdqu(
            64, R.XMM0, R.XMM1), ret_ins), "vmaskmovdqu requires r8-rooted rdi"),
        ("vmaskmovdqu_unsaved_rdi", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_reg_reg(
            Code.MOV_R64_RM64, R.RDI, R.R8), Instruction.create_vmaskmovdqu(
            64, R.XMM0, R.XMM1), ret_ins), "rdi modified without saving"),
        ("vmaskmovdqu_unrestored_rdi", encode_seq(ld_xmm0, ld_xmm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_vmaskmovdqu(64, R.XMM0, R.XMM1), ret_ins), "unrestored nonvolatile GPR rdi"),
        ("vmaskmovdqu_non_r8_rdi", encode_seq(ld_xmm0, ld_xmm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.RCX),
            ret_ins), "non-r8 or unsaved write to nonvolatile GPR rdi"),
        ("vmaskmovdqu_clobbered_save_slot", encode_seq(ld_xmm0, ld_xmm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8),
            Instruction.create_reg_reg(Code.VEX_VMOVQ_RM64_XMM, R.R10, R.XMM0),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10),
            ret_ins), "non-r8 or unsaved write to nonvolatile GPR rdi"),
        ("vmaskmovdqu_edi_override", encode_seq(ld_xmm0, ld_xmm1,
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.R10, R.RDI),
            Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R8))
            + bytes.fromhex("67c5f9f7c1")
            + encode_seq(Instruction.create_reg_reg(Code.MOV_R64_RM64, R.RDI, R.R10), ret_ins),
            "unsupported vmaskmovdqu target"),
        # Store target / write to r8 / addressing / alignment
        ("store_outside_r8_vmovq", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVQ_RM64_XMM, MemoryOperand(R.RCX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("store_outside_r8_vmovups", encode_seq(ld_xmm0, Instruction.create_mem_reg(
            Code.VEX_VMOVUPS_XMMM128_XMM, MemoryOperand(R.RDX), R.XMM0), ret_ins), "non-r8 store memory operand"),
        ("write_r8_vmovq", encode_seq(ld_xmm0, Instruction.create_reg_reg(
            Code.VEX_VMOVQ_RM64_XMM, R.R8, R.XMM0), ret_ins), "unsupported write to output buffer pointer r8"),
        ("non_arg_base_rax", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RAX)), ret_ins), "unsupported memory base"),
        ("nonvolatile_base_rbx", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RBX)), ret_ins), "nonvolatile GPR base"),
        ("clobbered_rcx_read", encode_seq(
            Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RCX, MemoryOperand(R.RDX)),
            ld_xmm0, ret_ins), "memory operand uses clobbered pointer rcx"),
        ("unaligned_vmovaps", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVAPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, displ=1, displ_size=1)), ret_ins),
            "unaligned 16-byte memory operand"),
        ("rip_rel_mem", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, MemoryOperand(R.RIP, displ=0x1020, displ_size=1)),
            ret_ins), "unsupported memory operand"),
        ("indexed_mem", encode_seq(Instruction.create_reg_mem(
            Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX, R.RDX, 4)), ret_ins), "unsupported memory operand"),
        # Prefixes, out-of-scope (including vaddsubps/vaddsubpd), undefined, stream boundary
        ("segment_prefix", encode_seq(seg_load, ret_ins), "unsupported instruction prefix"),
        ("lock_prefix", bytes.fromhex("f00fb109") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("rep_prefix", bytes.fromhex("f3a4") + encode_seq(ret_ins), "unsupported instruction prefix"),
        ("out_of_scope_vaddsubps", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_reg_reg_reg(
            Code.VEX_VADDSUBPS_XMM_XMM_XMMM128, R.XMM0, R.XMM0, R.XMM1), ret_ins), "unsupported instruction"),
        ("out_of_scope_vaddps", encode_seq(ld_xmm0, ld_xmm1, Instruction.create_reg_reg_reg(
            Code.VEX_VADDPS_XMM_XMM_XMMM128, R.XMM0, R.XMM0, R.XMM1), ret_ins), "unsupported instruction"),
        ("undef_xmm2", encode_seq(Instruction.create_reg_reg(
            Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, R.XMM2), ret_ins), "undefined vector register ymm2"),
        ("undef_rax", encode_seq(Instruction.create_reg_reg(
            Code.VEX_VMOVQ_XMM_RM64, R.XMM0, R.RAX), ret_ins), "undefined GPR rax"),
        ("missing_ret", encode_seq(ld_xmm0), "missing terminal ret"),
        ("trailing_after_ret", encode_seq(ld_xmm0, ret_ins, ld_xmm1), "unreachable instruction after ret"),
        ("empty_stream", b"", "truncated instruction stream"),
        ("invalid_bytes", bytes.fromhex("fffe") + encode_seq(ret_ins), "invalid instruction"),
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
        entry = export_rva(pe, "op_pipeline")
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
        ("pe_legacy_movq", unit_map["legacy_movq_xmm"], "unsupported legacy SSE encoding"),
        ("pe_legacy_maskmovdqu", unit_map["legacy_maskmovdqu"], "unsupported legacy SSE encoding"),
        ("pe_evex_vmovq", unit_map["evex_vmovq"], "unsupported EVEX instruction"),
        ("pe_nonvolatile_ymm15", unit_map["nonvolatile_ymm15"], "nonvolatile vector register"),
        ("pe_rdi_save_leak", unit_map["rdi_save_clobbers_prior_def"], "GPR rax holds saved rdi"),
        ("pe_unrestored_rdi", unit_map["vmaskmovdqu_unrestored_rdi"], "unrestored nonvolatile GPR rdi"),
        ("pe_store_outside_r8", unit_map["store_outside_r8_vmovq"], "non-r8 store memory operand"),
        ("pe_rip_rel", unit_map["rip_rel_mem"], "unsupported memory operand"),
        ("pe_evex_vmovapd", unit_map["evex_vmovapd"], "unsupported EVEX instruction"),
        ("pe_unaligned_vmovapd", unit_map["unaligned_vmovapd_ymm_load"], "unaligned 32-byte memory operand"),
        ("pe_non_arg_vmovapd", unit_map["non_arg_base_rax_vmovapd"], "unsupported memory base"),
        ("pe_trailing_after_ret", unit_map["trailing_after_ret"], "unreachable instruction after ret"),
    ]
    with TemporaryDirectory() as folder:
        patched_path = Path(folder) / "patched.dll"
        for label, patch, expected_reason in pe_patches:
            if len(patch) > max_len:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds op_pipeline ({max_len})")
            damaged = bytearray(pe_bytes)
            damaged[file_off : file_off + len(patch)] = patch
            struct.pack_into("<I", damaged, pdata_end_off, entry + len(patch))
            patched_path.write_bytes(damaged)
            try:
                translate(patched_path, ["op_pipeline"])
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
    orig, lifted, alt = (Path(p) for p in sys.argv[1:4])
    check_differential(orig, lifted, alt)
    check_alt_encodings_and_rejections(orig)
