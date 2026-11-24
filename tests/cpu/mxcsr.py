"""Compare native and translated LDMXCSR and STMXCSR in the opt-in host-MXCSR profile.

The translation runs through mxcsr_entry, which saves the caller's MXCSR and restores it on return, after a
refusal and when a guest memory fault leaves the lowered body. Every rounding mode is round-tripped and changes
an ADDPS halfway case and CVTSS2SI conversions whose operands the test writes at run time. Words with a
reserved bit, an unmasked exception, FTZ or DAZ are refused before the host register changes.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
import threading

import pefile
from iced_x86 import Decoder, FlowControl, Mnemonic as M

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges
from ffx_version_query import INTEGER_HEADER, MXCSR_HEADER, MXCSR_STATUS, Mxcsr, lower, mxcsr_entry, region

U64 = ctypes.c_uint64
EXPORTS = ("mxcsr_round", "mxcsr_leak", "mxcsr_after", "mxcsr_badload", "mxcsr_fault")
# LDMXCSR and STMXCSR in each export of mxcsr.asm.
SOURCE_READS = {"mxcsr_round": 4, "mxcsr_leak": 1, "mxcsr_after": 1, "mxcsr_badload": 1, "mxcsr_fault": 1}
REJECT = "{{ *mxcsr_status = {status}; return 0; }}"
# The MXCSR instructions of the fixtures, legacy and VEX, by name.
MXCSR_MNEMONICS = {M.LDMXCSR: "LDMXCSR", M.STMXCSR: "STMXCSR", M.VLDMXCSR: "VLDMXCSR", M.VSTMXCSR: "VSTMXCSR"}
DEFAULT = 0x1F80  # all six exceptions masked, round to nearest-even, FTZ and DAZ clear
CALLER = 0x7FA0   # a non-default caller word: round toward zero and the sticky precision flag
FILL = 0xA5
STATE = 0x100
GUARDS = [(0x04, 4), (0x0C, 4), (0x64, 4), (0x6C, 4), (0x74, 4), (0x80, 0x80)]


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


ONE_ULP = 2.0 ** -23
# ADDPS lane 0: 1 + 0.75 ulp, lane 1: -(1 + 0.75 ulp). Lanes 2 and 3 are halfway cases: 1 + 0.5 ulp, between
# an even and an odd neighbour, and (1 + 1 ulp) + 0.5 ulp, between an odd and an even one, so to-nearest-even
# rounds them in different directions. CVTSS2SI of 2.75 and -2.75.
ADD_A = (1.0, -1.0, 1.0, 1 + ONE_ULP)
ADD_B = (3 * 2.0 ** -25, -3 * 2.0 ** -25, 2.0 ** -24, 2.0 ** -24)
CVT_C, CVT_D = 2.75, -2.75
# Rounding control -> (four lane bits, CVTSS2SI(2.75), CVTSS2SI(-2.75)).
EXPECT = {
    0: (f32(1 + ONE_ULP), f32(-(1 + ONE_ULP)), f32(1.0), f32(1 + 2 * ONE_ULP), 3, -3),  # to nearest-even
    1: (f32(1.0), f32(-(1 + ONE_ULP)), f32(1.0), f32(1 + ONE_ULP), 2, -3),             # toward negative infinity
    2: (f32(1 + ONE_ULP), f32(-1.0), f32(1 + ONE_ULP), f32(1 + 2 * ONE_ULP), 3, -2),   # toward positive infinity
    3: (f32(1.0), f32(-1.0), f32(1.0), f32(1 + ONE_ULP), 2, -2),                       # toward zero
}
if len(set(EXPECT.values())) != 4:
    raise AssertionError("the probes do not tell the four rounding modes apart")
REFUSED = {0x11F80: "reserved", 0x1D80: "unmasked", 0x9F80: "unsupported", 0x1FC0: "unsupported"}
CONTROLS = {
    # LDMXCSR checks the word but never loads it.
    "noload": ("_mm_setcsr(want);", "(void)want;"),
    # STMXCSR stores the default word instead of the host's.
    "fixed": (", _mm_getcsr());", ", 0x1f80u);"),
}


def emit(image, output, variant="lifted"):
    parts = [MXCSR_HEADER + INTEGER_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            code = lower(region(pe, *bounds), ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {},
                         host_mxcsr=Mxcsr(REJECT))
            parts.append(f"static __declspec(noinline) word {name}_body(word a, word b, word c, word state, "
                         f"int* mxcsr_status) {{\n{code}\n}}\n")
            parts.append(mxcsr_entry(name, f"{name}_body"))
    text = "".join(parts)
    if variant != "lifted":
        literal, replacement = CONTROLS[variant]
        count = text.count(literal)
        if not count:
            raise AssertionError(f"variant {variant} changed nothing")
        text = text.replace(literal, replacement)
        print(f"variant {variant}: {count} replacements")
    output.write_text(text, encoding="ascii")


def rejected_by_default(image):
    """The default profile refuses every LDMXCSR and STMXCSR: each export is lowered with every earlier MXCSR
    instruction replaced by a no-op, so each one is the refused instruction once."""
    refused = []
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            body = region(pe, *bounds)
            sites = [i for i in body if i.mnemonic in MXCSR_MNEMONICS]
            for k, site in enumerate(sites):
                skip = {earlier.ip: ";" for earlier in sites[:k]}
                try:
                    lower(body, ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {}, host_ops=skip)
                except ValueError as exc:
                    if "unsupported query operation" not in str(exc) or f"{site.ip:#x}" not in str(exc):
                        raise AssertionError(f"{name}: expected the refusal of {site} at {site.ip:#x}, got {exc}")
                    refused.append(MXCSR_MNEMONICS[site.mnemonic])
                else:
                    raise AssertionError(f"{name}: the default profile lowered {site}")
    return refused


def load(path, lifted):
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
    struct.pack_into("<4f", state, 0x10, *ADD_A)
    struct.pack_into("<4f", state, 0x20, *ADD_B)
    struct.pack_into("<4f", state, 0x30, CVT_C, 0, 0, 0)
    struct.pack_into("<4f", state, 0x40, CVT_D, 0, 0, 0)
    return state


def guards_intact(state):
    return all(all(b == FILL for b in bytes(state[o:o + n])) for o, n in GUARDS)


class Host:
    def __init__(self, harness):
        self.lib = ctypes.CDLL(str(Path(harness).resolve()))
        self.lib.get_mxcsr.argtypes, self.lib.get_mxcsr.restype = [], ctypes.c_uint32
        self.lib.set_mxcsr.argtypes, self.lib.set_mxcsr.restype = [ctypes.c_uint32], None

    def get(self):
        return self.lib.get_mxcsr()

    def set(self, word):
        self.lib.set_mxcsr(word)


def round_case(host, lib, rc, sticky):
    """(outputs, problem) of mxcsr_round with rounding control rc and the requested sticky flags."""
    word = DEFAULT | rc << 13 | sticky
    state = fresh_state()
    host.set(CALLER)
    result = lib.mxcsr_round(0, word, 0, ctypes.addressof(state))
    after = host.get()
    lanes = struct.unpack_from("<4I", state, 0x50)
    cvt = struct.unpack_from("<i", state, 0x60)[0], struct.unpack_from("<i", state, 0x68)[0]
    readback = struct.unpack_from("<I", state, 0x70)[0]
    outputs = (*lanes, *cvt, readback)
    if result != 7:
        return outputs, f"returned {result:#x}"
    if (*lanes, *cvt) != EXPECT[rc]:
        return outputs, f"rounding control {rc}: lanes/conversions {lanes}, {cvt}, expected {EXPECT[rc]}"
    if readback & ~0x3F != word & ~0x3F or readback & sticky != sticky:
        return outputs, (f"STMXCSR read back {readback:#x} after loading {word:#x}: control bits and the "
                         "requested sticky flags must be kept")
    if struct.unpack_from("<I", state, 0x00)[0] != CALLER:
        return outputs, f"entry STMXCSR stored {struct.unpack_from('<I', state, 0)[0]:#x}, caller {CALLER:#x}"
    if not guards_intact(state):
        return outputs, "a guard byte next to a stored word changed"
    if after != CALLER:
        return outputs, f"caller MXCSR {after:#x} after the call, expected {CALLER:#x}"
    return outputs, None


def lifted_violation(host, native, lib, label):
    """The first broken rule of a translated library, or None. native None skips the native comparison and keeps
    the fixed expectations."""
    for rc in range(4):
        for sticky in (0, 0x3F):
            got, problem = round_case(host, lib, rc, sticky)
            if problem:
                return f"{label} mxcsr_round: {problem}"
            if native is None:
                continue
            expected, problem = round_case(host, native, rc, sticky)
            if problem:
                raise AssertionError(f"native mxcsr_round: {problem}")
            if got != expected:
                return f"{label} mxcsr_round rc {rc} sticky {sticky:#x}: {got} != native {expected}"
    # The boundary restores a word the guest leaves loaded.
    state = fresh_state()
    host.set(CALLER)
    result = lib.mxcsr_leak(0, DEFAULT | 1 << 13, 0, ctypes.addressof(state))
    if result != 7 or status(lib, "mxcsr_leak") or host.get() != CALLER:
        return f"{label} mxcsr_leak: returned {result:#x}, status {status(lib, 'mxcsr_leak')}, MXCSR {host.get():#x}"
    # A valid word lets the guest continue; a refused one stops it with its status and loads nothing.
    for word, reason in ((DEFAULT | 2 << 13, None), *REFUSED.items()):
        state = fresh_state()
        host.set(CALLER)
        result = lib.mxcsr_after(0, word, 0, ctypes.addressof(state))
        marker = struct.unpack_from("<Q", state, 0x78)[0]
        got = (result, status(lib, "mxcsr_after"), marker, host.get())
        want = (7, 0, 1, CALLER) if reason is None else (0, MXCSR_STATUS[reason], int.from_bytes([FILL] * 8, "little"), CALLER)
        if got != want:
            return f"{label} mxcsr_after({word:#x}): (return, status, marker, MXCSR) {got}, expected {want}"
    # The status belongs to the calling thread: a successful call on another thread leaves this thread's
    # refusal status in place.
    state = fresh_state()
    lib.mxcsr_after(0, 0x11F80, 0, ctypes.addressof(state))
    other = []
    worker = threading.Thread(target=lambda: other.append(
        (lib.mxcsr_after(0, DEFAULT, 0, ctypes.addressof(fresh_state())), status(lib, "mxcsr_after"))))
    worker.start()
    worker.join()
    if other != [(7, 0)] or status(lib, "mxcsr_after") != MXCSR_STATUS["reserved"]:
        return f"{label} status per thread: other thread {other}, this thread {status(lib, 'mxcsr_after')}"
    host.set(CALLER)
    # A guest memory fault: once at the LDMXCSR operand itself, once after a valid load. The boundary restores
    # the caller's word both times.
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    page = kernel32.VirtualAlloc(None, 4096, 0x3000, 0x01)  # MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS
    if not page:
        raise OSError(ctypes.get_last_error(), "VirtualAlloc")
    try:
        state = fresh_state()
        lib.mxcsr_fault(page, 0x11F80, 0, ctypes.addressof(state))
        if status(lib, "mxcsr_fault") != MXCSR_STATUS["reserved"]:
            return f"{label} mxcsr_fault did not record the reserved-bit refusal before the fault control"
        for name in ("mxcsr_badload", "mxcsr_fault"):
            state = fresh_state()
            host.set(CALLER)
            try:
                getattr(lib, name)(page, DEFAULT | 1 << 13, 0, ctypes.addressof(state))
            except OSError as exc:
                fault = str(exc)
            else:
                return f"{label} {name} returned from an inaccessible load"
            if host.get() != CALLER:
                return f"{label} {name}: MXCSR {host.get():#x} after the fault ({fault}), expected {CALLER:#x}"
            if status(lib, name) != 0:
                return f"{label} {name}: stale refusal status after the fault ({fault})"
    finally:
        kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
        kernel32.VirtualFree(ctypes.c_void_p(page), 0, 0x8000)
    return None


CHILD = """
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
fn = lib.mxcsr_after
fn.argtypes, fn.restype = [ctypes.c_uint64] * 3 + [ctypes.c_void_p], ctypes.c_uint64
state = (ctypes.c_ubyte * 256)()
print("calling", flush=True)
try:
    fn(0, int(sys.argv[2]), 0, ctypes.addressof(state))
except OSError as exc:
    print("fault:", exc, flush=True)
else:
    print("returned", flush=True)
"""


def native_reserved(path):
    """Reference observation: what a native LDMXCSR of a word with a reserved bit does, in a child process."""
    child = subprocess.run([sys.executable, "-I", "-c", CHILD, str(Path(path).resolve()), str(0x11F80)],
                           capture_output=True, text=True, timeout=60)
    return child.returncode, child.stdout.strip().replace("\n", " | "), child.stderr.strip()


def reads_in(path):
    """LDMXCSR and STMXCSR reachable from each export of an /O2 DLL, following every branch."""
    with pefile.PE(str(path), fast_load=True) as pe:
        base, image = pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image()
        found = {}
        for name in EXPORTS:
            pending, seen, count = [base + export_rva(pe, name)], set(), 0
            while pending:
                ip = pending.pop()
                while ip not in seen:
                    seen.add(ip)
                    if not base <= ip < base + len(image):
                        break
                    instruction = Decoder(64, image[ip - base:ip - base + 15], ip=ip).decode()
                    count += instruction.mnemonic in MXCSR_MNEMONICS
                    flow = instruction.flow_control
                    if flow == FlowControl.RETURN:
                        break
                    if flow in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
                        pending.append(instruction.near_branch_target)
                        if flow == FlowControl.UNCONDITIONAL_BRANCH:
                            break
                    elif flow == FlowControl.CALL and instruction.near_branch_target:
                        pending.append(instruction.near_branch_target)
                    elif flow not in (FlowControl.NEXT, FlowControl.CALL, FlowControl.INDIRECT_CALL):
                        break
                    ip = instruction.next_ip
            found[name] = count
    return found


def check(harness_path, native_path, lifted_path, *extra_paths):
    host = Host(harness_path)
    original = host.get()
    try:
        refused = rejected_by_default(native_path)
        print(f"the default profile refuses each of the {len(refused)} MXCSR instructions on its own: "
              f"{refused.count('LDMXCSR')} LDMXCSR, {refused.count('STMXCSR')} STMXCSR")
        native, lifted = load(native_path, False), load(lifted_path, True)
        problem = lifted_violation(host, native, lifted, "lifted")
        if problem:
            raise AssertionError(problem)
        print("lifted: all four rounding modes round-trip and change ADDPS and CVTSS2SI as natively, with and "
              "without sticky flags; adjacent bytes unchanged; the caller's word "
              f"{CALLER:#x} restored after return, refusal and a guest memory fault; refused words "
              f"{', '.join(f'{w:#x}={r}' for w, r in REFUSED.items())} stopped the guest before its next store")
        code, out, err = native_reserved(native_path)
        print(f"native reference, LDMXCSR of {0x11F80:#x} in a child process: exit {code:#x} {out} {err}")
        for path in extra_paths:
            stem = Path(path).stem
            if stem.endswith("_o2"):
                reads = reads_in(path)
                short = {n: c for n, c in reads.items() if c < SOURCE_READS[n]}
                if short:
                    raise AssertionError(f"{stem}: fewer LDMXCSR/STMXCSR than the source in {short}")
                problem = lifted_violation(host, native, load(path, True), stem)
                if problem:
                    raise AssertionError(problem)
                print(f"{stem}: the /O2 build keeps every rule, and every export still reaches at least its "
                      f"source's LDMXCSR and STMXCSR: {reads}")
                continue
            variant = stem.rsplit("_", 1)[-1]
            problem = lifted_violation(host, native, load(path, True), f"control {variant}")
            print(f"control {variant}: {problem}")
            if problem is None:
                raise AssertionError(f"control {variant} kept every rule")
    finally:
        host.set(original)


if __name__ == "__main__":
    mode, *args = sys.argv[1:]
    if mode == "emit":
        image, output, *variant = args
        emit(Path(image), Path(output), *variant)
    elif mode == "check":
        check(*args)
    else:
        raise ValueError(mode)
