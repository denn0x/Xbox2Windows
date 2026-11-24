"""Differential check for PF after VEX scalar compares (VCOMISS/VUCOMISS/VCOMISD/VUCOMISD)
and PF consumers (SETP/SETNP/CMOVP/CMOVNP/JP/JNP), including /O2, fault checks, and mutated controls.

Usage:
    python tests/cpu/vex_parity.py emit <dll> <cpp> [control]
    python tests/cpu/vex_parity.py check <guard_dll> <native_dll> <lifted_dll> <o2_dll> [controls...]
"""
import ctypes
from pathlib import Path
import platform
import re
import struct
import sys

import pefile
from iced_x86 import Decoder

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from cpu_leaf_aot import export_rva  # noqa: E402
from cpu_ranges import function_ranges  # noqa: E402
from ffx_version_query import INTEGER_HEADER, lower, region  # noqa: E402

FLAG_EXPORTS = (
    "vcomiss_reg_flags", "vcomiss_mem_flags",
    "vucomiss_reg_flags", "vucomiss_mem_flags",
    "vcomisd_reg_flags", "vcomisd_mem_flags",
    "vucomisd_reg_flags", "vucomisd_mem_flags",
)
FOUR_ARG_EXPORTS = FLAG_EXPORTS + (
    "setp_reg_bytes",
    "cmov_reg_outcomes",
    "join_vcomi_pf",
    "back_to_back_vcomi",
)
THREE_ARG_EXPORTS = ("cmovp_mem32", "cmovnp_mem32")
ALL_EXPORTS = FOUR_ARG_EXPORTS + THREE_ARG_EXPORTS

PAGE = 0x1000
ACCESS_VIOLATION = 0xC0000005
MXCSR_ALL_MASKED = 0x1F80  # IE..PE masked, round-to-nearest, DAZ=0, FTZ=0

CPUID_HELPER = """#include <intrin.h>
extern "C" __declspec(dllexport) uint32_t host_has_avx() {
    int info[4]{};
    __cpuid(info, 1);
    const int need = (1 << 27) | (1 << 28);
    if ((info[2] & need) != need) return 0u;
    return (_xgetbv(0) & 6u) == 6u ? 1u : 0u;
}
extern "C" __declspec(dllexport) uint32_t host_exchange_mxcsr(uint32_t next) {
    const uint32_t current = _mm_getcsr();
    _mm_setcsr(next);
    return current;
}
"""

PARITY_USE_RE = re.compile(r"(?<!bool )(?<![a-zA-Z0-9_])(!?parity)(?!\s*=)")


def mutate_control(cpp: str, control: str) -> str:
    if control == "lifted":
        return cpp
    if control == "stalepf":
        mutated, count = re.subn(r" parity = unordered;", "", cpp)
        if count == 0:
            raise AssertionError("stalepf control matched zero 'parity = unordered;' statements")
        return mutated
    if control == "swap":
        mutated, count = PARITY_USE_RE.subn(
            lambda m: "parity" if m.group(1).startswith("!") else "!parity", cpp
        )
        if count == 0:
            raise AssertionError("swap control matched zero PF consumer conditions")
        return mutated
    raise ValueError(f"unknown control variant: {control}")


def emit(native_dll: Path, output_cpp: Path, control: str = "lifted") -> None:
    parts = [INTEGER_HEADER, CPUID_HELPER]
    with pefile.PE(str(native_dll), fast_load=True) as pe:
        for name in FOUR_ARG_EXPORTS:
            ranges = function_ranges(pe, export_rva(pe, name))
            body = [ins for lo, hi in ranges for ins in region(pe, lo, hi)]
            code = lower(body, ["rcx = arg_a; rdx = arg_b; r8 = arg_dst; r9 = arg_extra;"], {}, {}, {})
            parts.append(
                f'extern "C" __declspec(dllexport) uint64_t {name}(uint64_t arg_a, uint64_t arg_b, uint64_t arg_dst, uint64_t arg_extra) {{\n{code}\n}}'
            )
        for name in THREE_ARG_EXPORTS:
            ranges = function_ranges(pe, export_rva(pe, name))
            body = [ins for lo, hi in ranges for ins in region(pe, lo, hi)]
            code = lower(body, ["rcx = arg_a; rdx = arg_b; r8 = reinterpret_cast<word>(arg_trace);"], {}, {}, {})
            parts.append(
                f'extern "C" __declspec(dllexport) word {name}(word arg_a, word arg_b, word* arg_trace) {{\n{code}\n}}'
            )
    text = mutate_control("\n".join(parts) + "\n", control)
    output_cpp.write_text(text, encoding="utf-8")


def decode_check(hex_bytes: str) -> list:
    body = list(Decoder(64, bytes.fromhex(hex_bytes), ip=0x1000))
    if any(i.is_invalid for i in body):
        raise AssertionError(f"invalid instruction in test bytes {hex_bytes!r}: {body}")
    return body


def check_rejections() -> None:
    def attempt(hex_bytes: str, calls=None, host_ops=None):
        body = decode_check(hex_bytes)
        try:
            lower(body, [], {}, calls or {}, {}, host_ops=host_ops)
        except ValueError as exc:
            return str(exc)
        return None

    vcomiss = "c5 f8 2f c1"   # vcomiss xmm0, xmm1
    vucomiss = "c5 f8 2e c1"  # vucomiss xmm0, xmm1
    vcomisd = "c5 f9 2f c1"   # vcomisd xmm0, xmm1
    vucomisd = "c5 f9 2e c1"  # vucomisd xmm0, xmm1
    setp_al = "0f 9a c0"      # setp al
    setnp_al = "0f 9b c0"     # setnp al
    cmovp = "0f 4a c2"        # cmovp eax, edx
    cmovnp = "0f 4b c2"       # cmovnp eax, edx
    jp_self = "7a 00"         # jp $+2
    ret = "c3"

    undef_msg = "reads PF, which no modeled PF producer defines on every path from the entry"
    indirect_msg = "unbound branch at"

    rejected = {
        "SETP AL at entry": (attempt(f"{setp_al} {ret}"), undef_msg),
        "CMOVP EAX, EDX at entry": (attempt(f"{cmovp} {ret}"), undef_msg),
        "JP at entry": (attempt(f"{jp_self} {ret}"), undef_msg),
        "VCOMISS + ADD + SETP": (attempt(f"{vcomiss} 01 d0 {setp_al} {ret}"), undef_msg),
        "VUCOMISS + CMP + CMOVNP": (attempt(f"{vucomiss} 39 d0 {cmovnp} {ret}"), undef_msg),
        "VCOMISD + INC + JP": (attempt(f"{vcomisd} ff c0 {jp_self} {ret}"), undef_msg),
        "VUCOMISD + TEST32 + SETNP": (attempt(f"{vucomisd} 85 c0 {setnp_al} {ret}"), undef_msg),
        "VCOMISS + SHL + CMOVP": (attempt(f"{vcomiss} d1 e0 {cmovp} {ret}"), undef_msg),
        "VCOMISS + CALL + SETP": (attempt(f"{vcomiss} e8 00 00 00 00 {setp_al} {ret}", calls={0x1004: ";"}), undef_msg),
        # JRCXZ (e3 04) skips VCOMISS (4 bytes) + JMP+2 (2 bytes -> e3 06) into ADD (01 d0).
        "join VCOMISS path and ADD path": (attempt(f"e3 06 {vcomiss} eb 02 01 d0 {setp_al} {ret}"), undef_msg),
        # JRCXZ skips VCOMISS + JMP+2 into MOV EAX, EDX (leaves PF undefined).
        "join VCOMISS path and MOV path": (attempt(f"e3 06 {vcomiss} eb 02 89 d0 {cmovp} {ret}"), undef_msg),
        "join VCOMISS path and TEST AL, AL path": (attempt(f"e3 06 {vcomiss} eb 02 84 c0 {setnp_al} {ret}"), undef_msg),
        "VCOMISS + 8-bit AND CL, 1 + CMOVNP": (attempt(f"{vcomiss} 80 e1 01 {cmovnp} {ret}"), undef_msg),
        # VCOMISS + SETP + JMP RAX (indirect branch).
        "indirect branch with PF consumer": (attempt(f"{vcomiss} {setp_al} ff e0"), indirect_msg),
        # host_ops replacing a NOP invalidates tracked flags/PF.
        "VCOMISS + host_ops + SETP": (attempt(f"{vcomiss} 90 {setp_al} {ret}", host_ops={0x1004: "rax = 1;"}), undef_msg),
    }
    for case, (msg, needle) in rejected.items():
        if msg is None or needle not in msg:
            raise AssertionError(f"{case}: expected rejection with {needle!r}, got {msg!r}")

    accepted = {
        "VCOMISS + MOV + LEA + VZEROUPPER + SETP": f"{vcomiss} 89 d0 8d 40 01 c5 f8 77 {setp_al} {ret}",
        "join VCOMISS path and VUCOMISD path": f"e3 06 {vcomiss} eb 04 {vucomisd} {cmovp} {ret}",
    }
    for case, hex_bytes in accepted.items():
        body = decode_check(hex_bytes)
        lower(body, [], {}, {}, {})

    # Functions without PF consumers must not emit parity tracking.
    no_pf_body = decode_check(f"{vcomiss} 0f 94 c0 {ret}")  # vcomiss + sete al + ret
    no_pf_cpp = lower(no_pf_body, [], {}, {}, {})
    if "parity" in no_pf_cpp:
        raise AssertionError(f"unexpected parity tracking emitted without PF consumer:\n{no_pf_cpp}")

    print(f"rejected {len(rejected)} negative PF cases, accepted {len(accepted)} positive sequences, and preserved parity-free lowering")


def ref_cmp_bits(bits_a: int, bits_b: int, single: bool):
    if single:
        a = struct.unpack("<f", struct.pack("<I", bits_a))[0]
        b = struct.unpack("<f", struct.pack("<I", bits_b))[0]
        abs_mask, inf_bits = 0x7FFFFFFF, 0x7F800000
    else:
        a = struct.unpack("<d", struct.pack("<Q", bits_a))[0]
        b = struct.unpack("<d", struct.pack("<Q", bits_b))[0]
        abs_mask, inf_bits = 0x7FFFFFFFFFFFFFFF, 0x7FF0000000000000
    unordered = (bits_a & abs_mask) > inf_bits or (bits_b & abs_mask) > inf_bits
    if unordered:
        pf, zf, cf = 1, 1, 1
    elif a == b:
        pf, zf, cf = 0, 1, 0
    elif a < b:
        pf, zf, cf = 0, 0, 1
    else:
        pf, zf, cf = 0, 0, 0
    return bytes((pf, pf ^ 1, zf, cf, pf, pf ^ 1, 1 if (pf == 0 and zf == 1) else 0))


def configure_lib(path: Path):
    lib = ctypes.CDLL(str(path.resolve()))
    for name in FOUR_ARG_EXPORTS:
        fn = getattr(lib, name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = ctypes.c_uint64
    for name in THREE_ARG_EXPORTS:
        fn = getattr(lib, name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)]
        fn.restype = ctypes.c_uint64
    return lib


F32_VALUES = (
    ("neg_2.5", 0xC0200000),
    ("neg_zero", 0x80000000),
    ("pos_zero", 0x00000000),
    ("subnormal", 0x00000001),
    ("one", 0x3F800000),
    ("two", 0x40000000),
    ("neg_inf", 0xFF800000),
    ("pos_inf", 0x7F800000),
    ("qnan_pos", 0x7FC00000),
    ("qnan_neg", 0xFFC01234),
    ("snan_pos", 0x7F800001),
    ("snan_neg", 0xFF801234),
)

F64_VALUES = (
    ("neg_3.5", 0xC00C000000000000),
    ("neg_zero", 0x8000000000000000),
    ("pos_zero", 0x0000000000000000),
    ("subnormal", 0x0000000000000001),
    ("one", 0x3FF0000000000000),
    ("two", 0x4000000000000000),
    ("neg_inf", 0xFFF0000000000000),
    ("pos_inf", 0x7FF0000000000000),
    ("qnan_pos", 0x7FF8000000000000),
    ("qnan_neg", 0xFFF8000000001234),
    ("snan_pos", 0x7FF0000000000001),
    ("snan_neg", 0xFFF0000000001234),
)


def check_cmov_faults(guard_path: Path, sides) -> int:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel32.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]

    guarded = ctypes.CDLL(str(guard_path.resolve())).guarded_call
    guarded.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                        ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
    guarded.restype = ctypes.c_uint32

    base = kernel32.VirtualAlloc(None, 2 * PAGE, 0x3000, 0x04)  # MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    if not base:
        raise ctypes.WinError(ctypes.get_last_error())
    cases = 0
    try:
        old = ctypes.c_uint32()
        if not kernel32.VirtualProtect(base + PAGE, PAGE, 0x01, ctypes.byref(old)):  # PAGE_NOACCESS
            raise ctypes.WinError(ctypes.get_last_error())
        readable_ptr = base + PAGE - 4
        ctypes.c_uint32.from_address(readable_ptr).value = 0xFEDCBA98
        trace = (ctypes.c_uint64 * 4)(0, 0, 0, 0)

        pairs = (
            ("ordered_lt", 0x3F800000, 0x40000000, 0),
            ("ordered_eq", 0x3F800000, 0x3F800000, 0),
            ("unordered_qnan_a", 0x7FC00000, 0x3F800000, 1),
            ("unordered_snan_b", 0x3F800000, 0x7F800001, 1),
        )
        for pair_name, bits_a, bits_b, pf in pairs:
            pair_buf = (ctypes.c_uint32 * 2)(bits_a, bits_b)
            for fn_name, take_on_pf1 in (("cmovp_mem32", True), ("cmovnp_mem32", False)):
                taken = (pf == 1) if take_on_pf1 else (pf == 0)
                want_val = 0x00000000FEDCBA98 if taken else 0x0000000055667788
                for mem_label, mem_ptr, want_code in (
                    ("readable", readable_ptr, 0),
                    ("page_noaccess", base + PAGE, ACCESS_VIOLATION),
                    ("null", 0, ACCESS_VIOLATION),
                ):
                    for side_label, lib in sides:
                        fn = getattr(lib, fn_name)
                        res = ctypes.c_uint64(0xDEADDEADDEADDEAD)
                        code = guarded(
                            ctypes.cast(fn, ctypes.c_void_p),
                            ctypes.addressof(pair_buf),
                            mem_ptr,
                            trace,
                            ctypes.byref(res),
                        )
                        if code != want_code:
                            raise AssertionError(
                                f"{side_label} {fn_name} ({pair_name}, {mem_label}): code={code:#x}, expected={want_code:#x}"
                            )
                        if want_code == 0 and res.value != want_val:
                            raise AssertionError(
                                f"{side_label} {fn_name} ({pair_name}, {mem_label}): result={res.value:#018x}, expected={want_val:#018x}"
                            )
                    cases += 1
    finally:
        kernel32.VirtualFree(base, 0, 0x8000)  # MEM_RELEASE
    return cases


def check(guard_path: Path, native_path: Path, lifted_path: Path, o2_path: Path, *control_paths: Path) -> None:
    check_rejections()

    lifted = configure_lib(lifted_path)
    lifted.host_has_avx.restype = ctypes.c_uint32
    if not lifted.host_has_avx():
        print("host lacks AVX; skipping native differential check")
        raise SystemExit(77)

    lifted_o2 = configure_lib(o2_path)
    native = configure_lib(native_path)
    sides = (("native", native), ("lifted", lifted), ("/O2", lifted_o2))

    exchange = lifted.host_exchange_mxcsr
    exchange.argtypes = [ctypes.c_uint32]
    exchange.restype = ctypes.c_uint32
    saved_mxcsr = exchange(MXCSR_ALL_MASKED)
    total_cases = 0
    out7 = (ctypes.c_ubyte * 7)()
    out_join = (ctypes.c_ubyte * 16)()
    out_cmov = (ctypes.c_uint64 * 6)()

    try:
        # 1. All 4 scalar VEX compares (register and memory RHS), including qNaN/sNaN in each operand.
        for fn_name in FLAG_EXPORTS:
            single = "ss" in fn_name
            vals = F32_VALUES if single else F64_VALUES
            fmt = "<I" if single else "<Q"
            for name_a, bits_a in vals:
                for name_b, bits_b in vals:
                    want = ref_cmp_bits(bits_a, bits_b, single)
                    buf_a = ctypes.create_string_buffer(struct.pack(fmt, bits_a))
                    buf_b = ctypes.create_string_buffer(struct.pack(fmt, bits_b))
                    for side_label, lib in sides:
                        ctypes.memset(out7, 0xCC, 7)
                        getattr(lib, fn_name)(ctypes.addressof(buf_a), ctypes.addressof(buf_b), ctypes.addressof(out7), 0)
                        got = bytes(out7)
                        if got != want:
                            raise AssertionError(
                                f"{side_label} {fn_name}({name_a}={bits_a:#x}, {name_b}={bits_b:#x}): "
                                f"got={got.hex()} expected={want.hex()}"
                            )
                    total_cases += 1

        # 2. SETP AL / SETNP AH preserving bits 16..63 of RAX.
        seed = 0x123456789ABCDE55
        for name_a, bits_a in F32_VALUES:
            for name_b, bits_b in F32_VALUES:
                pf = ref_cmp_bits(bits_a, bits_b, True)[0]
                want_rax = (seed & ~0xFFFF) | (((pf ^ 1) & 0xFF) << 8) | (pf & 0xFF)
                buf_a = ctypes.create_string_buffer(struct.pack("<I", bits_a))
                buf_b = ctypes.create_string_buffer(struct.pack("<I", bits_b))
                for side_label, lib in sides:
                    got_rax = lib.setp_reg_bytes(ctypes.addressof(buf_a), ctypes.addressof(buf_b), seed, 0)
                    if got_rax != want_rax:
                        raise AssertionError(
                            f"{side_label} setp_reg_bytes({name_a}, {name_b}): got={got_rax:#018x} want={want_rax:#018x}"
                        )
                total_cases += 1

        # 3. Both outcomes of CMOVP / CMOVNP at 32-bit (zero-extending upper 32 bits!), 64-bit, and 16-bit widths.
        init64 = 0x1122334455667788
        for alt in (0xFEDCBA9876543210, 0x0123456789ABCDEF):
            for name_a, bits_a in F32_VALUES:
                for name_b, bits_b in F32_VALUES:
                    pf = ref_cmp_bits(bits_a, bits_b, True)[0]
                    want = (
                        (alt if pf else init64) & 0xFFFFFFFF,
                        (alt if not pf else init64) & 0xFFFFFFFF,
                        alt if pf else init64,
                        alt if not pf else init64,
                        (init64 & ~0xFFFF) | ((alt if pf else init64) & 0xFFFF),
                        (init64 & ~0xFFFF) | ((alt if not pf else init64) & 0xFFFF),
                    )
                    buf_a = ctypes.create_string_buffer(struct.pack("<I", bits_a))
                    buf_b = ctypes.create_string_buffer(struct.pack("<I", bits_b))
                    for side_label, lib in sides:
                        ctypes.memset(out_cmov, 0xCC, ctypes.sizeof(out_cmov))
                        lib.cmov_reg_outcomes(ctypes.addressof(buf_a), ctypes.addressof(buf_b), alt, ctypes.addressof(out_cmov))
                        got = tuple(out_cmov)
                        if got != want:
                            raise AssertionError(
                                f"{side_label} cmov_reg_outcomes({name_a}, {name_b}, {alt:#x}): got={got} want={want}"
                            )
                    total_cases += 1

        # 4. CFG join (VCOMISS / VUCOMISD -> CMOVP + SETP + JP).
        join_inputs = (
            ("ordered", 0x3F800000, 0x40000000, 0x3FF0000000000000, 0x4000000000000000),
            ("equal", 0x3F800000, 0x3F800000, 0x8000000000000000, 0x0000000000000000),
            ("qnan0", 0x7FC00000, 0x3F800000, 0x7FF8000000000000, 0x3FF0000000000000),
            ("snan1", 0x3F800000, 0x7F800001, 0x3FF0000000000000, 0x7FF0000000000001),
        )
        for label, f_a, f_b, d_a, d_b in join_inputs:
            buf_a = ctypes.create_string_buffer(struct.pack("<IIQ", f_a, 0, d_a))
            buf_b = ctypes.create_string_buffer(struct.pack("<IIQ", f_b, 0, d_b))
            for sel in (0, 1):
                pf = ref_cmp_bits(f_a, f_b, True)[0] if sel else ref_cmp_bits(d_a, d_b, False)[0]
                want_cmov = 0xABCDEF01 if pf else 0x55667788
                for side_label, lib in sides:
                    ctypes.memset(out_join, 0xCC, 16)
                    lib.join_vcomi_pf(ctypes.addressof(buf_a), ctypes.addressof(buf_b), sel, ctypes.addressof(out_join))
                    got_cmov = struct.unpack_from("<Q", bytes(out_join), 0)[0]
                    got_setp, got_jp = out_join[8], out_join[9]
                    if (got_cmov, got_setp, got_jp) != (want_cmov, pf, pf):
                        raise AssertionError(
                            f"{side_label} join_vcomi_pf({label}, sel={sel}): got={(got_cmov, got_setp, got_jp)}"
                        )
                total_cases += 1

        # 5. Back-to-back compare (last VEX compare wins over previous PF).
        b2b_pairs = (
            ("nan_then_ordered", (0x7FC00000, 0x3F800000), (0x3F800000, 0x40000000), 0),
            ("ordered_then_nan", (0x3F800000, 0x40000000), (0x3F800000, 0x7F800001), 1),
        )
        for label, a2, b2, want_pf in b2b_pairs:
            buf_a = ctypes.create_string_buffer(struct.pack("<II", *a2))
            buf_b = ctypes.create_string_buffer(struct.pack("<II", *b2))
            want_cmov = 0xABCDEF01 if want_pf else 0x55667788
            for side_label, lib in sides:
                ctypes.memset(out_join, 0xCC, 16)
                lib.back_to_back_vcomi(ctypes.addressof(buf_a), ctypes.addressof(buf_b), ctypes.addressof(out_join), 0)
                got_setp, got_setnp = out_join[0], out_join[1]
                got_cmov = struct.unpack_from("<Q", bytes(out_join), 8)[0]
                if (got_setp, got_setnp, got_cmov) != (want_pf, want_pf ^ 1, want_cmov):
                    raise AssertionError(
                        f"{side_label} back_to_back_vcomi({label}): got={(got_setp, got_setnp, got_cmov)}"
                    )
            total_cases += 1

        # 6. Memory-source CMOVP/CMOVNP fault behavior and 32-bit zero-extension.
        total_cases += check_cmov_faults(guard_path, sides)

        # 7. Mutated controls (stalepf and swap) must mismatch.
        for ctrl_path in control_paths:
            ctrl_name = Path(ctrl_path).stem.rsplit("_", 1)[-1]
            ctrl = configure_lib(ctrl_path)
            mismatches = 0
            # Check vcomiss_reg_flags on (one, one) and (qnan_pos, one)
            for bits_a, bits_b in ((0x3F800000, 0x3F800000), (0x7FC00000, 0x3F800000)):
                want = ref_cmp_bits(bits_a, bits_b, True)
                buf_a = ctypes.create_string_buffer(struct.pack("<I", bits_a))
                buf_b = ctypes.create_string_buffer(struct.pack("<I", bits_b))
                ctypes.memset(out7, 0xCC, 7)
                ctrl.vcomiss_reg_flags(ctypes.addressof(buf_a), ctypes.addressof(buf_b), ctypes.addressof(out7), 0)
                if bytes(out7) != want:
                    mismatches += 1
            # Check back_to_back_vcomi on ordered_then_nan and nan_then_ordered.
            for a2, b2, want_pf, want_cmov in (
                ((0x3F800000, 0x40000000), (0x3F800000, 0x7F800001), 1, 0xABCDEF01),
                ((0x7FC00000, 0x3F800000), (0x3F800000, 0x40000000), 0, 0x55667788),
            ):
                buf_a = ctypes.create_string_buffer(struct.pack("<II", *a2))
                buf_b = ctypes.create_string_buffer(struct.pack("<II", *b2))
                ctypes.memset(out_join, 0xCC, 16)
                ctrl.back_to_back_vcomi(ctypes.addressof(buf_a), ctypes.addressof(buf_b), ctypes.addressof(out_join), 0)
                if (out_join[0], out_join[1], struct.unpack_from("<Q", bytes(out_join), 8)[0]) != (want_pf, want_pf ^ 1, want_cmov):
                    mismatches += 1
            # Check join_vcomi_pf on qnan0 (sel=0).
            buf_a = ctypes.create_string_buffer(struct.pack("<IIQ", 0x7FC00000, 0, 0x7FF8000000000000))
            buf_b = ctypes.create_string_buffer(struct.pack("<IIQ", 0x3F800000, 0, 0x3FF0000000000000))
            ctypes.memset(out_join, 0xCC, 16)
            ctrl.join_vcomi_pf(ctypes.addressof(buf_a), ctypes.addressof(buf_b), 0, ctypes.addressof(out_join))
            if (struct.unpack_from("<Q", bytes(out_join), 0)[0], out_join[8], out_join[9]) != (0xABCDEF01, 1, 1):
                mismatches += 1
            if mismatches == 0:
                raise AssertionError(f"mutated control {ctrl_name} matched every case!")
            print(f"control {ctrl_name}: caught {mismatches} deliberate PF mismatches")
    finally:
        exchange(saved_mxcsr)

    print(
        f"{total_cases} VEX scalar compare / PF consumer cases (masked MXCSR {MXCSR_ALL_MASKED:#06x}, "
        f"qNaN/sNaN in both operands, 32/64/16-bit CMOVP/CMOVNP, fault checks, CFG joins) matched native, "
        f"translated, and /O2 DLLs on {platform.processor()}"
    )


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "emit":
        emit(Path(sys.argv[2]), Path(sys.argv[3]), *sys.argv[4:])
    elif mode == "check":
        check(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5]),
              *(Path(p) for p in sys.argv[6:]))
    else:
        raise ValueError(mode)
