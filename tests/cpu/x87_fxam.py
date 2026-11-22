"""Compare native and translated FXAM of the x87 leaf-function profile.

Usage:
    python tests/cpu/x87_fxam.py asm <asm> <def>             regenerate the MASM source and exports
    python tests/cpu/x87_fxam.py emit <dll> <cpp> [control]   translate (and optionally mutate) the fixture
    python tests/cpu/x87_fxam.py check <harness> <native> <lifted> [controls...]

The exports, harness and guards follow tests/cpu/x87_add.py, and the load and store steps use the format of
tests/cpu/x87_moves.py. FXAM sets C3, C2 and C0 to the class of ST(0) and C1 to its sign. After each FNSTSW AX
a record step reads C1 through TEST AH, 02h with JNE and TEST AH, 47h with JP, then AH through the record of
tests/cpu/x87_status.py, then CMP AH with the masked normal, infinity and denormal classes and JE. signed
masks AH with 47h, stores it and compares it with the negative classes. Only flag-neutral MOVs come in
between, and EAX is cleared before the return. emit checks that every decoded x87 instruction is the form
its sequence names. The model classifies the encoding itself and is independent of tools/x87_soft.h.
"""
from pathlib import Path
import platform
import random
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import x87_add as base  # noqa: E402  (tests/cpu is the script directory)
import x87_compare as compare  # noqa: E402
import x87_moves as moves  # noqa: E402
import x87_status as status  # noqa: E402
from x87_add import J, Q, from_binary, m80_bytes  # noqa: E402
from x87_div import SIZE  # noqa: E402

INPUT, OUTPUT = base.INPUT, base.OUTPUT
C1_TESTS = 2  # TEST AH, 02h with JNE, and TEST AH, 47h with JP
CLASS_EQ = (0x04, 0x05, 0x44)  # normal, infinity and denormal after AND AH, 45h
RECORD = C1_TESTS + status.RECORD + len(CLASS_EQ)  # 28 bytes
SIGNED_EQ = (0x07, 0x42, 0x46, 0x03)  # negative infinity, zero, denormal and NaN after AND AH, 47h
SIGNED = 1 + len(SIGNED_EQ)
SW = [("fnstsw",), ("record", 0)]
P80 = [("fld", "m80", 0), ("fld", "m80", 16)]  # ST(0) = the value at 16, ST(1) = the value at 0
SEQUENCES = {
    # FXAM after loads of each format: the m64 and m32 loads bring denormals and the format limits.
    "fxam_m80": ("m80", [("fld", "m80", 0), ("fxam",)] + SW + [("fstp", "m80", 32)]),
    "fxam_m64": ("m64", [("fld", "m64", 0), ("fxam",)] + SW + [("fstp_st", 0)]),
    "fxam_m32": ("m32", [("fld", "m32", 0), ("fxam",)] + SW + [("fstp_st", 0)]),
    # Depth 8, where TOP is 0 again, and depth 2, where ST(1) is not examined.
    "fxam_depth8": ("m64x8", [("fld", "m64", 8 * k) for k in range(8)] + [("fxam",)] + SW + [("fstp_st", 0)] * 8),
    "fxam_st1": ("m80", P80 + [("fxam",)] + SW + [("fstp", "m80", 32), ("fstp_st", 0)]),
    # The sign through AND AH, 47h and CMP AH, as compilers test for negative classes.
    "fxam_signed": ("m80", [("fld", "m80", 0), ("fxam",), ("fnstsw",), ("signed", 0), ("fstp", "m80", 32)]),
    # FCHS and FABS change only the sign that FXAM reports.
    "fchs_fxam": ("m80", [("fld", "m80", 0), ("fchs",), ("fxam",)] + SW + [("fstp", "m80", 32)]),
    "fabs_fxam": ("m80", [("fld", "m80", 0), ("fabs",), ("fxam",)] + SW + [("fstp", "m80", 32)]),
    # Two FXAMs, each read through its own FNSTSW.
    "two_records": ("m80", P80 + [("fxam",), ("fnstsw",), ("record", 0), ("fstp_st", 0), ("fxam",), ("fnstsw",),
                                  ("record", RECORD), ("fstp_st", 0)]),
    # FCOMI leaves C0, C2 and C3 of the FXAM before it; C1 is not read.
    "fxam_fcomi": ("m80", P80 + [("fxam",), ("fcomi", 1), ("fnstsw",), ("crecord", 0), ("fstp", "m80", 32),
                                 ("fstp_st", 0)]),
    # The input dword at 48 selects FXAM or FTST; both paths define C0, C2 and C3, so C1 is not read.
    "joined": ("m80", [("fld", "m80", 0), ("choose",), ("fnstsw",), ("crecord", 0), ("fstp", "m80", 32)]),
}
STEPS = SEQUENCES

FXAM = re.escape("x87_sw = static_cast<uint16_t>((")
CONTROLS = {
    # C1 is always clear.
    "nosign": (re.compile(r"\.se & 0x8000 \? 0x0200 : 0\)"), ".se & 0x8000 ? 0 : 0)"),
    # A denormal or pseudo-denormal is reported as normal.
    "denormal": (re.compile(r"0x4000 : 0x4400\)"), "0x4000 : 0x0400)"),
    # An unnormal, pseudo-infinity or pseudo-NaN is classified by its exponent alone.
    "unnormal": (re.compile(r"\(x87_slot\[\d\]\.sig >> 63\) == 0 \? 0 : "), ""),
    # Infinity and NaN are exchanged.
    "infnan": (re.compile(r"0x0500 : 0x0100\)"), "0x0100 : 0x0500)"),
    # Zero and denormal are exchanged.
    "zerodenormal": (re.compile(r"0x4000 : 0x4400\)"), "0x4400 : 0x4000)"),
}
# Each control must mismatch in this export.
CONTROL_EXPORTS = {"nosign": "fxam_signed", "denormal": "fxam_m80", "unnormal": "fxam_m80", "infnan": "fchs_fxam",
                   "zerodenormal": "fabs_fxam"}


# ---- MASM generation and decoding ------------------------------------------------------------------

def taken(label, offset, jump, value=(1, 0)):
    return status.taken(label, offset, jump, value)


def asm_lines(step, name):
    op, *args = step
    if op == "fxam":
        return ["    fxam"]
    if op == "choose":
        return ["    mov eax, dword ptr [rcx+48]", "    test eax, eax", f"    je {name}_alt", "    fxam",
                f"    jmp {name}_join", f"{name}_alt:", "    ftst", f"{name}_join:"]
    if op == "record":
        o = args[0]
        lines = ["    test ah, 02h", *taken(f"{name}_{o}_c1", o, "jne"),
                 "    test ah, 47h", *taken(f"{name}_{o}_p47", o + 1, "jp")]
        lines += status.asm_lines(("record", o + C1_TESTS), name)[:-1]  # without its final MOV EAX, 0
        o += C1_TESTS + status.RECORD
        for k, value in enumerate(CLASS_EQ):
            lines += [f"    cmp ah, 0{value:02X}h", *taken(f"{name}_{o}_class{k}", o + k, "je")]
        return lines + ["    mov eax, 0"]
    if op == "crecord":
        return status.asm_lines(("record", args[0]), name)
    if op == "signed":
        o = args[0]
        lines = ["    and ah, 47h", f"    mov byte ptr [rdx+{o}], ah"]
        for k, value in enumerate(SIGNED_EQ):
            lines += [f"    cmp ah, 0{value:02X}h", *taken(f"{name}_{o}_neg{k}", o + 1 + k, "je")]
        return lines + ["    mov eax, 0"]
    return status.asm_lines(step, name)


def expected_forms(step):
    op, *args = step
    if op == "fxam":
        return [("fxam", "")]
    if op == "choose":
        return [("fxam", ""), ("ftst", "")]
    if op in ("record", "crecord", "signed"):
        return []
    return status.expected_forms(step)


def write_asm(asm_path, def_path):
    lines = ["; coverage: source cpu_aot_x87_fxam",
             "; Generated by tests/cpu/x87_fxam.py asm; edit SEQUENCES there and regenerate.",
             "; Source-owned differential fixture for FXAM in the x87 leaf-function profile. Each export reads an",
             "; input block at RCX, writes an output block at RDX and returns with the x87 stack empty.", ".code", ""]
    for name, (_, steps) in STEPS.items():
        lines += [f"{name} PROC FRAME", "    .endprolog", *(line for s in steps for line in asm_lines(s, name)),
                  "    ret", f"{name} ENDP", ""]
    lines.append("END")
    Path(asm_path).write_text("\n".join(lines) + "\n", encoding="ascii")
    Path(def_path).write_text("EXPORTS\n" + "".join(f"    {name}\n" for name in STEPS), encoding="ascii")


def lower_export(pe, name):
    from cpu_leaf_aot import export_rva
    from cpu_ranges import function_ranges
    from ffx_version_query import lower, region
    from x87_lower import is_x87

    (bounds,) = function_ranges(pe, export_rva(pe, name))
    body = region(pe, *bounds)
    decoded = [status.decoded_form(i) for i in body if is_x87(i)]
    wanted = [form for s in STEPS[name][1] for form in expected_forms(s)]
    if decoded != wanted:
        raise AssertionError(f"{name} assembled as {decoded}, expected {wanted}")
    return lower(body, ["rcx = a; rdx = b;"], {}, {}, {}, x87=True)


def emit(image, output, control="lifted"):
    import pefile
    from ffx_version_query import INTEGER_HEADER
    from x87_lower import X87_HEADER

    parts = [INTEGER_HEADER, X87_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in SEQUENCES:
            parts.append(f'extern "C" __declspec(dllexport) word {name}(word a, word b) {{\n'
                         f'{lower_export(pe, name)}\n}}\n')
    text = "".join(parts)
    if not re.search(FXAM, text):
        raise AssertionError("no FXAM was lowered")
    if control != "lifted":
        expression, replacement = CONTROLS[control]
        text, count = expression.subn(replacement, text)
        if not count:
            raise AssertionError(f"control {control} changed nothing")
        print(f"control {control}: {count} replacements")
    output.write_text(text, encoding="ascii")


# ---- Independent model -----------------------------------------------------------------------------

# Intel SDM Vol. 1, 8.3.6, and the FXAM entry of Vol. 2: C3, C2 and C0 for each class.
CLASSES = {"unsupported": 0b000, "nan": 0b001, "normal": 0b010, "infinity": 0b011, "zero": 0b100,
           "denormal": 0b110}


def classify(v):
    """The class of an 80-bit encoding (significand, sign and exponent)."""
    sig, se = v
    exponent, integer = se & 0x7FFF, sig >> 63
    if exponent == 0:
        return "zero" if sig == 0 else "denormal"  # a pseudo-denormal (integer bit set) too
    if not integer:
        return "unsupported"  # unnormal, pseudo-infinity and pseudo-NaN
    if exponent == 0x7FFF:
        return "infinity" if sig == J else "nan"
    return "normal"


def fxam_ah(v, top):
    """The known bits of AH after FXAM and FNSTSW AX: C0, C1, C2, TOP and C3."""
    bits = CLASSES[classify(v)]
    c3, c2, c0 = bits >> 2, bits >> 1 & 1, bits & 1
    return c0 | (v[1] >> 15) << 1 | c2 << 2 | top << 3 | c3 << 6


def even(value):
    return bin(value).count("1") % 2 == 0


def record_bytes(ah):
    masked = ah & 0x45
    return (bytes((int(ah & 0x02 != 0), int(even(ah & 0x47)))) + status.record_bytes(ah)
            + bytes(int(masked == value) for value in CLASS_EQ))


def signed_bytes(ah):
    masked = ah & 0x47
    return bytes((masked,)) + bytes(int(masked == value) for value in SIGNED_EQ)


def reference(name, data):
    """The output block, and the class at the last FXAM."""
    slots, out = [], bytearray(b"\xcc" * OUTPUT)
    ah = seen = examined = None

    def read(kind, offset):
        return int.from_bytes(data[offset:offset + SIZE[kind]], "little")

    for op, *args in STEPS[name][1]:
        if op == "fld":
            raw = read(*args)
            slots.append((raw & (2 ** 64 - 1), raw >> 64) if args[0] == "m80" else from_binary(args[0], raw))
        elif op == "fstp":
            v = slots.pop()
            out[args[1]:args[1] + 10] = (v[0] | v[1] << 64).to_bytes(10, "little")
        elif op == "fstp_st":
            slots[-1 - args[0]] = slots[-1]
            slots.pop()
        elif op == "fchs":
            slots[-1] = (slots[-1][0], slots[-1][1] ^ 0x8000)
        elif op == "fabs":
            slots[-1] = (slots[-1][0], slots[-1][1] & 0x7FFF)
        elif op == "fxam":
            examined = ("fxam", slots[-1])
        elif op == "fcomi":
            pass  # C0, C2 and C3 stay; the flags are not read
        elif op == "choose":
            examined = ("fxam", slots[-1]) if read("m32", 48) else ("ftst", compare.compare(slots[-1], (0, 0)))
        elif op == "fnstsw":
            top = (8 - len(slots)) & 7
            kind, value = examined
            ah = fxam_ah(value, top) if kind == "fxam" else status.known_ah(value, top)
            seen = classify(value) if kind == "fxam" else None
        elif op == "record":
            out[args[0]:args[0] + RECORD] = record_bytes(ah)
        elif op == "crecord":
            out[args[0]:args[0] + status.RECORD] = status.record_bytes(ah & ~0x02)
        elif op == "signed":
            out[args[0]:args[0] + SIGNED] = signed_bytes(ah)
        else:
            raise AssertionError(op)
    assert not slots
    return bytes(out), seen


# ---- Inputs ----------------------------------------------------------------------------------------

def cases():
    rng = random.Random(71)
    specials = base.special_m80()
    signed_specials = specials + [(sig, se ^ 0x8000) for sig, se in specials]

    def random_class():
        """A random encoding from each region of the class table, either sign."""
        sign = rng.choice((0, 0x8000))
        kind = rng.randrange(6)
        if kind == 0:  # exponent 0: zero, denormal or pseudo-denormal
            sig = rng.choice((0, rng.getrandbits(63), J | rng.getrandbits(63), 1, J))
            return sig, sign
        if kind == 1:  # unnormal
            return rng.getrandbits(63), sign | rng.randint(1, 0x7FFE)
        if kind == 2:  # normal
            return J | rng.getrandbits(63), sign | rng.randint(1, 0x7FFE)
        if kind == 3:  # exponent 0x7FFF with the integer bit: infinity or NaN
            return rng.choice((J, J | rng.getrandbits(63), J | Q, J | 1)), sign | 0x7FFF
        if kind == 4:  # pseudo-infinity and pseudo-NaN
            return rng.choice((0, rng.getrandbits(63), Q)), sign | 0x7FFF
        return rng.choice(((J, 1), (J, 0x7FFE), (J - 1, 0), (0xFFFFFFFFFFFFFFFF, 0x7FFE)))[0], sign | rng.choice((0, 1))

    m80 = []  # the examined value at 0, ST(0) of the two-value exports at 16 and the selector at 48
    for v in signed_specials:
        for other in signed_specials[::7]:
            m80.append(m80_bytes(v).ljust(16, b"\0") + m80_bytes(other).ljust(32, b"\0")
                       + rng.getrandbits(32).to_bytes(4, "little"))
            m80.append(m80_bytes(other).ljust(16, b"\0") + m80_bytes(v).ljust(32, b"\0")
                       + rng.choice((0, 1)).to_bytes(4, "little"))
    for _ in range(1200):
        m80.append(m80_bytes(random_class()).ljust(16, b"\0") + m80_bytes(random_class()).ljust(32, b"\0")
                   + rng.choice((0, 1, rng.getrandbits(32))).to_bytes(4, "little"))
    blocks = {
        "m80": m80,
        "m64": [v.to_bytes(8, "little") for v in base.f64_bits() + [rng.getrandbits(64) for _ in range(400)]],
        "m32": [v.to_bytes(4, "little") for v in base.f32_bits() + [rng.getrandbits(32) for _ in range(400)]],
        "m64x8": [b"".join(rng.choice(base.f64_bits()).to_bytes(8, "little") for _ in range(8)) for _ in range(200)],
    }
    for name, (kind, _) in STEPS.items():
        for data in blocks[kind]:
            yield name, data.ljust(INPUT, b"\0")


# ---- Check -----------------------------------------------------------------------------------------

def check(harness_path, native_path, lifted_path, *control_paths):
    expected = [(name, data, *reference(name, data)) for name, data in cases()]
    for label, path in (("native", native_path), ("lifted", lifted_path)):
        run = base.Runner(harness_path, path)
        bad = 0
        for name, data, want, _ in expected:
            got, state = run(name, data)
            if not base.entry_state_ok(state):
                raise AssertionError(f"{label} {name} left state {state:#014x}; the profile requires control "
                                     "0x027f and an empty stack at TOP 0")
            if got != want:
                if bad < 3:
                    print(f"{label} mismatch: {name}(input {data[:56].hex()})\n  got      {got[:48].hex()}\n"
                          f"  expected {want[:48].hex()}")
                bad += 1
        if bad:
            raise AssertionError(f"{label}: {bad}/{len(expected)} mismatches")
    print(f"{len(expected)} cases over {len(SEQUENCES)} exports (output block and guards) matched the reference "
          "in native and translated DLLs; each export returned with control 0x027f and an empty stack")
    counts = {}
    for name, data, want, seen in expected:
        if seen is not None:
            counts[seen] = counts.get(seen, 0) + 1
    print("fxam classes: " + ", ".join(f"{label} {counts.get(label, 0)}" for label in CLASSES))
    print(f"processor: {platform.processor()}")
    first = {}
    for name, data, want, _ in expected:
        first.setdefault(name, (data, want))
    for label, path in (("native", native_path), ("lifted", lifted_path)):
        run = base.Runner(harness_path, path)
        failed = [name for name, (data, want) in first.items() if not base.preserved(run, "x87_call", name, data, want)]
        if failed:
            raise AssertionError(f"x87_call lost a live caller value or control word around {label} {failed}")
    print(f"x87_call kept a live caller ST(0) value and control word around all {len(first)} native and "
          "translated exports")
    for path in control_paths:
        control = Path(path).stem.rsplit("_", 1)[-1]
        run = base.Runner(harness_path, path)
        bad = sum(1 for name, data, want, _ in expected if run(name, data)[0] != want)
        target = CONTROL_EXPORTS[control]
        named = sum(1 for name, data, want, _ in expected if name == target and run(name, data)[0] != want)
        print(f"control {control}: {bad}/{len(expected)} mismatches, {named} in {target}")
        if not named:
            raise AssertionError(f"control {control} matched every case of {target}")
    check_rejections()


def check_rejections():
    from iced_x86 import Decoder
    from ffx_version_query import lower

    def attempt(hex_bytes):
        body = list(Decoder(64, bytes.fromhex(hex_bytes), ip=0x1000))
        try:
            lower(body, [], {}, {}, {}, x87=True)
        except ValueError as exc:
            return str(exc)
        return None

    fld, pop, clear = "db 29", "dd d8", "31 c0 c3"  # fld tbyte ptr [rcx]; fstp st(0); xor eax, eax; ret
    fxam, ftst, sw, sahf_jb, c1_jne = "d9 e5", "d9 e4", "df e0", "9e 72 00", "f6 c4 02 75 00"
    and47_cmp7 = "80 e4 47 80 fc 07 74 00"  # and ah, 47h; cmp ah, 07h; je
    rejected = {
        "FXAM on an empty stack": (attempt(f"{fxam} {sw} {sahf_jb} {clear}"), "x87 stack underflow"),
        "TEST AH, 02h after FTST and FNSTSW": (attempt(f"{fld} {ftst} {sw} {c1_jne} {pop} {clear}"),
                                               "reads ZF, which depends on a status-word bit"),
        "TEST AH, 02h after FXAM, FCOMI and FNSTSW": (
            attempt(f"{fld} {fld} {fxam} db f1 {sw} {c1_jne} {pop} {pop} {clear}"),
            "reads ZF, which depends on a status-word bit"),
        "AND AH, 47h and CMP after FXAM, FCHS and FNSTSW": (
            attempt(f"{fld} {fxam} d9 e0 {sw} {and47_cmp7} {pop} {clear}"),
            "cmp ah,7 at 0x100b reads AX bits 0x4700, which FNSTSW AX leaves unknown"),
        "MOV of AH after FXAM and FNSTSW": (attempt(f"{fld} {fxam} {sw} 88 22 {pop} {clear}"),
                                            "reads AX bits 0x8000, which FNSTSW AX leaves unknown"),
        "TEST AH, 02h on a join of an FXAM path and an FTST path": (
            attempt(f"{fld} 85 c0 74 04 {fxam} eb 02 {ftst} {sw} {c1_jne} {pop} {clear}"),
            "reads ZF, which depends on a status-word bit"),
    }
    for case, (message, needle) in rejected.items():
        if message is None or needle not in message:
            raise AssertionError(f"{case}: expected a rejection containing {needle!r}, got {message!r}")
    print(f"rejected {len(rejected)} sequences: " + "; ".join(rejected))
    accepted = {
        "FXAM, FNSTSW AX, TEST AH, 02h and JNE": attempt(f"{fld} {fxam} {sw} {c1_jne} {pop} {clear}"),
        "FXAM, FNSTSW AX, AND AH, 47h, CMP AH, 07h and JE": attempt(f"{fld} {fxam} {sw} {and47_cmp7} {pop} {clear}"),
        "FXAM, FCOMI, FNSTSW AX, SAHF and JB": attempt(f"{fld} {fld} {fxam} db f1 {sw} {sahf_jb} {pop} {pop} {clear}"),
    }
    for case, message in accepted.items():
        if message is not None:
            raise AssertionError(f"{case}: expected translation, got {message!r}")
    print(f"translated {len(accepted)} sequences: " + "; ".join(accepted))


if __name__ == "__main__":
    mode, first, second, *rest = sys.argv[1:]
    if mode == "asm":
        write_asm(first, second)
    elif mode == "emit":
        emit(Path(first), Path(second), *rest)
    elif mode == "check":
        check(first, second, *rest)
    else:
        raise ValueError(mode)
