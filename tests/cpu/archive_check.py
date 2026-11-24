"""Check the archive reader against independent byte, status and callback observations."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile

from exe_entry_check import Observation, check
from mapped_exe_chain_check import run_mapped
from native_map_check import Mapper, STATUS

FIRST_RECORD = bytes.fromhex("08 00 04 82 24 25 8f 80 7f")
SECOND_RECORD = bytes.fromhex("08 00 04 84 24 25 8f 80 7f")

# Issue #1293: when Control Flow Guard is enforced for a process, ntdll accepts a longjmp target only inside an
# image the Windows loader registered (or under a process-wide protected policy). pe_map maps blast_source
# privately, so blast's out-of-input longjmp fails fast with FAST_FAIL_INVALID_LONGJUMP_TARGET. The pull that
# returns no input ("input 0" in a trace) is the only path to that longjmp. The parent marks those mapped runs
# with LONGJMP_ENV; the mapped child then reads its own effective CFG policy before it maps anything and, when
# CFG is active, prints CFG_SKIP and exits with SKIP instead of failing fast. Every other run is unchanged.
LONGJMP_EVENT = "input 0"
LONGJMP_ENV = "X2W_MAPPED_LONGJMP_CASE"
CFG_SKIP = "skip: CFG is active in the pe_map runner; blast's out-of-input longjmp would fail fast (#1293)"
SKIP = 77
PROCESS_CONTROL_FLOW_GUARD_POLICY = 7


def cfg_active(kernel32=None):
    """The EnableControlFlowGuard bit of this process's effective CFG policy; raises if it cannot be read."""
    from ctypes import wintypes
    k = kernel32 or ctypes.WinDLL("kernel32", use_last_error=True)
    k.GetCurrentProcess.restype = wintypes.HANDLE
    k.GetProcessMitigationPolicy.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    k.GetProcessMitigationPolicy.restype = wintypes.BOOL
    flags = ctypes.c_uint32()
    if not k.GetProcessMitigationPolicy(k.GetCurrentProcess(), PROCESS_CONTROL_FLOW_GUARD_POLICY,
                                        ctypes.byref(flags), ctypes.sizeof(flags)):
        raise OSError(f"cannot read the CFG policy of the pe_map runner: error {ctypes.get_last_error()}")
    return bool(flags.value & 1)


def run_mapped_case(command, data, events, timeout, env=None):
    """Run the mapped runner for one case. Returns (child, skipped): skipped only for a case that reaches
    blast's longjmp and whose runner reported CFG_SKIP with SKIP."""
    import os
    reaches_longjmp = LONGJMP_EVENT in events
    env = dict(os.environ if env is None else env)
    env.pop(LONGJMP_ENV, None)
    if reaches_longjmp:
        env[LONGJMP_ENV] = "1"
    child = subprocess.run(command, input=data, capture_output=True, timeout=timeout, env=env)
    skipped = reaches_longjmp and child.returncode == SKIP and CFG_SKIP.encode("ascii") in child.stderr
    return child, skipped


def report_cfg_skips(skipped):
    """Print the skipped mapped cases and return the exit status for the test."""
    if not skipped:
        return 0
    print(f"skip: CFG is active in the pe_map runner, so its blast longjmp cases did not run (#1293): "
          f"{', '.join(skipped)}")
    return SKIP


def wider_exit_control(native):
    # The startup consumes AL. Replace its zero-extension with MOV ECX,EAX
    # and a NOP. The status in AH and above was never established on error.
    from archive.translate_reader import BINDINGS, HOST
    from cpu_stdio_exe_aot import translate

    with pefile.PE(str(native)) as pe, TemporaryDirectory() as tmp:
        entry = pe.OPTIONAL_HEADER.AddressOfEntryPoint
        unwind, = (item.struct for item in pe.DIRECTORY_ENTRY_EXCEPTION if item.struct.BeginAddress == entry)
        code = pe.get_data(entry, unwind.EndAddress - entry)
        original = bytes.fromhex("0f b6 c8")
        check("unique MOVZX ECX,AL in pinned startup", code.count(original), 1)
        offset = pe.get_offset_from_rva(entry + code.index(original))
        image = bytearray(native.read_bytes())
        image[offset:offset + 3] = bytes.fromhex("89 c1 90")
        control = Path(tmp) / native.name
        control.write_bytes(image)
        try:
            translate(control, BINDINGS, HOST, allow_process_exit=True)
        except ValueError as error:
            if "callee argument rcx is undefined" not in str(error):
                raise
        else:
            raise AssertionError("wide exit status accepted from a byte-sized result")


def module_names(image):
    with pefile.PE(str(image)) as pe:
        return {module.dll.decode("ascii").lower() for module in pe.DIRECTORY_ENTRY_IMPORT}


def mapped_exports(mapper, image, providers):
    base, status, detail, _ = mapper(image, providers)
    if not base or status != STATUS["ok"]:
        raise AssertionError(f"mapping {image.name}: {status}, {detail}")
    with pefile.PE(str(image)) as pe:
        return {entry.name: mapper.export(base, entry.name) for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols}


def mapped_archive(mapper_path, native, codec, source=None):
    if source is not None and os.environ.get(LONGJMP_ENV) == "1":
        try:
            active = cfg_active()
        except OSError as error:
            sys.stderr.write(f"{error}\n")
            sys.exit(1)
        if active:
            sys.stderr.write(CFG_SKIP + "\n")
            sys.exit(SKIP)
    # DXGI loads d3d10warp.dll from the process's directory first. This host is python.exe, so load the
    # app-local WARP (OFFSCREEN_WARP_DIR) beside the native EXE explicitly, as the native run finds it.
    warp = native.with_name("d3d10warp.dll")
    if warp.exists():
        ctypes.WinDLL(str(warp))
    if source is None:
        provider = ctypes.CDLL(str(codec))
    else:
        mapper = Mapper(mapper_path)
        # Keep all three mappings alive through RtlExitUserProcess in this child.
        blast = mapped_exports(mapper, source, {"vcruntime140.dll": ctypes.CDLL("vcruntime140.dll")})
        hosts = {name: ctypes.CDLL(name) for name in module_names(codec) - {source.name.lower()}}
        provider = mapped_exports(mapper, codec, {**hosts, source.name.lower(): blast})
    run_mapped(mapper_path, native, {codec.name.lower(): provider, "ntdll.dll": ctypes.WinDLL("ntdll")})
    raise AssertionError("the mapped Zig startup returned from process termination")


def rejected_output(decode):
    # blast must pass the caller's actual callback contexts and propagate the refusal.
    byte_ptr = ctypes.POINTER(ctypes.c_ubyte)
    input_type = ctypes.CFUNCTYPE(ctypes.c_uint, ctypes.c_void_p, ctypes.POINTER(byte_ptr))
    output_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, byte_ptr, ctypes.c_uint)
    decode.argtypes = [input_type, ctypes.c_void_p, output_type, ctypes.c_void_p,
                       ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(byte_ptr)]
    decode.restype = ctypes.c_int
    compressed = bytes.fromhex("00 04 82 24 25 8f 80 7f")
    payload = (ctypes.c_ubyte * len(compressed)).from_buffer_copy(compressed)
    trace = []

    @input_type
    def supply(context, pointer):
        pointer[0] = ctypes.cast(payload, byte_ptr)
        trace.append(("input", context))
        return len(payload) if len(trace) == 1 else 0

    @output_type
    def refuse(context, pointer, size):
        trace.append(("output", context, ctypes.string_at(pointer, size)))
        return 1

    result = decode(supply, 101, refuse, 202, None, None)
    check("output refusal", (result, trace), (1, [("input", 101), ("output", 202, b"AIAIAIAIAIAIA")]))


def window_record(copies):
    """DCL stream: three literals, overlapping 512-byte copies at distance 3, final literal and end."""
    bits = []

    def put(value, width):
        bits.extend((value >> n) & 1 for n in range(width))

    def literal(byte):
        put(0, 1)
        put(byte, 8)

    for byte in b"ABC":
        literal(byte)
    for _ in range(copies):
        put(1, 1)
        put(0, 7)    # Length symbol 15 (base 264).
        put(248, 8)  # 264 + 248 = 512.
        put(3, 2)    # Distance symbol 0.
        put(2, 4)    # Distance 1 + 2 = 3.
    literal(ord("Z"))
    put(1, 1)
    put(0, 7)
    put(255, 8)      # Length 519 ends the stream.
    packed = bytes(sum(bit << n for n, bit in enumerate(bits[start:start + 8]))
                   for start in range(0, len(bits), 8))
    payload = b"\x00\x04" + packed
    return bytes([len(payload)]) + payload


def compare(mapper, native, lifted, codec, *blast_images):
    wider_exit_control(native)
    first, second = FIRST_RECORD, SECOND_RECORD
    full = ["alloc 8", "input 8", "output 13", "free"]
    partial = ["alloc 8", "input 4", "input 0", "output 1", "free"]
    cases = [
        (first + second, b"AIAIAIAIAIAIABIBIBIBIBIBIB", 0, full + full + ["completed 2"]),
        (first + second[:5], b"AIAIAIAIAIAIA", 2, full + partial + ["completed 1"]),
    ]
    mapped = [sys.executable, "-B", __file__, "--mapped", mapper, native, codec]
    if blast_images:
        source, translated, adapter = blast_images
        for dll in (source, translated):
            rejected_output(ctypes.CDLL(str(dll)).blast)
        mapped.append(source)
        # 3 + 8 * 512 + 1 = 4100 bytes. The distance-three match straddles
        # the first flush, then a distinct literal verifies decoding continued.
        accepted = window_record(8)
        refused = window_record(16)
        first_output = b"AIAIAIAIAIAIA"
        cases.extend([
            (first + accepted, first_output + b"ABC" * 1366 + b"AZ", 0, full + [
                f"alloc {accepted[0]}", f"input {accepted[0]}", "output 4096", "output 4", "free", "completed 2"]),
            (first + refused, first_output, 2, full + [
                f"alloc {refused[0]}", f"input {refused[0]}", "output 4096", "output 4096",
                "refused after 4096", "free", "completed 1"]),
        ])
        check("native codec binding", source.name.lower() in module_names(codec), True)
        check("AOT reader binding", adapter.name.lower() in module_names(lifted), True)
        check("AOT decoder binding", translated.name.lower() in module_names(adapter), True)
        for image in (lifted, adapter, translated):
            check("no native decoder fallback", module_names(image) & {source.name.lower(), codec.name.lower()}, set())
    commands = ([native], mapped, [lifted])
    skipped = []
    for data, output, code, events in cases:
        trace = ("\n".join(events) + "\n").encode("ascii")
        for command in commands:
            if command is mapped:
                child, skip = run_mapped_case(command, data, events, 15)
                if skip:
                    skipped.append(f"{len(data)} input bytes")
                    continue
            else:
                child = subprocess.run(command, input=data, capture_output=True, timeout=15)
            check(str(command), Observation(child.stderr, child.stdout, child.returncode),
                  Observation(trace, output, code))
        print(f"{len(data)} input bytes: {len(output)} published bytes, exit {code}, {events}")
    return report_cfg_skips(skipped)


if __name__ == "__main__":
    if sys.argv[1] == "--mapped":
        mapped_archive(*map(Path, sys.argv[2:]))
    sys.exit(compare(*map(Path, sys.argv[1:])))
