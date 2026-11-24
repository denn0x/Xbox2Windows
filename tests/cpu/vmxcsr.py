"""VLDMXCSR and VSTMXCSR in the opt-in host-MXCSR profile, reusing the rules of tests/cpu/mxcsr.py.

vmxcsr.asm has the exports of mxcsr.asm with the VEX forms and two exports that mix legacy and VEX forms. Two
parts keep a native capability skip from hiding a translated failure:

- check translated: the translation against the fixed expectations of mxcsr.py, the default-profile refusal, the
  malformed VEX encodings and the controls. It needs no AVX and never exits 77.
- check native: the native comparison, which needs AVX and OSXSAVE with XCR0 SSE and AVX state; without them it
  exits 77. It also records the native #UD of a malformed VEX encoding in a child process.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Decoder, Mnemonic as M

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mxcsr
from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges
from ffx_version_query import INTEGER_HEADER, MXCSR_HEADER, Mxcsr, lower, mxcsr_entry, region

MIXED = ("mix_ld_vst", "mix_vld_st")
EXPORTS = (*mxcsr.EXPORTS, *MIXED)
# Undefined encodings next to VLDMXCSR [rax] (c5 f8 ae 10): decoded as invalid, #UD natively. VEX.W is ignored
# (c4 e1 f8 ae 10 is still VLDMXCSR), so it is not among them.
MALFORMED = {"VEX.L=1": "c5fcae10", "VEX.L=1 three-byte": "c4e17cae10", "vvvv!=1111": "c5b8ae10",
             "pp=01": "c5f9ae10", "register operand": "c5f8aed0", "map 0F38": "c4e278ae10"}
# Undefined encodings next to VSTMXCSR [rax] (c5 f8 ae 18, the /3 form), same defects as above.
MALFORMED_STORE = {"VEX.L=1": "c5fcae18", "VEX.L=1 three-byte": "c4e17cae18", "vvvv!=1111": "c5b8ae18",
                   "pp=01": "c5f9ae18", "register operand": "c5f8aed8", "map 0F38": "c4e278ae18"}
# The valid VEX.128 forms the malformed ones above are one defect away from, with the marker each lowers to.
WELL_FORMED = {"VLDMXCSR": ("c5f8ae10", "_mm_setcsr(want)"), "VLDMXCSR VEX.W=1": ("c4e1f8ae10", "_mm_setcsr(want)"),
               "VSTMXCSR": ("c5f8ae18", "_mm_getcsr()"), "VSTMXCSR VEX.W=1": ("c4e1f8ae18", "_mm_getcsr()")}
CONTROLS = {"noload": [("_mm_setcsr(want);", "(void)want;")], "fixed": [(", _mm_getcsr());", ", 0x1f80u);")],
            # The unmasked-exception refusal never fires, so mxcsr_after loads 0x1d80.
            "unmasked": [("(want & 0x1f80u) != 0x1f80u", "(want & 0x1f80u) == 0x1f81u")]}
# One instruction of a mixed export each: variant -> (export, literal, replacement). The control must break a rule
# in that export, so each of its four MXCSR instructions has a check that notices when it is wrong.
SINGLE = {"mixldload": ("mix_ld_vst", "_mm_setcsr(want);", "(void)want;"),
          "mixvstread": ("mix_ld_vst", ", _mm_getcsr());", ", 0x1f80u);"),
          "mixvldload": ("mix_vld_st", "_mm_setcsr(want);", "(void)want;"),
          "mixstread": ("mix_vld_st", ", _mm_getcsr());", ", 0x1f80u);")}


def emit(image, output, variant="lifted"):
    parts = [MXCSR_HEADER + INTEGER_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            code = lower(region(pe, *bounds), ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {},
                         host_mxcsr=Mxcsr(mxcsr.REJECT))
            if variant in SINGLE and SINGLE[variant][0] == name:
                _, literal, replacement = SINGLE[variant]
                if code.count(literal) != 1:
                    raise AssertionError(f"variant {variant}: expected one {literal!r} in {name}")
                code = code.replace(literal, replacement)
            parts.append(f"static __declspec(noinline) word {name}_body(word a, word b, word c, word state, "
                         f"int* mxcsr_status) {{\n{code}\n}}\n")
            parts.append(mxcsr_entry(name, f"{name}_body"))
    text = "".join(parts)
    for literal, replacement in CONTROLS.get(variant, []):
        if literal not in text:
            raise AssertionError(f"variant {variant} changed nothing for {literal!r}")
        text = text.replace(literal, replacement)
    output.write_text(text, encoding="ascii")


def rejected_by_default(image):
    """The default profile refuses every MXCSR instruction of every export, the mixed ones included: each export is
    lowered with every earlier MXCSR instruction replaced by a no-op, so each one is the refused instruction once."""
    refused = []
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in EXPORTS:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            body = region(pe, *bounds)
            sites = [i for i in body if i.mnemonic in mxcsr.MXCSR_MNEMONICS]
            for k, site in enumerate(sites):
                skip = {earlier.ip: ";" for earlier in sites[:k]}
                try:
                    lower(body, ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {}, host_ops=skip)
                except ValueError as exc:
                    if "unsupported query operation" not in str(exc) or f"{site.ip:#x}" not in str(exc):
                        raise AssertionError(f"{name}: expected the refusal of {site}, got {exc}")
                    refused.append(mxcsr.MXCSR_MNEMONICS[site.mnemonic])
                else:
                    raise AssertionError(f"{name}: the default profile lowered {site}")
    return refused


def malformed_lowering():
    """Malformed VEX loads and stores are refused by the lowering itself, at their address, in the host-MXCSR
    profile, and the well-formed forms next to them (VEX.W included) lower to the host MXCSR access."""
    start = 0x1000
    for form, table in (("VLDMXCSR", MALFORMED), ("VSTMXCSR", MALFORMED_STORE)):
        for label, hex_bytes in table.items():
            body = list(Decoder(64, bytes.fromhex(hex_bytes) + b"\xc3", ip=start))
            if not body[0].is_invalid:
                raise AssertionError(f"{form} {label} ({hex_bytes}) decodes as {body[0]}, expected invalid")
            try:
                lower(body, [], {}, {}, {}, host_mxcsr=Mxcsr(mxcsr.REJECT))
            except ValueError as exc:
                if f"{start:#x}" not in str(exc):
                    raise AssertionError(f"{form} {label}: the refusal does not name {start:#x}: {exc}")
            else:
                raise AssertionError(f"{form} {label} ({hex_bytes}) was lowered")
    for label, (hex_bytes, marker) in WELL_FORMED.items():
        body = list(Decoder(64, bytes.fromhex(hex_bytes) + b"\xc3", ip=start))
        code = lower(body, ["rcx = a; rdx = b; r8 = c; r9 = state;"], {}, {}, {}, host_mxcsr=Mxcsr(mxcsr.REJECT))
        if body[0].is_invalid or marker not in code:
            raise AssertionError(f"{label} ({hex_bytes}) did not lower to {marker}: {code}")
    return len(MALFORMED) + len(MALFORMED_STORE)


def malformed_refused():
    """The malformed VEX forms never reach the lowering: they decode as invalid."""
    for label, hex_bytes in MALFORMED.items():
        instruction = Decoder(64, bytes.fromhex(hex_bytes) + b"\xc3", ip=0x1000).decode()
        if not instruction.is_invalid:
            raise AssertionError(f"{label} ({hex_bytes}) decodes as {instruction}, expected an invalid instruction")
    if Decoder(64, bytes.fromhex("c4e1f8ae10"), ip=0x1000).decode().mnemonic != M.VLDMXCSR:
        raise AssertionError("VEX.W=1 VLDMXCSR no longer decodes as VLDMXCSR")
    return list(MALFORMED)


def load(path):
    lib = mxcsr.load(path, True)
    for name in MIXED:
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [mxcsr.U64, mxcsr.U64, mxcsr.U64, ctypes.c_void_p], mxcsr.U64
    return lib


def mixed_violation(host, lib, label):
    """Both mixed exports, translated: the word loaded by one form reads back through the other, and the boundary
    restores the caller's word."""
    for name in MIXED:
        for rc in range(4):
            word = mxcsr.DEFAULT | rc << 13
            state = mxcsr.fresh_state()
            host.set(mxcsr.CALLER)
            result = getattr(lib, name)(0, word, 0, ctypes.addressof(state))
            readback = struct.unpack_from("<I", state, 0x70)[0]
            if (result, readback & ~0x3F, host.get()) != (7, word & ~0x3F, mxcsr.CALLER):
                return (f"{label} {name}({word:#x}): (return, read-back, caller MXCSR) "
                        f"{(result, hex(readback), hex(host.get()))}")
    # A refused word in the VEX load of a mixed export: the legacy read-back after it must not run.
    state = mxcsr.fresh_state()
    host.set(mxcsr.CALLER)
    result = lib.mix_vld_st(0, 0x1D80, 0, ctypes.addressof(state))
    untouched = struct.unpack_from("<I", state, 0x70)[0] == int.from_bytes([mxcsr.FILL] * 4, "little")
    got = (result, mxcsr.status(lib, "mix_vld_st"), untouched, host.get())
    if got != (0, mxcsr.MXCSR_STATUS["unmasked"], True, mxcsr.CALLER):
        return f"{label} mix_vld_st(0x1d80): (return, status, read-back untouched, caller MXCSR) {got}"
    return None


def check_translated(harness_path, native_path, lifted_path, *extra_paths):
    host = mxcsr.Host(harness_path)
    original = host.get()
    try:
        refused = rejected_by_default(native_path)
        print(f"the default profile refuses each of the {len(refused)} MXCSR instructions of every export on its own: "
              + ", ".join(f"{refused.count(n)} {n}" for n in ("VLDMXCSR", "VSTMXCSR", "LDMXCSR", "STMXCSR")))
        print(f"malformed VEX forms decode as invalid and never reach the lowering: {', '.join(malformed_refused())}")
        lifted = load(lifted_path)
        problem = mxcsr.lifted_violation(host, None, lifted, "lifted") or mixed_violation(host, lifted, "lifted")
        if problem:
            raise AssertionError(problem)
        print("lifted, against the fixed expectations: every rounding mode round-trips through VLDMXCSR/VSTMXCSR and "
              "changes ADDPS and CVTSS2SI, refusals stop the guest, the caller's word is restored after return, "
              "refusal and both faults, and the mixed legacy/VEX exports read back the loaded word")
        for path in extra_paths:
            stem = Path(path).stem
            if stem.endswith("_o2"):
                reads = mxcsr.reads_in(path)
                short = {n: c for n, c in reads.items() if c < mxcsr.SOURCE_READS[n]}
                if short:
                    raise AssertionError(f"{stem}: fewer MXCSR instructions than the source in {short}")
                problem = mxcsr.lifted_violation(host, None, load(path), stem) or mixed_violation(host, load(path), stem)
                if problem:
                    raise AssertionError(problem)
                print(f"{stem}: the /O2 build keeps every rule ({reads})")
                continue
            variant = stem.rsplit("_", 1)[-1]
            problem = (mxcsr.lifted_violation(host, None, load(path), f"control {variant}")
                       or mixed_violation(host, load(path), f"control {variant}"))
            print(f"control {variant}: {problem}")
            if problem is None:
                raise AssertionError(f"control {variant} kept every rule")
            if variant in SINGLE and f" {SINGLE[variant][0]}(" not in problem:
                raise AssertionError(f"control {variant} broke a rule, but not in {SINGLE[variant][0]}: {problem}")
    finally:
        host.set(original)


def check_malformed():
    """The lowering alone, no fixture: runs wherever the Python tools run."""
    count = malformed_lowering()
    print(f"{count} malformed VEX loads and stores are refused by the lowering at their address; the well-formed "
          "forms, VEX.W included, lower to the host MXCSR access")


CHILD = """
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
fn = lib.mxcsr_leak
fn.argtypes, fn.restype = [ctypes.c_uint64] * 3 + [ctypes.c_void_p], ctypes.c_uint64
state = (ctypes.c_ubyte * 256)()
print("calling", flush=True)
try:
    fn(0, 0x1f80, 0, ctypes.addressof(state))
except OSError as exc:
    print("fault:", exc, flush=True)
else:
    print("returned", flush=True)
"""


def check_native(harness_path, native_path, lifted_path):
    host = mxcsr.Host(harness_path)
    host.lib.has_avx_os.argtypes, host.lib.has_avx_os.restype = [], ctypes.c_uint32
    if not host.lib.has_avx_os():
        print("skip: this host cannot execute VEX instructions natively (AVX, OSXSAVE or XCR0 SSE/AVX state missing)")
        raise SystemExit(77)
    original = host.get()
    try:
        native = load(native_path)
        problem = mxcsr.lifted_violation(host, native, load(lifted_path), "lifted")
        if problem:
            raise AssertionError(problem)
        for name in MIXED:
            for rc in range(4):
                word = mxcsr.DEFAULT | rc << 13
                results = []
                for lib in (native, load(lifted_path)):
                    state = mxcsr.fresh_state()
                    host.set(mxcsr.CALLER)
                    getattr(lib, name)(0, word, 0, ctypes.addressof(state))
                    results.append(struct.unpack_from("<I", state, 0x70)[0])
                    host.set(mxcsr.CALLER)
                if results[0] != results[1]:
                    raise AssertionError(f"{name}({word:#x}): native read-back {results[0]:#x}, lifted {results[1]:#x}")
        print("native and translated VLDMXCSR/VSTMXCSR match in every rounding mode and in the mixed exports")
    finally:
        host.set(original)
    # Reference observation: a malformed VEX.L=1 VLDMXCSR in the native fixture, in a child process.
    data = bytearray(Path(native_path).read_bytes())
    with pefile.PE(data=bytes(data), fast_load=True) as pe:
        (bounds,) = function_ranges(pe, export_rva(pe, "mxcsr_leak"))
        site = next(i for i in region(pe, *bounds) if i.mnemonic == M.VLDMXCSR)
        offset = pe.get_offset_from_rva(site.ip)
    # VEX.L is bit 2 of the second byte of a two-byte (C5) prefix and of the third byte of a three-byte (C4) one.
    l_byte = {0xC5: offset + 1, 0xC4: offset + 2}.get(data[offset])
    if l_byte is None or data[l_byte] & 0x04:
        raise AssertionError(f"unexpected VLDMXCSR encoding in mxcsr_leak: {bytes(data[offset:offset + 6]).hex()}")
    data[l_byte] |= 0x04  # VEX.L = 1
    with TemporaryDirectory() as folder:
        patched = Path(folder) / "malformed.dll"
        patched.write_bytes(data)
        child = subprocess.run([sys.executable, "-I", "-c", CHILD, str(patched)], capture_output=True, text=True,
                               timeout=60)
    lines = child.stdout.strip().splitlines()
    if child.returncode or child.stderr or lines[:1] != ["calling"] or "0xc000001d" not in "".join(lines[1:]).lower():
        raise AssertionError(f"native VEX.L=1 VLDMXCSR: exit {child.returncode & 0xFFFFFFFF:#x}, stdout "
                             f"{child.stdout!r}, stderr {child.stderr!r}; expected STATUS_ILLEGAL_INSTRUCTION")
    print(f"native reference: VLDMXCSR with VEX.L=1 raises STATUS_ILLEGAL_INSTRUCTION in a child process ({lines[1]})")


if __name__ == "__main__":
    mode, *args = sys.argv[1:]
    if mode == "emit":
        image, output, *variant = args
        emit(Path(image), Path(output), *variant)
    elif mode == "check":
        part, *rest = args
        {"translated": check_translated, "native": check_native, "malformed": check_malformed}[part](*rest)
    else:
        raise ValueError(mode)
