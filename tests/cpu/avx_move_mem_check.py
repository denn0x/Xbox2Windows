"""Memory-source forms of the AVX logical ops: native, lifted and model comparison, mutation and rejection controls."""

import ctypes
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_avx_move_aot import lift_code, translate
from cpu_leaf_aot import export_rva

BUFFER_SIZE = 96
OUT_SIZE = 32
OUT_FILL = b"\x5A" * OUT_SIZE
GUARD = bytes(0xA0 + i for i in range(32))
SHIFTS = (0, 1, 7, 16)
RANDOM_CASES = 48

# (export, op, is_256_bit, base, displacement, destination is the register source)
# Every export reads the register source from the first 32 bytes of the RCX buffer,
# seeds a separate destination from the first 32 bytes of the RDX buffer and writes
# the YMM result to the first 32 bytes of the R8 buffer.
SPECS = (
    ("op_andpd_mem128", "andpd", False, "rdx", 16, False),
    ("op_andps_mem128", "andps", False, "rcx", 32, True),
    ("op_andnpd_mem128", "andnpd", False, "r8", 32, False),
    ("op_andnps_mem128", "andnps", False, "rdx", 64, True),
    ("op_orpd_mem128", "orpd", False, "rcx", 64, False),
    ("op_orps_mem128", "orps", False, "r8", 64, True),
    ("op_xorpd_mem128", "xorpd", False, "rdx", 48, False),
    ("op_xorps_mem128", "xorps", False, "rdx", 0, True),
    ("op_andpd_mem256", "andpd", True, "rcx", 32, False),
    ("op_andps_mem256", "andps", True, "r8", 32, True),
    ("op_andnpd_mem256", "andnpd", True, "rdx", 64, False),
    ("op_andnps_mem256", "andnps", True, "rdx", 32, True),
    ("op_orpd_mem256", "orpd", True, "rcx", 40, False),
    ("op_orps_mem256", "orps", True, "rdx", 8, True),
    ("op_xorpd_mem256", "xorpd", True, "r8", 64, False),
    ("op_xorps_mem256", "xorps", True, "rcx", 64, True),
)
EXPORTS = tuple(spec[0] for spec in SPECS)
OPS = ("andpd", "andps", "andnpd", "andnps", "orpd", "orps", "xorpd", "xorps")
# The pd and ps forms of one operation compute the same bits, so a wrong-operation model
# is another operation, not the other element type.
OTHER_OPS = {op: tuple(other for other in ("andpd", "andnpd", "orpd", "xorpd") if other[:-2] != op[:-2])
             for op in OPS}

# What the ALTER_MUT fixture does differently: the vandn* exports take the register
# source from memory and the memory source from the register buffer; the two XMM
# exports below keep the destination's old bits 255:128.
ALTERED = {
    "op_andnpd_mem128": "swap", "op_andnps_mem128": "swap",
    "op_andnpd_mem256": "swap", "op_andnps_mem256": "swap",
    "op_orpd_mem128": "keep_upper", "op_xorps_mem128": "keep_upper",
}
BUFFER_NAMES = ("rcx", "rdx", "r8")
OTHER_BASE = {"rcx": "rdx", "rdx": "r8", "r8": "rcx"}


def pack32(*words):
    return struct.pack("<4I", *words)


def pack64(*words):
    return struct.pack("<2Q", *words)


PATTERNS = (
    bytes(16),
    b"\xff" * 16,
    pack32(*(0x80000000,) * 4),
    pack64(0x8000000000000000, 0x8000000000000000),
    pack32(0x7FC00000, 0x7F800001, 0xFFC00000, 0xFF800001),
    pack64(0x7FF8000000000000, 0x7FF0000000000001),
    pack64(0xFFF8000000000123, 0xFFF4000000000001),
    pack32(0x3F800000, 0x80000000, 0x7FC00000, 0x7F800000),
    pack32(0xFF800000, 0x00000001, 0x007FFFFF, 0x80000001),
    pack64(0x7FEFFFFFFFFFFFFF, 0x0000000000000001),
    b"\x55" * 16,
    b"\xaa" * 16,
    pack64(0x0123456789ABCDEF, 0xFEDCBA9876543210),
    pack32(1, 2, 4, 8),
    pack32(0xFFFFFFFF, 0, 0xFFFFFFFF, 0),
    pack64(0xFFFFFFFFFFFFFFFF, 0),
)


# --- Independent model on the raw bytes of the operands ---

def combine(op, reg, mem, mask):
    """dest = op(register source, memory source); only the register source is inverted by andn."""
    if op.startswith("andn"):
        return (~reg) & mem & mask
    if op.startswith("and"):
        return reg & mem
    if op.startswith("or"):
        return reg | mem
    return reg ^ mem


def expected_output(spec, a, b, c, mutation=None, op=None):
    """Return the 32 output bytes for one case; mutation names a deliberately wrong behavior."""
    _, spec_op, wide, base, disp, alias = spec
    width = 32 if wide else 16
    mask = (1 << (8 * width)) - 1
    if mutation == "base":
        base = OTHER_BASE[base]
    if mutation == "disp":
        disp = disp - 8 if disp >= 8 else disp + 8
    buffers = {"rcx": a, "rdx": b, "r8": c}
    reg = int.from_bytes(a[:width], "little")
    mem = int.from_bytes(buffers[base][disp:disp + width], "little")
    if mutation == "swap":
        reg, mem = mem, reg
    low = combine(op or spec_op, reg, mem, mask).to_bytes(width, "little")
    seed = a[:OUT_SIZE] if alias else b[:OUT_SIZE]
    upper = seed[width:] if mutation == "keep_upper" else bytes(OUT_SIZE - width)
    return low + upper


def build_cases():
    """Deterministic (spec, a, b, c) list: every pattern pair in the memory and register sources, then random data."""
    rng = random.Random(0x4D454D31)
    count = len(PATTERNS)
    cases = []
    for spec in SPECS:
        _, _, wide, base, disp, _ = spec
        width = 32 if wide else 16
        pairs = []
        for pi in range(count):
            for qi in range(count):
                reg = PATTERNS[pi] + PATTERNS[(pi * 5 + qi + 1) % count]
                mem = PATTERNS[qi] + PATTERNS[(qi * 7 + pi + 3) % count]
                pairs.append((reg, mem[:width]))
        for _ in range(RANDOM_CASES):
            pairs.append((None, None))
        for reg, mem in pairs:
            a, b, c = (bytearray(rng.randbytes(BUFFER_SIZE)) for _ in range(3))
            c[:OUT_SIZE] = OUT_FILL
            if reg is not None:
                a[:OUT_SIZE] = reg
                target = {"rcx": a, "rdx": b, "r8": c}[base]
                target[disp:disp + width] = mem
            cases.append((spec, bytes(a), bytes(b), bytes(c)))
    return cases


# --- Native, lifted and altered execution ---

def guarded(payload, shift):
    buf = ctypes.create_string_buffer(32 + len(payload) + 32 + 32 + 64)
    start = (ctypes.addressof(buf) + 32 + 31) // 32 * 32 + shift
    offset = start - ctypes.addressof(buf)
    buf[offset - 32:offset] = GUARD
    buf[offset:offset + len(payload)] = payload
    buf[offset + len(payload):offset + len(payload) + 32] = GUARD
    return buf, start, offset


def read_guarded(buf, offset, length, label):
    raw = buf.raw
    if raw[offset - 32:offset] != GUARD or raw[offset + length:offset + length + 32] != GUARD:
        raise AssertionError(f"guard bytes corrupted around buffer {label}")
    return raw[offset:offset + length]


def invoke(fn, a, b, c, shift=0):
    """Call one export on guarded copies of the three buffers and return its 32 output bytes."""
    regions = [guarded(payload, shift) for payload in (a, b, c)]
    fn(regions[0][1], regions[1][1], regions[2][1])
    after = [read_guarded(buf, offset, BUFFER_SIZE, name)
             for (buf, _, offset), name in zip(regions, BUFFER_NAMES)]
    if after[0] != a or after[1] != b:
        raise AssertionError("an input buffer was modified")
    if after[2][OUT_SIZE:] != c[OUT_SIZE:]:
        raise AssertionError("bytes next to the output window were modified")
    return after[2][:OUT_SIZE]


def bind_exports(dll_path, prefix=""):
    lib = ctypes.CDLL(str(Path(dll_path).resolve()))
    funcs = {}
    for export in EXPORTS:
        fn = getattr(lib, prefix + export)
        fn.argtypes = [ctypes.c_uint64] * 3
        fn.restype = None
        funcs[export] = fn
    return funcs


def host_has_avx():
    if sys.platform == "win32":
        return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(39))  # PF_AVX_INSTRUCTIONS_AVAILABLE
    try:
        return " avx " in Path("/proc/cpuinfo").read_text(encoding="ascii", errors="replace")
    except OSError:
        return False


def require_equal(label, export, index, got, want):
    if got != want:
        raise AssertionError(f"{label} {export} case {index}: got {got.hex()} != expected {want.hex()}")


def check_differential(native_path, lifted_path, altered_path):
    native = bind_exports(native_path)
    lifted = bind_exports(lifted_path, "lifted_")
    altered = bind_exports(altered_path, "lifted_")
    cases = build_cases()

    native_outputs = []
    for index, (spec, a, b, c) in enumerate(cases):
        name = spec[0]
        shift = SHIFTS[index % len(SHIFTS)]
        want = expected_output(spec, a, b, c)
        got_native = invoke(native[name], a, b, c, shift)
        got_lifted = invoke(lifted[name], a, b, c, shift)
        require_equal("native", name, index, got_native, want)
        require_equal("lifted", name, index, got_lifted, want)
        native_outputs.append(got_native)

    # The altered build must match the altered model on every export, and each mutated
    # export must really differ from the unaltered behavior on some of its cases.
    diverged = {name: 0 for name in ALTERED}
    for index, (spec, a, b, c) in enumerate(cases):
        name = spec[0]
        want_alt = expected_output(spec, a, b, c, ALTERED.get(name))
        got_alt = invoke(altered[name], a, b, c, SHIFTS[index % len(SHIFTS)])
        require_equal("altered", name, index, got_alt, want_alt)
        if name in ALTERED and got_alt != native_outputs[index]:
            diverged[name] += 1
    for name, count in diverged.items():
        if not count:
            raise AssertionError(f"altered control {name} did not diverge from the unaltered build")

    # The inputs must tell wrong behavior from right: each wrong model has to contradict the
    # native result on some case of every export it applies to.
    wrong = {}
    for index, (spec, a, b, c) in enumerate(cases):
        name = spec[0]
        wide = spec[2]
        variants = [("base", None), ("disp", None)]
        variants += [("op", other) for other in OTHER_OPS[spec[1]]]
        if spec[1].startswith("andn"):
            variants.append(("swap", None))
        if not wide:
            variants.append(("keep_upper", None))
        for kind, other in variants:
            mutation = None if kind == "op" else kind
            candidate = expected_output(spec, a, b, c, mutation, other)
            if candidate != native_outputs[index]:
                wrong[(name, kind, other)] = wrong.get((name, kind, other), 0) + 1
    for spec in SPECS:
        required = [("base", None), ("disp", None)]
        required += [("op", other) for other in OTHER_OPS[spec[1]]]
        if spec[1].startswith("andn"):
            required.append(("swap", None))
        if not spec[2]:
            required.append(("keep_upper", None))
        for kind, other in required:
            if not wrong.get((spec[0], kind, other)):
                raise AssertionError(f"{spec[0]}: wrong model {kind} {other or ''} matched the native result on every case")

    flipped = bytearray(native_outputs[0])
    flipped[17] ^= 1
    try:
        require_equal("mutation", cases[0][0][0], 0, bytes(flipped), expected_output(*cases[0]))
    except AssertionError:
        pass
    else:
        raise AssertionError("a flipped output bit was not caught by the comparator")

    print(f"{len(cases)} cases across {len(EXPORTS)} exports matched native, lifted and the model bit for bit "
          f"(unaligned pointers {SHIFTS}); altered build diverged on "
          f"{diverged}; "
          f"{len(wrong)} wrong-behavior models were each contradicted by the native result")


# --- Translator acceptance and rejection controls ---

def encode_seq(*instructions):
    encoder = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += encoder.encode(ins, ip)
    return encoder.take_buffer()


LOGICAL_CODES = {
    "andpd": (Code.VEX_VANDPD_XMM_XMM_XMMM128, Code.VEX_VANDPD_YMM_YMM_YMMM256),
    "andps": (Code.VEX_VANDPS_XMM_XMM_XMMM128, Code.VEX_VANDPS_YMM_YMM_YMMM256),
    "andnpd": (Code.VEX_VANDNPD_XMM_XMM_XMMM128, Code.VEX_VANDNPD_YMM_YMM_YMMM256),
    "andnps": (Code.VEX_VANDNPS_XMM_XMM_XMMM128, Code.VEX_VANDNPS_YMM_YMM_YMMM256),
    "orpd": (Code.VEX_VORPD_XMM_XMM_XMMM128, Code.VEX_VORPD_YMM_YMM_YMMM256),
    "orps": (Code.VEX_VORPS_XMM_XMM_XMMM128, Code.VEX_VORPS_YMM_YMM_YMMM256),
    "xorpd": (Code.VEX_VXORPD_XMM_XMM_XMMM128, Code.VEX_VXORPD_YMM_YMM_YMMM256),
    "xorps": (Code.VEX_VXORPS_XMM_XMM_XMMM128, Code.VEX_VXORPS_YMM_YMM_YMMM256),
}
BASES = {"rcx": R.RCX, "rdx": R.RDX, "r8": R.R8}
MEMORY_REASON = "unsupported memory operand at RVA"
REGISTER_ONLY_REASON = "unsupported memory operand for register-domain AVX instruction"


def check_translator(native_path):
    load_ymm0 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))
    load_ymm1 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM1, MemoryOperand(R.RCX))
    store = Instruction.create_mem_reg(Code.VEX_VMOVDQU_YMMM256_YMM, MemoryOperand(R.R8), R.YMM0)
    ret_ins = Instruction.create(Code.RETNQ)
    prefix = (load_ymm1,)

    def logical(op, wide, memory, dest=None):
        code = LOGICAL_CODES[op][1 if wide else 0]
        regs = (R.YMM0, R.YMM1) if wide else (R.XMM0, R.XMM1)
        return Instruction.create_reg_reg_mem(code, dest or regs[0], regs[1], memory)

    # 1) Every op, width, base and displacement shape that is accepted lowers cleanly.
    accepted = 0
    for op in OPS:
        for wide in (False, True):
            for base in BASES.values():
                for disp in (0, 0x20, 0x7F, 0x80, 0x12340, -0x20):
                    size = 0 if disp == 0 else 1 if -0x80 <= disp < 0x80 else 8
                    memory = MemoryOperand(base, displ=disp, displ_size=size)
                    seq = (*prefix, logical(op, wide, memory), store, ret_ins)
                    source, count = lift_code("accept", 0x1000, encode_seq(*seq))
                    assert count == len(seq), (op, wide, base, disp, count)
                    accepted += 1

    # 2) Rejections: the memory form adds no addressing mode and no encoding to the profile.
    evex_masked = Instruction.create_reg_reg_mem(
        Code.EVEX_VANDPS_YMM_K1Z_YMM_YMMM256B32, R.YMM0, R.YMM1, MemoryOperand(R.RCX))
    evex_masked.op_mask = R.K1
    evex_plain = Instruction.create_reg_reg_mem(
        Code.EVEX_VXORPD_XMM_K1Z_XMM_XMMM128B64, R.XMM0, R.XMM1, MemoryOperand(R.RCX))
    segment = logical("orps", True, MemoryOperand(R.RCX))
    segment.segment_prefix = R.FS
    cases = [
        ("evex_masked_mem", (*prefix, evex_masked, store, ret_ins), "unsupported EVEX instruction"),
        ("evex_unmasked_mem", (*prefix, evex_plain, store, ret_ins), "unsupported EVEX instruction"),
        ("index_scale", (*prefix, logical("andps", True, MemoryOperand(R.RCX, R.RDX, 4)), store, ret_ins), MEMORY_REASON),
        ("index_only", (*prefix, logical("xorpd", False, MemoryOperand(R.RCX, R.RDX, 1, 8, 1)), store, ret_ins), MEMORY_REASON),
        ("rip_relative", (*prefix, logical("andnps", True, MemoryOperand(R.RIP, displ=0x1010, displ_size=1)), store, ret_ins), MEMORY_REASON),
        ("address_size_ecx", (*prefix, logical("orpd", True, MemoryOperand(R.ECX)), store, ret_ins), MEMORY_REASON),
        ("address_size_edx_disp", (*prefix, logical("xorps", False, MemoryOperand(R.EDX, displ=0x10, displ_size=1)), store, ret_ins), MEMORY_REASON),
        ("base_rsp", (*prefix, logical("andpd", True, MemoryOperand(R.RSP)), store, ret_ins), MEMORY_REASON),
        ("base_rax", (*prefix, logical("andnpd", True, MemoryOperand(R.RAX)), store, ret_ins), MEMORY_REASON),
        ("base_r9", (*prefix, logical("orps", False, MemoryOperand(R.R9, displ=0x10, displ_size=1)), store, ret_ins), MEMORY_REASON),
        ("segment_prefix", (*prefix, segment, store, ret_ins), "unsupported instruction prefix"),
        ("undefined_source_ymm3", (Instruction.create_reg_reg_mem(
            Code.VEX_VANDNPS_YMM_YMM_YMMM256, R.YMM0, R.YMM3, MemoryOperand(R.RCX)), store, ret_ins),
         "undefined vector register ymm3"),
        ("nonvolatile_destination", (*prefix, logical("xorps", True, MemoryOperand(R.RCX), dest=R.YMM6), ret_ins),
         "nonvolatile vector register"),
        ("register_only_vshufps", (*prefix, Instruction.create_reg_reg_mem_u32(
            Code.VEX_VSHUFPS_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0xE4), store, ret_ins),
         REGISTER_ONLY_REASON),
        ("register_only_vperm2f128", (*prefix, Instruction.create_reg_reg_mem_u32(
            Code.VEX_VPERM2F128_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0x20), store, ret_ins),
         REGISTER_ONLY_REASON),
        ("register_only_vblendps", (*prefix, Instruction.create_reg_reg_mem_u32(
            Code.VEX_VBLENDPS_XMM_XMM_XMMM128_IMM8, R.XMM0, R.XMM1, MemoryOperand(R.RDX), 0x5), store, ret_ins),
         REGISTER_ONLY_REASON),
        ("register_only_vunpcklps", (*prefix, Instruction.create_reg_reg_mem(
            Code.VEX_VUNPCKLPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), store, ret_ins),
         REGISTER_ONLY_REASON),
        ("register_only_vpermilps_var", (*prefix, Instruction.create_reg_reg_mem(
            Code.VEX_VPERMILPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), store, ret_ins),
         REGISTER_ONLY_REASON),
        ("out_of_scope_vaddps_mem", (*prefix, Instruction.create_reg_reg_mem(
            Code.VEX_VADDPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX)), store, ret_ins),
         "unsupported instruction"),
    ]
    for label, seq, reason in cases:
        try:
            lift_code(label, 0x1000, encode_seq(*seq))
        except ValueError as exc:
            if reason not in str(exc):
                raise AssertionError(f"{label}: expected {reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator accepted a rejected control")

    # 3) The same refusals when the instruction sits in a real PE export.
    pe_bytes = Path(native_path).read_bytes()
    with pefile.PE(data=pe_bytes, fast_load=True) as pe:
        entry = export_rva(pe, "op_xorps_mem256")
        file_offset = pe.get_offset_from_rva(entry)
        pdata = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        records = pe.get_data(pdata.VirtualAddress, pdata.Size)
        record = next(i for i in range(0, len(records), 12) if struct.unpack_from("<I", records, i)[0] == entry)
        end_offset = pe.get_offset_from_rva(pdata.VirtualAddress + record + 4)
        capacity = struct.unpack_from("<I", records, record + 4)[0] - entry
    patches = [
        ("pe_evex_mem", encode_seq(*prefix, evex_masked, ret_ins), "unsupported EVEX instruction"),
        ("pe_index_scale", encode_seq(*prefix, logical("andps", True, MemoryOperand(R.RCX, R.RDX, 4)), ret_ins),
         MEMORY_REASON),
        ("pe_address_size", encode_seq(*prefix, logical("orpd", True, MemoryOperand(R.ECX)), ret_ins), MEMORY_REASON),
        ("pe_register_only_mem", encode_seq(*prefix, cases[13][1][1], ret_ins), REGISTER_ONLY_REASON),
    ]
    with TemporaryDirectory() as folder:
        patched = Path(folder) / "patched.dll"
        for label, patch, reason in patches:
            if len(patch) > capacity:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds op_xorps_mem256 ({capacity})")
            damaged = bytearray(pe_bytes)
            damaged[file_offset:file_offset + len(patch)] = patch
            struct.pack_into("<I", damaged, end_offset, entry + len(patch))
            patched.write_bytes(damaged)
            try:
                translate(patched, ["op_xorps_mem256"])
            except ValueError as exc:
                if reason not in str(exc):
                    raise AssertionError(f"{label}: expected {reason!r}, got {exc!r}") from exc
            else:
                raise AssertionError(f"{label}: PE translate accepted a rejected patch")

    # 4) The unpatched fixture lifts in full.
    _, meta = translate(native_path, list(EXPORTS))
    assert [entry[0] for entry in meta] == list(EXPORTS)
    print(f"{accepted} accepted memory forms, {len(cases)} rejection controls and {len(patches)} PE-patched "
          f"rejection controls passed; all {len(EXPORTS)} exports translate")


def main(native_path, lifted_path, altered_path):
    check_translator(native_path)
    if not host_has_avx():
        print("Host CPU/OS lacks AVX; skipping the native differential check.")
        sys.exit(77)
    check_differential(native_path, lifted_path, altered_path)


if __name__ == "__main__":
    main(*sys.argv[1:4])
