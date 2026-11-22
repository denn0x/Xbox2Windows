"""Compare native and translated FTST, FICOM and FICOMP of the x87 leaf-function profile.

Usage:
    python tests/cpu/x87_ftst.py asm <asm> <def>             regenerate the MASM source and exports
    python tests/cpu/x87_ftst.py emit <dll> <cpp> [control]   translate (and optionally mutate) the fixture
    python tests/cpu/x87_ftst.py check <harness> <native> <lifted> [controls...]

The exports, harness and guards follow tests/cpu/x87_add.py, and the load and store steps use the format of
tests/cpu/x87_moves.py. FTST compares ST(0) with +0, and FICOM and FICOMP compare it with an m16 or m32
two's-complement integer; FICOMP then pops. After each FNSTSW AX the record of tests/cpu/x87_status.py
reads AH through every accepted consumer and clears EAX. emit checks that every decoded x87 instruction is
the form its sequence names. The model compares with Fraction arithmetic (tests/cpu/x87_compare.py),
converts the integer itself and is independent of tools/x87_soft.h.
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
INTEGER = ("ficom", "ficomp")  # (op, kind, offset): ST(0) with an m16 or m32 integer
POPS = {"ftst": 0, "ficom": 0, "ficomp": 1}
WIDTH = {"m16": 16, "m32": 32}
PTR = {"m16": "word", "m32": "dword"}
SW = [("fnstsw",), ("record", 0)]
SEQUENCES = {
    # FTST after loads of each format: the m64 and m32 loads bring denormals and the format limits.
    "ftst_m80": ("m80int", [("fld", "m80", 0), ("ftst",)] + SW + [("fstp", "m80", 24)]),
    "ftst_m64": ("m64", [("fld", "m64", 0), ("ftst",)] + SW + [("fstp_st", 0)]),
    "ftst_m32": ("m32", [("fld", "m32", 0), ("ftst",)] + SW + [("fstp_st", 0)]),
    # Depth 8, where TOP is 0 again.
    "ftst_depth8": ("m64x8", [("fld", "m64", 8 * k) for k in range(8)] + [("ftst",)] + SW + [("fstp_st", 0)] * 8),
    # Each integer form; the popping forms leave the value loaded first.
    **{f"{op}_{kind}": ("m80int", [("fld", "m80", 0)] + ([("fld", "m80", 16)] if POPS[op] else [])
                        + [(op, kind, 48)] + SW + [("fstp", "m80", 24)])
       for op in INTEGER for kind in ("m16", "m32")},
    # FILD and FICOM of the same integer are equal.
    "fild_ficom_m16": ("m80int", [("fild", "m16", 48), ("ficom", "m16", 48)] + SW + [("fstp", "m80", 24)]),
    "fild_ficomp_m32": ("m80int", [("fild", "m32", 48), ("ficomp", "m32", 48)] + SW),
    # The input dword at 52 selects FTST or FICOM m16; both paths define C0, C2 and C3.
    "joined": ("m80int", [("fld", "m80", 0), ("choose",)] + SW + [("fstp", "m80", 24)]),
}
STEPS = SEQUENCES

CONTROLS = {
    # FTST compares with +1 instead of +0.
    "ftstone": (re.compile(r"X87Value\{\}\)"), "x87_from_integer(1))"),
    # FTST compares +0 with ST(0).
    "ftstswap": (re.compile(r"x87_compare\((x87_slot\[\d\]), X87Value\{\}\)"), r"x87_compare(X87Value{}, \1)"),
    # FICOM m16 zero-extends the integer.
    "zeroext": (re.compile(r"(x87_compare\(x87_slot\[\d\], x87_from_integer\(static_cast<)int16_t(>\(load<uint16_t>)"),
                r"\1uint16_t\2"),
    # FICOM m32 reads only the low 16 bits.
    "narrow": (re.compile(r"(x87_compare\(x87_slot\[\d\], x87_from_integer\(static_cast<)int32_t>\(load<uint32_t>"),
               r"\1int16_t>(load<uint16_t>"),
    # FICOM and FICOMP compare the integer with ST(0).
    "intswap": (re.compile(r"x87_compare\((x87_slot\[\d\]), (x87_from_integer\(static_cast<int\d\d_t>\(load<uint\d\d_t>"
                           r"\(a\)\)\))\)"), r"x87_compare(\2, \1)"),
}


# ---- MASM generation and decoding ------------------------------------------------------------------

def asm_lines(step, name):
    op, *args = step
    if op == "ftst":
        return ["    ftst"]
    if op in INTEGER:
        return [f"    {op} {PTR[args[0]]} ptr [rcx+{args[1]}]"]
    if op == "fild" and args[0] in PTR:
        return [f"    fild {PTR[args[0]]} ptr [rcx+{args[1]}]"]
    if op == "choose":
        return ["    mov eax, dword ptr [rcx+52]", "    test eax, eax", f"    je {name}_alt", "    ftst",
                f"    jmp {name}_join", f"{name}_alt:", "    ficom word ptr [rcx+48]", f"{name}_join:"]
    if op in ("fnstsw", "record"):
        return status.asm_lines(step, name)
    return [moves.asm_line(step)]


def expected_forms(step):
    op, *args = step
    if op == "ftst":
        return [("ftst", "")]
    if op in INTEGER or op == "fild":
        return [(op, args[0])]
    if op == "choose":
        return [("ftst", ""), ("ficom", "m16")]
    if op == "fnstsw":
        return [("fnstsw", "ax")]
    if op == "record":
        return []
    return [moves.expected_form(step)]


def decoded_form(i):
    from iced_x86 import MemorySize as S, Mnemonic as M, OpKind as K

    if i.mnemonic in (M.FICOM, M.FICOMP, M.FILD) and i.op0_kind == K.MEMORY:
        name = next(n for n, v in vars(M).items() if v == i.mnemonic).lower()
        return name, {S.INT16: "m16", S.INT32: "m32", S.INT64: "m64"}[i.memory_size]
    return status.decoded_form(i)


def write_asm(asm_path, def_path):
    lines = ["; coverage: source cpu_aot_x87_ftst",
             "; Generated by tests/cpu/x87_ftst.py asm; edit SEQUENCES there and regenerate.",
             "; Source-owned differential fixture for FTST, FICOM and FICOMP in the x87 leaf-function profile.",
             "; Each export reads an input block at RCX, writes an output block at RDX and returns with the x87",
             "; stack empty.", ".code", ""]
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
    decoded = [decoded_form(i) for i in body if is_x87(i)]
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
    if not re.search(status.STATUS, text):
        raise AssertionError("no comparison into the status word was lowered")
    if control != "lifted":
        expression, replacement = CONTROLS[control]
        text, count = expression.subn(replacement, text)
        if not count:
            raise AssertionError(f"control {control} changed nothing")
        print(f"control {control}: {count} replacements")
    output.write_text(text, encoding="ascii")


# ---- Independent model -----------------------------------------------------------------------------

def from_integer(n):
    """The exact 80-bit encoding of an integer, as (significand, sign and exponent)."""
    if n == 0:
        return 0, 0
    m = abs(n)
    return m << (64 - m.bit_length()), (0x8000 if n < 0 else 0) | (0x3FFF + m.bit_length() - 1)


def signed(raw, bits):
    return raw - (1 << bits) if raw >> (bits - 1) else raw


def reference(name, data):
    """The output block, and the comparison result (0 less, 1 equal, 2 greater, 3 unordered) at FNSTSW."""
    slots, out = [], bytearray(b"\xcc" * OUTPUT)
    result = seen = ah = None

    def read(kind, offset):
        return int.from_bytes(data[offset:offset + SIZE.get(kind, WIDTH.get(kind, 0) // 8)], "little")

    def integer(kind, offset):
        return signed(read(kind, offset), WIDTH[kind])

    for op, *args in STEPS[name][1]:
        if op == "fld":
            raw = read(*args)
            slots.append((raw & (2 ** 64 - 1), raw >> 64) if args[0] == "m80" else from_binary(args[0], raw))
        elif op == "fild":
            slots.append(from_integer(integer(*args)))
        elif op == "fstp":
            kind, offset = args
            v = slots.pop()
            encoded = v[0] | v[1] << 64 if kind == "m80" else to_binary(kind, v)
            out[offset:offset + SIZE[kind]] = encoded.to_bytes(SIZE[kind], "little")
        elif op == "fstp_st":
            slots[-1 - args[0]] = slots[-1]
            slots.pop()
        elif op == "ftst":
            result = compare.compare(slots[-1], (0, 0))
        elif op in INTEGER:
            result = compare.compare(slots[-1], from_integer(integer(*args)))
            del slots[len(slots) - POPS[op]:]
        elif op == "choose":
            other = (0, 0) if read("m32", 52) else from_integer(integer("m16", 48))
            result = compare.compare(slots[-1], other)
        elif op == "fnstsw":
            ah = status.known_ah(result, (8 - len(slots)) & 7)
            seen, result = result, None
        elif op == "record":
            out[args[0]:args[0] + status.RECORD] = status.record_bytes(ah)
        else:
            raise AssertionError(op)
    assert not slots
    return bytes(out), seen


# ---- Inputs ----------------------------------------------------------------------------------------

def cases():
    rng = random.Random(67)
    specials = base.special_m80()
    limits = [0, 1, -1, 2, -2, 0x7FFF, -0x8000, 0x7FFFFFFF, -0x80000000, 0x8000, -0x8001, 0x10000]

    def nearby(n):
        """m80 values at and around the integer n: n itself, the neighbours in the lowest significand bit and
        n plus and minus a quarter."""
        if n == 0:
            return [(0, 0), (0, 0x8000), (1, 0), (J | 1, 0x3FFF - 64)]
        sig, se = from_integer(n)
        values = [(sig, se), (sig + 1, se) if sig + 1 < 2 ** 64 else (sig, se), (sig - 1, se) if sig - 1 >= J else (sig, se)]
        quarter = (abs(n) * 4 + (1 if n > 0 else -1)) * (1 if n > 0 else -1)  # n + 1/4 toward larger magnitude
        q_sig, q_se = from_integer(quarter)
        values.append((q_sig, (q_se & 0x8000) | ((q_se & 0x7FFF) - 2)))
        return values

    def block(value, other, n, selector=0):
        return (m80_bytes(value) + m80_bytes(other)).ljust(48, b"\0") + (n & 0xFFFFFFFF).to_bytes(4, "little") \
            + selector.to_bytes(4, "little")

    m80int = []
    for n in limits + [rng.randint(-0x8000, 0x7FFF) for _ in range(60)] + [rng.getrandbits(32) - 2 ** 31 for _ in range(60)]:
        for v in nearby(n):
            for m in (n, n + 1, n - 1, 0):
                m80int.append(block(v, rng.choice(specials), m, rng.choice((0, 1))))
    for v in specials:
        for n in limits:
            m80int.append(block(v, rng.choice(specials), n, rng.choice((0, 1))))
    for _ in range(100):  # +0 and -0 against random integers and 0
        m80int.append(block((0, rng.choice((0, 0x8000))), rng.choice(specials), rng.choice((0, rng.getrandbits(32))),
                            rng.getrandbits(32)))
    for _ in range(300):
        v = (J | rng.getrandbits(63), rng.randint(0x3FFF - 4, 0x3FFF + 33) | rng.choice((0, 0x8000)))
        m80int.append(block(v, rng.choice(specials), rng.getrandbits(32), rng.getrandbits(32)))
    blocks = {
        "m80int": m80int,
        "m64": [v.to_bytes(8, "little") for v in base.f64_bits() + [rng.getrandbits(64) for _ in range(300)]],
        "m32": [v.to_bytes(4, "little") for v in base.f32_bits() + [rng.getrandbits(32) for _ in range(300)]],
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
    for group, names in (("ftst", [n for n in SEQUENCES if n.startswith("ftst")]),
                         ("ficom and ficomp", [n for n in SEQUENCES if "ficom" in n])):
        counts = {}
        for name, data, want, r in expected:
            if name in names:
                counts[r] = counts.get(r, 0) + 1
        print(f"{group}: " + ", ".join(f"{label} {counts.get(r, 0)}"
                                       for r, label in ((0, "less"), (1, "equal"), (2, "greater"), (3, "unordered"))))
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
        print(f"control {control}: {bad}/{len(expected)} mismatches")
        if not bad:
            raise AssertionError(f"control {control} matched every case")
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
    ftst, ficom16, ficomp32, sw, sahf_jb = "d9 e4", "de 11", "da 19", "df e0", "9e 72 00"
    unknown = "which depends on a status-word bit that is not known on every path from the entry"
    rejected = {
        "FTST on an empty stack": (attempt(f"{ftst} {clear}"), "x87 stack underflow"),
        "FICOM m16 on an empty stack": (attempt(f"{ficom16} {clear}"), "x87 stack underflow"),
        "FICOMP m32, then a pop of the emptied stack": (attempt(f"{fld} {ficomp32} {pop} {clear}"),
                                                         "x87 stack underflow"),
        "FADD between FTST and FNSTSW": (attempt(f"{fld} {fld} {ftst} dc c1 {sw} {sahf_jb} {pop} {pop} {clear}"),
                                         "reads CF, " + unknown),
        "FLD between FICOM and FNSTSW": (attempt(f"{fld} {ficom16} {fld} {sw} {sahf_jb} {pop} {pop} {clear}"),
                                         "reads CF, " + unknown),
        "TEST AL, 1 after FTST and FNSTSW": (attempt(f"{fld} {ftst} {sw} a8 01 75 00 {pop} {clear}"),
                                             "reads AX bits 0x00ff, which FNSTSW AX leaves unknown"),
        "FXAM": (attempt(f"{fld} d9 e5 {sw} {sahf_jb} {pop} {clear}"), "unsupported x87 operation"),
    }
    for case, (message, needle) in rejected.items():
        if message is None or needle not in message:
            raise AssertionError(f"{case}: expected a rejection containing {needle!r}, got {message!r}")
    print(f"rejected {len(rejected)} sequences: " + "; ".join(rejected))
    accepted = {
        "FTST, FNSTSW AX, SAHF and JB": attempt(f"{fld} {ftst} {sw} {sahf_jb} {pop} {clear}"),
        "FICOMP m32, FNSTSW AX, TEST AH, 45h and JP": attempt(f"{fld} {ficomp32} {sw} f6 c4 45 7a 00 {clear}"),
        "FCOMI between FICOM and FNSTSW": attempt(f"{fld} {fld} {ficom16} db f1 {sw} {sahf_jb} {pop} {pop} {clear}"),
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
