"""Compare native and translated PF readers after the VEX scalar compares in the shared lowering.

Every comparison runs with MXCSR 0x1F80 (all exceptions masked, round to nearest), which the native harness sets
before and restores after the checks: a signaling or quiet NaN then only sets the flags. The fixed expectation of
each operand pair follows the compare's definition: unordered sets ZF, PF and CF, less sets CF, equal sets ZF.
"""
import ctypes
from pathlib import Path
import struct
import sys

from iced_x86 import Decoder, Mnemonic
import pefile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges
from ffx_version_query import INTEGER_HEADER, lower, region
from x87_lower import X87_HEADER

U64 = ctypes.c_uint64
EXPORTS = ("pf_ucomiss", "pf_comiss", "pf_ucomisd", "pf_comisd", "pf_fucomi", "pf_equal", "pf_fault", "pf_fault_p",
           "pf_lahf")
REFUSED = ("pf_after_add", "pf_join", "pf_undefined")
SINGLE = {"pf_ucomiss": True, "pf_comiss": True, "pf_ucomisd": False, "pf_comisd": False, "pf_fucomi": False}
X87 = {"pf_fucomi"}
LAHF_PAIRS = [(0, 0), (1, 2), (1, 0), (0xFE, 1), (0x7F, 0x80), (0xFFFFFFFF, 1), (0x12345678, 0x0F0F0F0F)]
MASKED = 0x1F80
FILL = 0xA5
F32 = {"1": 0x3F800000, "2": 0x40000000, "+0": 0x00000000, "-0": 0x80000000, "+inf": 0x7F800000,
       "-inf": 0xFF800000, "qnan": 0x7FC00000, "-qnan": 0xFFC00001, "snan": 0x7F800001}
F64 = {"1": 0x3FF0000000000000, "2": 0x4000000000000000, "+0": 0, "-0": 0x8000000000000000,
       "+inf": 0x7FF0000000000000, "-inf": 0xFFF0000000000000, "qnan": 0x7FF8000000000000,
       "-qnan": 0xFFF8000000000001, "snan": 0x7FF0000000000001}
PAIRS = [("1", "2"), ("2", "1"), ("1", "1"), ("+0", "-0"), ("+inf", "+inf"), ("-inf", "1"), ("qnan", "1"),
         ("1", "qnan"), ("snan", "1"), ("1", "snan"), ("-qnan", "snan"), ("snan", "snan")]
ORDER = {"1": 1.0, "2": 2.0, "+0": 0.0, "-0": -0.0, "+inf": float("inf"), "-inf": float("-inf")}
CONTROLS = {
    # The compare no longer assigns PF, so PF keeps its stale initial value.
    "stale": [(" parity = unordered;", "")],
    # Every PF reader takes the reversed condition.
    "reversed": [("(!parity)", "PF_SWAP_MARK"), ("(parity)", "(!parity)"), ("PF_SWAP_MARK", "(parity)")],
}


def flags(a, b):
    """(ZF, PF, CF) of the compare of two named operands."""
    if a not in ORDER or b not in ORDER:
        return 1, 1, 1
    x, y = ORDER[a], ORDER[b]
    return int(x == y), 0, int(x < y)


def emit(image, output, variant="lifted"):
    parts = [INTEGER_HEADER, X87_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            code = lower(region(pe, *bounds), ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {},
                         x87=name in X87)
            parts.append(f'extern "C" __declspec(dllexport) word {name}(word a, word b, word c, word state) {{\n{code}\n}}\n')
    text = "".join(parts)
    for literal, replacement in CONTROLS.get(variant, []):
        if literal not in text:
            raise AssertionError(f"variant {variant} changed nothing for {literal!r}")
        text = text.replace(literal, replacement)
    output.write_text(text, encoding="ascii")


def refusals(image):
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in REFUSED:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            try:
                lower(region(pe, *bounds), ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {})
            except ValueError as exc:
                if "reads PF" not in str(exc):
                    raise AssertionError(f"{name}: unexpected refusal {exc}")
            else:
                raise AssertionError(f"the lowering accepted {name}")
        # A host binding of the compare is opaque emitted code and does not define PF for its readers.
        (bounds,) = function_ranges(pe, export_rva(pe, "pf_ucomiss"))
        body = region(pe, *bounds)
        (compare,) = [i.ip for i in body if i.mnemonic == Mnemonic.VUCOMISS]
        try:
            lower(body, ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {}, host_ops={compare: ";"})
        except ValueError as exc:
            if "reads PF" not in str(exc):
                raise AssertionError(f"host-bound compare: unexpected refusal {exc}")
        else:
            raise AssertionError("the lowering accepted a PF reader after a host-bound compare")
    # A reader that no path from the entry reaches is refused: ret, then setnp al.
    try:
        lower(list(Decoder(64, bytes.fromhex("c3 0f 9b c0 c3"), ip=0x1000)), [], {}, {}, {})
    except ValueError as exc:
        if "reads PF" not in str(exc):
            raise AssertionError(f"unreached reader: unexpected refusal {exc}")
    else:
        raise AssertionError("the lowering accepted an unreached PF reader")
    return [*REFUSED, "a host-bound compare", "an unreached reader"]


def load(path):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    for name in EXPORTS:
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [U64, U64, U64, ctypes.c_void_p], U64
    return lib


def state_for(single, a, b):
    state = (ctypes.c_ubyte * 0x40)(*([FILL] * 0x40))
    table = F32 if single else F64
    struct.pack_into("<I" if single else "<Q", state, 0, table[a])
    struct.pack_into("<I" if single else "<Q", state, 8, table[b])
    struct.pack_into("<I", state, 0x28, 9)
    return state


def run(lib, name, a, b):
    state = state_for(SINGLE[name], a, b)
    getattr(lib, name)(0, 0, 0, ctypes.addressof(state))
    setnp, setp, sete, setb = bytes(state[0x10:0x14])
    cmovp, cmovnp = struct.unpack_from("<2Q", state, 0x18)
    jp = struct.unpack_from("<Q", state, 0x30)[0]
    return setnp, setp, sete, setb, cmovp, cmovnp, jp


def expected(a, b):
    zf, pf, cf = flags(a, b)
    return (1 - pf, pf, zf, cf, 7 if pf else 0xFFFFFFFF, 0xFFFFFFFF if pf else 9, 1 if pf else 2)


def violation(native, lib, label):
    for name in SINGLE:
        for a, b in PAIRS:
            want = expected(a, b)
            got = run(lib, name, a, b)
            if got != want:
                return f"{label} {name}({a}, {b}): (setnp, setp, sete, setb, cmovp, cmovnp, jp) {got}, expected {want}"
            if native is not None and run(native, name, a, b) != want:
                raise AssertionError(f"native {name}({a}, {b}) = {run(native, name, a, b)}, expected {want}")
    for a, b in PAIRS:
        zf, pf, _ = flags(a, b)
        state = state_for(True, a, b)
        got = lib.pf_equal(0, 0, 0, ctypes.addressof(state))
        if got != int(zf and not pf):
            return f"{label} pf_equal({a}, {b}) = {got}, expected {int(zf and not pf)}"
    for a, b in LAHF_PAIRS:
        want = int(bin((a + b) & 0xFF).count("1") % 2 == 0)
        got = lib.pf_lahf(a, b, 0, None)
        if got != want:
            return f"{label} pf_lahf({a:#x}, {b:#x}) = {got}, expected {want}"
        if native is not None and native.pf_lahf(a, b, 0, None) != want:
            raise AssertionError(f"native pf_lahf({a:#x}, {b:#x}), expected {want}")
    return None


def faults(lib, label, page):
    """pf_fault and pf_fault_p read their CMOVNP and CMOVP source through an inaccessible pointer: each must fault
    for an ordered and an unordered pair alike."""
    for name in ("pf_fault", "pf_fault_p"):
        for a, b in (("1", "2"), ("qnan", "1")):
            state = state_for(True, a, b)
            try:
                getattr(lib, name)(page, 0, 0, ctypes.addressof(state))
            except OSError:
                continue
            return f"{label} {name}({a}, {b}) returned from an inaccessible source"
    return None


def check(harness_path, native_path, lifted_path, *extra_paths):
    harness = ctypes.CDLL(str(Path(harness_path).resolve()))
    harness.get_mxcsr.restype = ctypes.c_uint32
    harness.set_mxcsr.argtypes = [ctypes.c_uint32]
    harness.has_avx_os.restype = ctypes.c_uint32
    avx = bool(harness.has_avx_os())
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    page = kernel32.VirtualAlloc(None, 4096, 0x3000, 0x01)
    original = harness.get_mxcsr()
    try:
        harness.set_mxcsr(MASKED)
        print(f"refused, with a PF reader that no modeled producer reaches: {', '.join(refusals(native_path))}")
        native = load(native_path) if avx else None
        problem = violation(native, load(lifted_path), "lifted") or faults(load(lifted_path), "lifted", page)
        if problem:
            raise AssertionError(problem)
        problem = faults(native, "native", page) if native is not None else None
        if problem:
            raise AssertionError(problem)
        print(f"lifted{' and native' if native else ''}: SETNP, SETP, SETE, SETB, CMOVP, CMOVNP and JP after "
              f"VUCOMISS, VCOMISS, VUCOMISD, VCOMISD and FUCOMIP match the fixed flags for {len(PAIRS)} pairs "
              "(ordered, equal, signed zeros, infinities, quiet and signaling NaNs in either operand) under MXCSR "
              "0x1f80; CMOVP clears the upper half of RAX either way; the equality idiom holds; SETP after a LAHF "
              f"source matches the parity of the sum for {len(LAHF_PAIRS)} pairs; the CMOVNP and CMOVP sources "
              "fault for ordered and unordered pairs" + ("" if native else " (no native comparison: this host cannot "
                                                    "execute VEX instructions)"))
        for path in extra_paths:
            stem = Path(path).stem
            if stem.endswith("_o2"):
                problem = violation(native, load(path), stem) or faults(load(path), stem, page)
                if problem:
                    raise AssertionError(problem)
                print(f"{stem}: the /O2 build keeps every rule")
                continue
            variant = stem.rsplit("_", 1)[-1]
            problem = violation(None, load(path), f"control {variant}")
            print(f"control {variant}: {problem}")
            if problem is None:
                raise AssertionError(f"control {variant} kept every rule")
    finally:
        harness.set_mxcsr(original)
        kernel32.VirtualFree(ctypes.c_void_p(page), 0, 0x8000)


if __name__ == "__main__":
    mode, *args = sys.argv[1:]
    if mode == "emit":
        image, output, *variant = args
        emit(Path(image), Path(output), *variant)
    elif mode == "check":
        check(*args)
    else:
        raise ValueError(mode)
