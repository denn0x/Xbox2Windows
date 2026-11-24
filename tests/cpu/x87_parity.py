"""Compare native and translated PF of FCOMI, FCOMIP, FUCOMI and FUCOMIP, and FCMOVU and FCMOVNU, in the
x87 leaf-function profile.

Usage:
    python tests/cpu/x87_parity.py asm <asm> <def>             regenerate the MASM source and exports
    python tests/cpu/x87_parity.py emit <dll> <cpp> [control]   translate (and optionally mutate) the fixture
    python tests/cpu/x87_parity.py check <harness> <native> <lifted> [controls...]

The exports, harness and guards follow tests/cpu/x87_add.py, and the load and store steps use the format of
tests/cpu/x87_moves.py. A record step stores whether JP, JNP, JE and JB are taken, through flag-neutral MOVs.
The exports read the PF of each FCOMI form through JP and JNP, move on it with FCMOVU and FCMOVNU, run the
equality and classification branches that compilers emit after FUCOMIP, and check that the last writer of
PF wins: FUCOMI after SAHF, SAHF after FUCOMI, an 8-bit TEST after FUCOMI, and a join of a FUCOMI path and a
SAHF path. emit checks that every decoded x87 instruction is the form its sequence names. The model compares
with Fraction arithmetic (tests/cpu/x87_compare.py), copies raw encodings for the moves and is independent
of tools/x87_soft.h.
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
from x87_add import J, from_binary, m80_bytes, to_binary  # noqa: E402
from x87_div import SIZE  # noqa: E402

INPUT, OUTPUT = base.INPUT, base.OUTPUT
FLAG_COMPARES = ("fcomi", "fcomip", "fucomi", "fucomip")  # (op, i): ST(0) with ST(i), into ZF, PF and CF
MOVES = ("fcmovu", "fcmovnu")                             # (op, i): ST(0) = ST(i) when PF is set or clear
POPS = {"fcomi": 0, "fcomip": 1, "fucomi": 0, "fucomip": 1}
RECORD_JUMPS = ("jp", "jnp", "je", "jb")
CLASS = {"unordered": 2, "equal": 1, "other": 0}  # the byte that the classify step stores

P80 = [("fld", "m80", 0), ("fld", "m80", 16)]  # ST(0) = the value at 16, ST(1) = the value at 0
T80 = P80 + [("fld", "m80", 32)]               # ST(0) = 32, ST(1) = 16, ST(2) = 0
OUT2 = [("fstp", "m80", 24), ("fstp", "m80", 40)]
OUT3 = OUT2 + [("fstp_st", 0)]
SEQUENCES = {
    # Each form, read through JP, JNP, JE and JB.
    **{f"{op}_record": ("pair80", P80 + [(op, 1), ("record", 0)] + (OUT2 if not POPS[op] else OUT2[:1]))
       for op in FLAG_COMPARES},
    # Moves on the PF of a comparison, also after a popping form and with ST(7) at depth 8.
    "fucomi_fcmovu": ("pair80", P80 + [("fucomi", 1), ("fcmovu", 1)] + OUT2),
    "fcomi_fcmovnu": ("pair80", P80 + [("fcomi", 1), ("fcmovnu", 1)] + OUT2),
    "fucomip_fcmovnu": ("triple80", T80 + [("fucomip", 2), ("fcmovnu", 1)] + OUT2),
    "fcomip_fcmovu": ("triple80", T80 + [("fcomip", 1), ("fcmovu", 1)] + OUT2),
    "fucomi_st7": ("m64x8", [("fld", "m64", 8 * k) for k in range(8)] + [("fucomi", 7), ("fcmovu", 7), ("record", 0)]
                   + [("fstp", "m80", 24)] + [("fstp_st", 0)] * 7),
    # a == b as compilers emit it: FUCOMIP, FSTP ST(0), then JP and JNE to the false branch.
    "equal_branch": ("pair80", P80 + [("fucomip", 1), ("fstp_st", 0), ("equal", 0)]),
    # Unordered, equal or neither: JNP and then JE.
    "classify": ("pair80", P80 + [("fcomip", 1), ("fstp_st", 0), ("classify", 0)]),
    # The last writer of PF wins: FUCOMI after SAHF, SAHF after FUCOMI and TEST AL, AL after FUCOMI.
    "sahf_fucomi": ("triple80", T80 + [("fcom", 1), ("fnstsw",), ("sahf",), ("fucomi", 2), ("record", 0),
                                       ("mov_eax0",)] + OUT3),
    "fucomi_sahf": ("triple80", T80 + [("fucomi", 2), ("fcom", 1), ("fnstsw",), ("sahf",), ("fcmovu", 2),
                                       ("record", 0), ("mov_eax0",)] + OUT3),
    "fucomi_test": ("pairbyte", P80 + [("fucomi", 1), ("test_al", 48), ("fcmovu", 1), ("record", 0)] + OUT2),
    # FCMOVU and FCMOVNU on the PF that SAHF loads from every AH value.
    "byte_sahf": ("pairbyte", P80 + [("ah_byte", 48), ("sahf",), ("fcmovu", 1), ("record", 0)] + OUT2),
    # The input dword at 48 selects FUCOMI ST(1) or FCOM ST(2) with FNSTSW AX and SAHF; both write PF.
    "joined": ("tripleint", T80 + [("choose", 1, 2), ("fcmovu", 1), ("record", 0), ("mov_eax0",)] + OUT3),
}
STEPS = SEQUENCES

CONTROLS = {
    # The FCOMI family clears PF.
    "pfnever": (re.compile(r"parity = r == 3; "), "parity = false; "),
    # The FCOMI family sets PF for equal instead of unordered.
    "pfequal": (re.compile(r"parity = r == 3; "), "parity = r == 1; "),
    # The FCOMI family leaves PF as it was.
    "pfkeep": (re.compile(r"parity = r == 3; "), ""),
    # FCMOVU moves when PF is clear.
    "uinvert": (re.compile(r"if \(parity\) (x87_slot\[\d\] = x87_slot\[\d\];  // fcmovu )"), r"if (!parity) \1"),
    # FCMOVNU moves when PF is set.
    "nuinvert": (re.compile(r"if \(!parity\) (x87_slot\[\d\] = x87_slot\[\d\];  // fcmovnu )"), r"if (parity) \1"),
}
# The export in which each control must mismatch; pfkeep can mismatch in sahf_fucomi only where the SAHF and
# the FUCOMI after it leave different PFs.
CONTROL_EXPORTS = {"pfnever": "equal_branch", "pfequal": "classify", "pfkeep": "sahf_fucomi",
                   "uinvert": "fucomi_fcmovu", "nuinvert": "fcomi_fcmovnu"}


# ---- MASM generation and decoding ------------------------------------------------------------------

def asm_lines(step, name):
    op, *args = step
    if op in FLAG_COMPARES or op in MOVES:
        return [f"    {op} st, st({args[0]})"]
    if op == "fcom":
        return [f"    fcom st({args[0]})"]
    if op == "fnstsw":
        return ["    fnstsw ax"]
    if op == "sahf":
        return ["    sahf"]
    if op == "mov_eax0":
        return ["    mov eax, 0"]
    if op == "ah_byte":
        return [f"    mov ah, byte ptr [rcx+{args[0]}]"]
    if op == "test_al":
        return [f"    mov al, byte ptr [rcx+{args[0]}]", "    test al, al"]
    if op == "record":
        o = args[0]
        return [line for k, jump in enumerate(RECORD_JUMPS)
                for line in status.taken(f"{name}_{o}_{jump}", o + k, jump)]
    if op == "equal":  # 1 if equal, else 0
        o = args[0]
        return [f"    mov byte ptr [rdx+{o}], 0", f"    jp {name}_done", f"    jne {name}_done",
                f"    mov byte ptr [rdx+{o}], 1", f"{name}_done:"]
    if op == "classify":
        o = args[0]
        return [f"    jnp {name}_ordered", f"    mov byte ptr [rdx+{o}], {CLASS['unordered']}", f"    jmp {name}_done",
                f"{name}_ordered:", f"    mov byte ptr [rdx+{o}], {CLASS['equal']}", f"    je {name}_done",
                f"    mov byte ptr [rdx+{o}], {CLASS['other']}", f"{name}_done:"]
    if op == "choose":
        return ["    mov eax, dword ptr [rcx+48]", "    test eax, eax", f"    je {name}_alt",
                f"    fucomi st, st({args[0]})", f"    jmp {name}_join", f"{name}_alt:", f"    fcom st({args[1]})",
                "    fnstsw ax", "    sahf", f"{name}_join:"]
    return [moves.asm_line(step)]


def expected_forms(step):
    op, *args = step
    if op in FLAG_COMPARES or op in MOVES or op == "fcom":
        return [(op, f"st0,st{args[0]}")]
    if op == "fnstsw":
        return [("fnstsw", "ax")]
    if op == "choose":
        return [("fucomi", f"st0,st{args[0]}"), ("fcom", f"st0,st{args[1]}"), ("fnstsw", "ax")]
    if op in ("sahf", "mov_eax0", "ah_byte", "test_al", "record", "equal", "classify"):
        return []
    return [moves.expected_form(step)]


def write_asm(asm_path, def_path):
    lines = ["; coverage: source cpu_aot_x87_parity",
             "; Generated by tests/cpu/x87_parity.py asm; edit SEQUENCES there and regenerate.",
             "; Source-owned differential fixture for PF of the FCOMI family and FCMOVU and FCMOVNU in the x87",
             "; leaf-function profile. Each export reads an input block at RCX, writes an output block at RDX and",
             "; returns with the x87 stack empty.", ".code", ""]
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
    if text.count("parity = r == 3; ") != sum(1 for _, steps in SEQUENCES.values() for s in steps
                                              if s[0] in FLAG_COMPARES or s[0] == "choose"):
        raise AssertionError("not every FCOMI-family comparison sets PF")
    if control != "lifted":
        expression, replacement = CONTROLS[control]
        text, count = expression.subn(replacement, text)
        if not count:
            raise AssertionError(f"control {control} changed nothing")
        print(f"control {control}: {count} replacements")
    output.write_text(text, encoding="ascii")


# ---- Independent model -----------------------------------------------------------------------------

def operand(kind, raw):
    return (raw & (2 ** 64 - 1), raw >> 64) if kind == "m80" else from_binary(kind, raw)


def reference(name, data, moved=None):
    """The output block. moved, if given, collects the PF that each FCMOVU or FCMOVNU read."""
    slots, out = [], bytearray(b"\xcc" * OUTPUT)
    zf = cf = pf = None
    result = ah = None

    def read(kind, offset):
        return int.from_bytes(data[offset:offset + SIZE[kind]], "little")

    def flags_from(r):  # ZF, CF and PF of the FCOMI family
        return r in (1, 3), r in (0, 3), r == 3

    for op, *args in STEPS[name][1]:
        if op == "fld":
            slots.append(operand(args[0], read(*args)))
        elif op == "fstp":
            kind, offset = args
            v = slots.pop()
            encoded = v[0] | v[1] << 64 if kind == "m80" else to_binary(kind, v)
            out[offset:offset + SIZE[kind]] = encoded.to_bytes(SIZE[kind], "little")
        elif op == "fstp_st":
            slots[-1 - args[0]] = slots[-1]
            slots.pop()
        elif op in FLAG_COMPARES:
            zf, cf, pf = flags_from(compare.compare(slots[-1], slots[-1 - args[0]]))
            del slots[len(slots) - POPS[op]:]
        elif op == "fcom":
            result = compare.compare(slots[-1], slots[-1 - args[0]])
        elif op == "fnstsw":
            ah = status.known_ah(result, (8 - len(slots)) & 7)
        elif op == "ah_byte":
            ah = data[args[0]]
        elif op == "sahf":
            zf, cf, pf = bool(ah & 0x40), bool(ah & 0x01), bool(ah & 0x04)
        elif op == "test_al":
            b = data[args[0]]
            zf, cf, pf = b == 0, False, status.even(b)
        elif op == "mov_eax0":
            pass
        elif op == "choose":
            if read("m32", 48):
                zf, cf, pf = flags_from(compare.compare(slots[-1], slots[-1 - args[0]]))
            else:
                ah = status.known_ah(compare.compare(slots[-1], slots[-1 - args[1]]), (8 - len(slots)) & 7)
                zf, cf, pf = bool(ah & 0x40), bool(ah & 0x01), bool(ah & 0x04)
        elif op in MOVES:
            if moved is not None:
                moved.append(pf)
            if pf == (op == "fcmovu"):
                slots[-1] = slots[-1 - args[0]]
        elif op == "record":
            taken = {"jp": pf, "jnp": not pf, "je": zf, "jb": cf}
            out[args[0]:args[0] + len(RECORD_JUMPS)] = bytes(int(taken[j]) for j in RECORD_JUMPS)
        elif op == "equal":
            out[args[0]] = int(zf and not pf)
        elif op == "classify":
            out[args[0]] = CLASS["unordered"] if pf else CLASS["equal"] if zf else CLASS["other"]
        else:
            raise AssertionError(op)
        assert None not in (zf, cf, pf) or op not in MOVES + ("record", "equal", "classify")
    assert not slots
    return bytes(out)


# ---- Inputs ----------------------------------------------------------------------------------------

def cases():
    rng = random.Random(61)
    specials = base.special_m80()

    def random_m80():
        return J | rng.getrandbits(63), rng.randint(0x3FFF - 70, 0x3FFF + 70) | rng.choice((0, 0x8000))

    near = []
    for _ in range(100):
        v = random_m80()
        near += [(v, v), (v, (v[0] ^ 1, v[1])), (v, (v[0], v[1] ^ 0x8000))]
    pairs = [(a, b) for a in specials for b in specials] + near + [(random_m80(), random_m80()) for _ in range(200)]
    f64 = base.f64_bits()

    def triple():
        return b"".join(m80_bytes(v) for v in rng.sample(specials, 3))

    blocks = {
        "pair80": [m80_bytes(a) + m80_bytes(b) for a, b in pairs],
        "triple80": [triple() for _ in range(400)] + [b"".join(m80_bytes(v) for v in (a, b, a)) for a, b in near],
        "tripleint": [triple().ljust(48, b"\0") + rng.choice((0, 1, 0x80000000, rng.getrandbits(32))).to_bytes(4, "little")
                      for _ in range(400)],
        "pairbyte": [(m80_bytes(a) + m80_bytes(b)).ljust(48, b"\0") + bytes((x,))
                     for x in range(256) for a, b in (rng.choice(pairs), rng.choice(pairs))],
        "m64x8": [b"".join(rng.choice(f64).to_bytes(8, "little") for _ in range(8)) for _ in range(200)],
    }
    for name, (kind, _) in STEPS.items():
        for block in blocks[kind]:
            yield name, block.ljust(INPUT, b"\0")


# ---- Check -----------------------------------------------------------------------------------------

def check(harness_path, native_path, lifted_path, *control_paths):
    expected, read_pf = [], []
    for name, data in cases():
        expected.append((name, data, reference(name, data, read_pf)))
    for label, path in (("native", native_path), ("lifted", lifted_path)):
        run = base.Runner(harness_path, path)
        bad = 0
        for name, data, want in expected:
            got, state = run(name, data)
            if not base.entry_state_ok(state):
                raise AssertionError(f"{label} {name} left state {state:#014x}; the profile requires control "
                                     "0x027f and an empty stack at TOP 0")
            if got != want:
                if bad < 3:
                    print(f"{label} mismatch: {name}(input {data[:52].hex()})\n  got      {got[:52].hex()}\n"
                          f"  expected {want[:52].hex()}")
                bad += 1
        if bad:
            raise AssertionError(f"{label}: {bad}/{len(expected)} mismatches")
    print(f"{len(expected)} cases over {len(SEQUENCES)} exports (output block and guards) matched the reference "
          "in native and translated DLLs; each export returned with control 0x027f and an empty stack")
    unordered = sum(1 for name, data, want in expected if name in (f"{op}_record" for op in FLAG_COMPARES) and want[0])
    records = sum(1 for name, data, want in expected if name in (f"{op}_record" for op in FLAG_COMPARES))
    print(f"FCOMI-family records: JP taken in {unordered} of {records} cases; FCMOVU and FCMOVNU read PF set "
          f"in {sum(read_pf)} and clear in {len(read_pf) - sum(read_pf)} moves")
    print(f"processor: {platform.processor()}")
    first = {}
    for name, data, want in expected:
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
        missed = [name for name, data, want in expected if run(name, data)[0] != want]
        target = CONTROL_EXPORTS[control]
        print(f"control {control}: {len(missed)}/{len(expected)} mismatches, {missed.count(target)} in {target}")
        if not missed.count(target):
            raise AssertionError(f"control {control} matched every case of {target}")
    check_rejections()


def check_rejections():
    from iced_x86 import Decoder
    from ffx_version_query import lower

    def attempt(hex_bytes, calls=None):
        body = list(Decoder(64, bytes.fromhex(hex_bytes), ip=0x1000))
        try:
            lower(body, [], {}, calls or {}, {}, x87=True)
        except ValueError as exc:
            return str(exc)
        return None

    fld = "db 29"  # fld tbyte ptr [rcx]
    two, pop2, ret = f"{fld} {fld}", "dd d8 dd d8", "c3"  # fstp st(0) twice
    fucomi, fcmovu, fcmovnu, jp = "db e9", "da d9", "db d9", "7a 00"
    parity = "reads PF, which the integer lowering models only after SAHF, FCOMI, FCOMIP, FUCOMI, FUCOMIP"
    undefined = "reads PF, which no instruction defines on every path from the entry"
    unknown = "reads PF, which depends on a status-word bit that is not known on every path from the entry"
    rejected = {
        "FCMOVU before any flag writer": (attempt(f"{two} {fcmovu} {pop2} {ret}"), undefined),
        "FCMOVNU after TEST EAX, EAX": (attempt(f"{two} 85 c0 {fcmovnu} {pop2} {ret}"), parity),
        "FCMOVU after FUCOMI and INC EAX": (attempt(f"{two} {fucomi} ff c0 {fcmovu} {pop2} {ret}"), parity),
        "JP after FUCOMI and ADD EAX, 1": (attempt(f"{two} {fucomi} 83 c0 01 {jp} {pop2} {ret}"), parity),
        # FUCOMIP and FSTP ST(0) empty the stack before the call, which leaves PF undefined.
        "FCMOVU after FUCOMIP and a call": (attempt(f"{two} df e9 dd d8 e8 00 00 00 00 {two} {fcmovu} {pop2} {ret}",
                                                    calls={0x1008: ";"}), undefined),
        # JRCXZ selects FUCOMI or CMP; the join keeps PF modelled only if both paths model it.
        "a join of a FUCOMI path and a CMP path": (attempt(f"{two} e3 04 {fucomi} eb 02 39 c0 {fcmovu} {pop2} {ret}"),
                                                   parity),
        "SAHF on FNSTSW AX with no comparison, then FCMOVU": (
            attempt(f"{two} df e0 9e {fcmovu} {pop2} 31 c0 {ret}"), unknown),
        "FCMOVU ST(2) with two values": (attempt(f"{two} {fucomi} da da {pop2} {ret}"), "x87 stack underflow"),
        "FCMOVNU ST(1) with one value": (attempt(f"{fld} db e8 {fcmovnu} dd d8 {ret}"), "x87 stack underflow"),
        # SETP after FUCOMI is lowered (#1153); after an INC that writes PF last it is refused.
        "SETP after FUCOMI and INC": (attempt(f"{two} {fucomi} ff c0 0f 9a c0 {pop2} {ret}"), parity),
    }
    for case, (message, needle) in rejected.items():
        if message is None or needle not in message:
            raise AssertionError(f"{case}: expected a rejection containing {needle!r}, got {message!r}")
    print(f"rejected {len(rejected)} sequences: " + "; ".join(rejected))
    accepted = {
        "FCMOVU after FUCOMI": attempt(f"{two} {fucomi} {fcmovu} {pop2} {ret}"),
        "FCMOVNU after FUCOMI and MOV EAX, [RCX]": attempt(f"{two} {fucomi} 8b 01 {fcmovnu} {pop2} {ret}"),
        "JP after FUCOMIP and FSTP ST(0)": attempt(f"{two} df e9 dd d8 {jp} {ret}"),
        "a join of a FUCOMI path and a SAHF path": attempt(f"{two} e3 04 {fucomi} eb 03 b4 04 9e {fcmovu} {pop2} {ret}"),
        "FCMOVU after FUCOMI and LEA": attempt(f"{two} {fucomi} 8d 40 01 {fcmovu} {pop2} {ret}"),
    }
    for case, message in accepted.items():
        if message is not None:
            raise AssertionError(f"{case}: expected translation, got {message!r}")
    print(f"translated {len(accepted)} sequences whose PF the integer lowering models: " + "; ".join(accepted))


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
