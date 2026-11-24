"""Compare native and translated FTZ and DAZ in the host-MXCSR profile's denormals opt-in.

The translation runs through mxcsr_entry(..., denormals=True), which reads the host's MXCSR_MASK once per entry.
LDMXCSR accepts FTZ and DAZ only where that mask has the bit and refuses any other unsupported bit before the host
register changes. An FTZ case adds normal operands whose exact sums are tiny and signed; a DAZ case adds signed
subnormal operands whose results change when DAZ reads them as zeros. Both run in all four FTZ/DAZ combinations
under two rounding controls, against fixed raw words and native execution.
"""
import ctypes
from pathlib import Path
import struct
import sys

import pefile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges
from ffx_version_query import INTEGER_HEADER, MXCSR_HEADER, MXCSR_STATUS, Mxcsr, lower, mxcsr_entry, region

U64 = ctypes.c_uint64
EXPORTS = ("den_add", "den_after", "den_fault")
REJECT = "{{ *mxcsr_status = {status}; return 0; }}"
DEFAULT = 0x1F80
CALLER = 0x7FA0  # round toward zero, the precision flag set, FTZ and DAZ clear
FTZ, DAZ = 0x8000, 0x0040
FILL = 0xA5
STATE = 0x100
GUARDS = [(0x04, 4), (0x0C, 4), (0x74, 4), (0x80, 0x80)]
TINY = 2.0 ** -126  # the smallest normal binary32 value
SUB = 2.0 ** -140   # a subnormal binary32 value
FTZ_A = (TINY, -TINY, 0.0, 0.0)
FTZ_B = (-1.5 * TINY, 1.5 * TINY, 0.0, 0.0)
DAZ_C = (TINY, -TINY, -SUB, 0.0)
DAZ_D = (-SUB, SUB, 0.0, 0.0)
COMBINATIONS = {"none": 0, "FTZ": FTZ, "DAZ": DAZ, "both": FTZ | DAZ}
# Measured on the host natively and fixed here: the FTZ sums, then the DAZ sums, as raw binary32 words.
FTZ_SUMS = {False: (0x80400000, 0x00400000, 0, 0), True: (0x80000000, 0x00000000, 0, 0)}


def daz_sums(ftz, daz, rc):
    if daz:
        return (0x00800000, 0x80800000, 0x80000000 if rc == 1 else 0x00000000, 0)
    if ftz:
        return (0x00000000, 0x80000000, 0x80000000, 0)
    return (0x007FFE00, 0x807FFE00, 0x80000200, 0)


CONTROLS = {
    # LDMXCSR clears FTZ or DAZ before it loads the word.
    "noftz": [("_mm_setcsr(want);", "_mm_setcsr(want & ~0x8000u);")],
    "nodaz": [("_mm_setcsr(want);", "_mm_setcsr(want & ~0x40u);")],
    # Simulated host without DAZ: only the probed capability changes; the variant must still keep every rule.
    "sim": [("if (!mxcsr_mask) mxcsr_mask = 0xFFBFu;", "mxcsr_mask = 0xFFBFu;")],
    # Ignore the simulated refusal but clear the bit at the actual load so the mutant is safe
    # to run even on a physical host without DAZ. It must still be caught continuing guest work.
    "nomask": [("if (!mxcsr_mask) mxcsr_mask = 0xFFBFu;", "mxcsr_mask = 0xFFBFu;"),
               ("if (want & ~mxcsr_mask)", "if (want & 0u)"),
               ("_mm_setcsr(want);", "_mm_setcsr(want & ~0x40u);")],
}
SIMULATED = ("sim", "nomask")


def emit(image, output, variant="lifted"):
    parts = [MXCSR_HEADER + INTEGER_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            code = lower(region(pe, *bounds), ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {},
                         host_mxcsr=Mxcsr(REJECT, denormals=True))
            parts.append(f"static __declspec(noinline) word {name}_body(word a, word b, word c, word state, "
                         f"int* mxcsr_status, uint32_t mxcsr_mask) {{\n{code}\n}}\n")
            parts.append(mxcsr_entry(name, f"{name}_body", denormals=True))
    text = "".join(parts)
    for literal, replacement in CONTROLS.get(variant, []):
        count = text.count(literal)
        if not count:
            raise AssertionError(f"variant {variant} changed nothing for {literal!r}")
        text = text.replace(literal, replacement)
    output.write_text(text, encoding="ascii")


class Host:
    def __init__(self, harness):
        self.lib = ctypes.CDLL(str(Path(harness).resolve()))
        self.lib.get_mxcsr.argtypes, self.lib.get_mxcsr.restype = [], ctypes.c_uint32
        self.lib.set_mxcsr.argtypes, self.lib.set_mxcsr.restype = [ctypes.c_uint32], None
        self.lib.mxcsr_mask_of.argtypes, self.lib.mxcsr_mask_of.restype = [ctypes.c_void_p], ctypes.c_uint32

    def get(self):
        return self.lib.get_mxcsr()

    def set(self, word):
        self.lib.set_mxcsr(word)

    def mask(self):
        raw = (ctypes.c_ubyte * 528)()
        aligned = (ctypes.addressof(raw) + 15) & ~15
        value = self.lib.mxcsr_mask_of(aligned)
        return value or 0xFFBF


def load(path):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    for name in EXPORTS:
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [U64, U64, U64, ctypes.c_void_p], U64
    return lib


def status(lib, name):
    getter = getattr(lib, f"{name}_status")
    getter.argtypes, getter.restype = [], ctypes.c_int
    return getter()


def fresh_state():
    state = (ctypes.c_ubyte * STATE)(*([FILL] * STATE))
    for offset, values in ((0x10, FTZ_A), (0x20, FTZ_B), (0x30, DAZ_C), (0x40, DAZ_D)):
        struct.pack_into("<4f", state, offset, *values)
    return state


def guards_intact(state):
    return all(all(b == FILL for b in bytes(state[o:o + n])) for o, n in GUARDS)


def add_case(host, lib, word):
    state = fresh_state()
    before = bytes(state)
    host.set(CALLER)
    result = lib.den_add(0, word, 0, ctypes.addressof(state))
    after = host.get()
    return (result, struct.unpack_from("<4I", state, 0x50), struct.unpack_from("<4I", state, 0x60),
            struct.unpack_from("<I", state, 0x70)[0], after, state, before)


def violation(host, native, lib, label, daz_ok, probe_daz):
    """Return the first mismatch for words with or without DAZ, according to probe_daz."""
    for rc in (0, 1):
        for combination, bits in COMBINATIONS.items():
            if bool(bits & DAZ) != probe_daz:
                continue
            word = DEFAULT | rc << 13 | bits
            result, ftz_sums, daz_lanes, readback, after, state, before = add_case(host, lib, word)
            if after != CALLER:
                return f"{label} den_add({word:#x}): caller MXCSR {after:#x}, expected {CALLER:#x}"
            if not guards_intact(state):
                return f"{label} den_add({word:#x}): a guard byte changed"
            if bits & DAZ and not daz_ok:
                # Refused before LDMXCSR: only the entry STMXCSR and the requested word were stored, every output
                # keeps its fill.
                untouched = bytes(state)[0x0C:] == before[0x0C:]
                if (result, status(lib, "den_add"), untouched) != (0, MXCSR_STATUS["unsupported"], True):
                    return (f"{label} den_add({word:#x}) on a host without DAZ: return {result}, status "
                            f"{status(lib, 'den_add')}, outputs unchanged {untouched}")
                continue
            want_ftz, want_daz = FTZ_SUMS[bool(bits & FTZ)], daz_sums(bool(bits & FTZ), bool(bits & DAZ), rc)
            if result != 7 or status(lib, "den_add"):
                return f"{label} den_add({word:#x}): return {result}, status {status(lib, 'den_add')}"
            if ftz_sums != want_ftz:
                return f"{label} rc {rc} {combination}: FTZ sums {[hex(v) for v in ftz_sums]}, expected {[hex(v) for v in want_ftz]}"
            if daz_lanes != want_daz:
                return f"{label} rc {rc} {combination}: DAZ sums {[hex(v) for v in daz_lanes]}, expected {[hex(v) for v in want_daz]}"
            if readback & ~0x3F != word & ~0x3F:
                return f"{label} rc {rc} {combination}: STMXCSR read back {readback:#x} after loading {word:#x}"
            if native is not None:
                reference = add_case(host, native, word)
                if reference[1:4] != (ftz_sums, daz_lanes, readback):
                    return (f"{label} rc {rc} {combination}: {ftz_sums}, {daz_lanes}, {readback:#x} != native "
                            f"{reference[1]}, {reference[2]}, {reference[3]:#x}")
    # Guest work after the load runs for an accepted word and not for a refused one.
    for word in ((DEFAULT | DAZ, DEFAULT | FTZ | DAZ) if probe_daz else (DEFAULT | FTZ,)):
        state = fresh_state()
        host.set(CALLER)
        result = lib.den_after(0, word, 0, ctypes.addressof(state))
        marker = struct.unpack_from("<Q", state, 0x78)[0]
        accepted = daz_ok or not word & DAZ
        want = (7, 0, 1) if accepted else (0, MXCSR_STATUS["unsupported"], int.from_bytes([FILL] * 8, "little"))
        if (result, status(lib, "den_after"), marker) != want or host.get() != CALLER:
            return (f"{label} den_after({word:#x}): (return, status, marker) {(result, status(lib, 'den_after'), marker)}, "
                    f"expected {want}, MXCSR {host.get():#x}")
    # A guest fault after loading supported denormal mode bits restores the caller's word.
    fault_word = DEFAULT | FTZ | (DAZ if probe_daz and daz_ok else 0)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    page = kernel32.VirtualAlloc(None, 4096, 0x3000, 0x01)
    if not page:
        raise OSError(ctypes.get_last_error(), "VirtualAlloc")
    try:
        # The state is built first: packing the subnormal operands sets sticky flags in this thread's MXCSR.
        state = fresh_state()
        host.set(CALLER)
        try:
            lib.den_fault(page, fault_word, 0, ctypes.addressof(state))
        except OSError:
            pass
        else:
            return f"{label} den_fault returned from an inaccessible load"
        if host.get() != CALLER:
            return f"{label} den_fault: MXCSR {host.get():#x} after the fault, expected {CALLER:#x}"
    finally:
        kernel32.VirtualFree(ctypes.c_void_p(page), 0, 0x8000)
    return None


def check(harness_path, native_path, lifted_path, *extra_paths, probe_daz):
    host = Host(harness_path)
    original = host.get()
    try:
        mask = host.mask()
        daz_host = bool(mask & DAZ)
        print(f"host MXCSR_MASK {mask:#x}: DAZ {'supported' if daz_host else 'not supported'}")
        native = load(native_path)
        problem = violation(host, native, load(lifted_path), "lifted", daz_host, probe_daz)
        if problem:
            raise AssertionError(problem)
        if not probe_daz:
            scope = "FTZ flushes the tiny signed sums under both rounding controls, as natively and as the fixed raw words"
        elif daz_host:
            scope = "DAZ zeros the signed subnormal operands with FTZ off and on, under both rounding controls"
        else:
            scope = "Every request with DAZ is refused before LDMXCSR"
        print(f"lifted: {scope}; read-back, adjacent bytes, the refusal marker and the caller's word after return "
              "and fault hold")
        for path in extra_paths:
            stem = Path(path).stem
            variant = "lifted_o2" if stem.endswith("_o2") else stem.rsplit("_", 1)[-1]
            if variant not in (("nodaz", "sim", "nomask", "lifted_o2") if probe_daz else ("noftz", "lifted_o2")):
                continue
            if variant == "nodaz" and not daz_host:
                # The host refuses DAZ already, so a translation that clears it cannot differ.
                print("control nodaz is not measurable: this host has no DAZ")
                continue
            simulated = variant in SIMULATED
            problem = violation(host, None if simulated else native, load(path), variant,
                                daz_host and not simulated, probe_daz)
            if variant in ("lifted_o2", "sim"):
                if problem:
                    raise AssertionError(problem)
                print(f"{variant}: keeps every rule" + (" with DAZ refused before LDMXCSR on the simulated host"
                                                        if simulated else " in the /O2 build"))
            else:
                print(f"control {variant}: {problem}")
                if problem is None:
                    raise AssertionError(f"control {variant} kept every rule")
        if probe_daz and not daz_host:
            print("skip: this host's MXCSR_MASK has no DAZ; the refusal controls above passed")
            raise SystemExit(77)
    finally:
        host.set(original)


if __name__ == "__main__":
    mode, *args = sys.argv[1:]
    if mode == "emit":
        image, output, *variant = args
        emit(Path(image), Path(output), *variant)
    elif mode in ("check-ftz", "check-daz"):
        check(*args, probe_daz=(mode == "check-daz"))
    else:
        raise ValueError(mode)
