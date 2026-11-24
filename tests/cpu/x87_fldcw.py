"""Compare native FLDCW code with the checked entry of the x87 leaf-function profile under every supported
precision and rounding control.

Usage:
    python tests/cpu/x87_fldcw.py asm <asm> <def>             regenerate the MASM source and exports
    python tests/cpu/x87_fldcw.py emit <dll> <cpp> [control]   translate (and optionally mutate) the fixture
    python tests/cpu/x87_fldcw.py check <harness> <native> <lifted> [controls...]

Each export loads control words from its input block (offsets 48, 50 and 52), runs rounding operations
and stores the results in its output block; cw_fninit and cw_finit also run FNINIT, which empties the
stack, clears C0 to C3 and loads 0x037F. The native exports run through x87_call of
tests/cpu/x87_harness.asm, which reports the control word each one leaves. The translation is
lower(..., x87=Checked(REJECT)) inside a checked entry, uint32_t name(word input, word output, word* result),
which returns the profile-rejection status and writes *result only for status 0. It touches no x87
state, so the check calls it directly.

The model rounds with exact rational arithmetic under the word's precision and rounding control. For
special operands it calls the fixed-word models of the earlier x87 tests, whose results do not depend on
the control word, and the check compares it with those models at 0x027F. It shares no code with
tools/x87_soft.h. Native runs never load a word with an exception unmasked, which would trap: such
words run only through the translation. A word with the reserved precision control 01 runs natively as
an observation and is not compared.
"""
from fractions import Fraction
import ctypes
from math import isqrt
from pathlib import Path
import platform
import random
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import x87_add as base  # noqa: E402  (tests/cpu is the script directory)
import x87_constants as constants  # noqa: E402
import x87_div as fixed_div  # noqa: E402
import x87_integers as fixed_integers  # noqa: E402
import x87_mul as fixed_mul  # noqa: E402
import x87_rndint as fixed_rndint  # noqa: E402
import x87_scale as fixed_scale  # noqa: E402
import x87_sqrt as fixed_sqrt  # noqa: E402
import x87_sub as fixed_sub  # noqa: E402
from x87_add import J, classify, floor_log2, from_binary, m80, m80_bytes, m80_exact  # noqa: E402

INPUT, OUTPUT = base.INPUT, base.OUTPUT
CW1, RESTORE, CW2, FLAG = 48, 50, 52, 56  # input offsets of the loaded words and the branch byte
STATUS = {"ok": 0, "unmasked": 1, "precision": 2, "unrestored": 3}  # x87_lower.STATUS, and 0 for success
PRECISION = {0: 24, 2: 53, 3: 64}
SENTINEL = 0x5A5A5A5A5A5A5A5A  # *result before the call; the checked entry keeps it on a rejection
REJECT = "{{ x87_status = {status}; return 0; }}"
PTR = {"m32": "dword", "m64": "qword", "m80": "tbyte", "i16": "word", "i32": "dword", "i64": "qword"}
SIZE = {"m32": 4, "m64": 8, "m80": 10, "i16": 2, "i32": 4, "i64": 8}
CONSTS = ("fldpi", "fldl2t", "fldl2e", "fldlg2", "fldln2", "fld1")
POP_ARITH = {"faddp": "add", "fsubp": "sub", "fmulp": "mul", "fdivp": "div"}

ENTER, LEAVE = ("fldcw", "rcx", CW1), ("fldcw", "rcx", RESTORE)
LOAD2 = [("fld", "m80", 0), ("fld", "m80", 16)]  # ST(1) = a (offset 0), ST(0) = b (offset 16)
# name: (input kind, steps). Every export ends with RET; ("ret",) also returns early.
SEQUENCES = {
    # ST(1) = a op b through the popping forms, stored as m80.
    **{f"cw_{name}": ("pair", [ENTER, *LOAD2, (op,), ("fstp", "m80", 0), LEAVE]) for op, name in POP_ARITH.items()},
    "cw_sqrt": ("single", [ENTER, ("fld", "m80", 0), ("fsqrt",), ("fstp", "m80", 0), LEAVE]),
    # Memory operands: fadd m64 and fdivr m32 (ST(0) = m32 / ST(0)).
    "cw_memory": ("memory", [ENTER, ("fld", "m80", 0), ("fadd", "m64", 32), ("fstp", "m80", 0),
                             ("fld", "m80", 0), ("fdivr", "m32", 40), ("fstp", "m80", 16), LEAVE]),
    # Integer operands: fimul m32 and fidivr m16.
    "cw_iarith": ("iarith", [ENTER, ("fld", "m80", 0), ("fimul", "i32", 32), ("fstp", "m80", 0),
                             ("fld", "m80", 0), ("fidivr", "i16", 36), ("fstp", "m80", 16), LEAVE]),
    # Rounding sites outside the precision control: narrowing and integer stores (FISTTP always chops).
    "cw_stores": ("single", [ENTER, ("fld", "m80", 0), ("fst", "m32", 0), ("fst", "m64", 8), ("fist", "i32", 16),
                             ("fist", "i16", 20), ("fld_st", 0), ("fistp", "i64", 24), ("fisttp", "i64", 32), LEAVE]),
    "cw_rndint": ("single", [ENTER, ("fld", "m80", 0), ("frndint",), ("fstp", "m80", 0), LEAVE]),
    # ST(0) = a scaled by ST(1) = b; both stored.
    "cw_scale": ("pair", [ENTER, ("fld", "m80", 16), ("fld", "m80", 0), ("fscale",), ("fstp", "m80", 0),
                          ("fstp", "m80", 16), LEAVE]),
    "cw_constants": ("none", [ENTER, *[s for k, name in enumerate(CONSTS) for s in ((name,), ("fstp", "m80", 10 * k))],
                              ("fldz",), ("fstp", "m32", 60), LEAVE]),
    # The control word as stored under the loaded word (FNSTCW and FSTCW) and after the restore.
    "cw_fnstcw": ("pair", [ENTER, ("fnstcw", "rdx", 0), ("fstcw", "rdx", 2), *LOAD2, ("fdivp",), ("fstp", "m80", 16),
                           LEAVE, ("fnstcw", "rdx", 4)]),
    # The MSVC truncation idiom: save the word, set RC to toward zero, FISTP and restore the saved word.
    "cw_msvc": ("single", [ENTER, ("fld", "m80", 0), ("fnstcw", "rsp", 8), ("movzx_eax", 8), ("or_eax_c00",),
                           ("mov_ax", 16), ("fldcw", "rsp", 16), ("fistp", "i32", 0), ("fldcw", "rsp", 8),
                           ("fld", "m80", 0), ("fistp", "i32", 4), LEAVE]),
    # Two returns that restore from different slots: the home-space slot saved on entry (bit 0 of the byte
    # at 56 set) or the input word at 50 (clear).
    "cw_branch": ("pair", [("fnstcw", "rsp", 8), ENTER, *LOAD2, ("fdivp",), ("fstp", "m80", 0), ("test_flag",),
                           ("je", "input"), ("fldcw", "rsp", 8), ("ret",), ("label", "input"), LEAVE]),
    # Guest stores before any FLDCW, between the two loads and after the second.
    "cw_writes": ("single", [("fld", "m80", 0), ("fst", "m32", 0), ("mov_byte", 12, 17), ENTER, ("fst", "m32", 4),
                             ("mov_byte", 13, 34), ("fldcw", "rcx", CW2), ("fst", "m32", 8), ("mov_byte", 14, 51),
                             ("fstp", "m64", 16), LEAVE]),
    # FNINIT under a loaded word, after FCOM has set C0, C2 and C3: it empties the two-value stack, clears C0
    # to C3 and sets the word to 0x037F (64-bit precision), under which a quotient is rounded and stored; the
    # restore then returns 0x027F.
    "cw_fninit": ("pair", [ENTER, *LOAD2, ("fcom",), ("fninit",), ("sw_c", 32), ("fnstcw", "rdx", 34), *LOAD2, ("fdivp",),
                           ("fstp", "m80", 0), LEAVE]),
    # FNSTSW m16 right after FNINIT, the only place where every status-word bit is known: the word is stored at
    # an odd offset, after FCOM has set C0, C2 or C3 (FNINIT clears them) and a flag-neutral store, with a byte
    # stored next to the word's end, and the quotient that follows is rounded under 0x037F.
    "cw_fnstsw": ("pair", [ENTER, *LOAD2, ("fcom",), ("fninit",), ("mov_byte", 36, 0x77), ("fnstsw_m", 33), *LOAD2,
                           ("fdivp",), ("fstp", "m80", 0), LEAVE]),
    # The same at depth 8, after FINIT (WAIT and FNINIT) and a flag-neutral store, where TOP was 0 before and
    # is 0 after; the word is stored at an even offset.
    "cw_finit_fnstsw": ("single", [ENTER, ("fld", "m80", 0), *[("fld_st", 0)] * 7, ("finit",), ("mov_byte", 18, 0x66),
                                   ("fnstsw_m", 16), LEAVE]),
    # FINIT (WAIT and FNINIT) at depth 8, a square root under 0x037F, and a return without a restore.
    "cw_finit": ("single", [("fld", "m80", 0), *[("fld_st", 0)] * 7, ("finit",), ("fld", "m80", 0), ("fsqrt",),
                            ("fstp", "m80", 0), ("fnstcw", "rdx", 16)]),
}


# ---- MASM generation and decoding ------------------------------------------------------------------

def asm_line(step):
    op, *args = step
    if op in ("fldcw", "fnstcw", "fstcw"):
        return f"    {op} word ptr [{args[0]}+{args[1]}]"
    if op in ("fld", "fadd", "fdivr", "fimul", "fidivr"):
        return f"    {op} {PTR[args[0]]} ptr [rcx+{args[1]}]"
    if op in ("fst", "fstp", "fist", "fistp", "fisttp"):
        return f"    {op} {PTR[args[0]]} ptr [rdx+{args[1]}]"
    if op in POP_ARITH:
        return f"    {op} st(1), st"
    if op == "fld_st":
        return f"    fld st({args[0]})"
    if op == "fcom":
        return "    fcom st(1)"
    if op in ("fsqrt", "frndint", "fscale", "fldz", "fninit", "finit", *CONSTS):
        return f"    {op}"
    if op == "fnstsw_m":
        return f"    fnstsw word ptr [rdx+{args[0]}]"
    if op == "sw_c":  # C0 to C3 of the status word through AH, then a known RAX
        return f"    fnstsw ax\n    and ah, 47h\n    mov byte ptr [rdx+{args[0]}], ah\n    mov eax, 0"
    return {"movzx_eax": lambda: f"    movzx eax, word ptr [rsp+{args[0]}]",
            "or_eax_c00": lambda: "    or eax, 0C00h",
            "mov_ax": lambda: f"    mov word ptr [rsp+{args[0]}], ax",
            "mov_byte": lambda: f"    mov byte ptr [rdx+{args[0]}], {args[1]}",
            "test_flag": lambda: f"    test byte ptr [rcx+{FLAG}], 1",
            "je": lambda: f"    je L_{args[0]}",
            "label": lambda: f"L_{args[0]}:",
            "ret": lambda: "    ret"}[op]()


def expected_forms(step):
    """The decoded x87 instructions of a step as (mnemonic, memory operand bytes): FSTCW is WAIT, FNSTCW."""
    op, *args = step
    if op == "fstcw":
        return [("wait", 0), ("fnstcw", 2)]
    if op == "finit":
        return [("wait", 0), ("fninit", 0)]
    if op in ("sw_c", "fnstsw_m"):
        return [("fnstsw", 0 if op == "sw_c" else 2)]
    if op in ("fldcw", "fnstcw"):
        return [(op, 2)]
    if op in ("fld", "fadd", "fdivr", "fimul", "fidivr", "fst", "fstp", "fist", "fistp", "fisttp"):
        return [(op, SIZE[args[0]])]
    if op == "fld_st":
        return [("fld", 0)]
    if op == "fcom":
        return [("fcom", 0)]
    if op in POP_ARITH or op in ("fsqrt", "frndint", "fscale", "fldz", "fninit", *CONSTS):
        return [(op, 0)]
    return []


def decoded_form(i):
    from iced_x86 import MemorySizeExt, Mnemonic as M, OpKind as K

    name = next(n for n, v in vars(M).items() if v == i.mnemonic).lower()
    memory = any(i.op_kind(k) == K.MEMORY for k in range(i.op_count))
    return name, MemorySizeExt.size(i.memory_size) if memory else 0


def write_asm(asm_path, def_path):
    lines = ["; coverage: source cpu_aot_x87_fldcw",
             "; Generated by tests/cpu/x87_fldcw.py asm; edit SEQUENCES there and regenerate.",
             "; Source-owned differential fixture for FLDCW and the checked entry of the x87 leaf-function",
             "; profile. Each export reads an input block at RCX, writes an output block at RDX and returns with",
             "; the x87 stack empty.", ".code", ""]
    for name, (_, steps) in SEQUENCES.items():
        lines += [f"{name} PROC FRAME", "    .endprolog", *(asm_line(s) for s in steps), "    ret", f"{name} ENDP", ""]
    lines.append("END")
    Path(asm_path).write_text("\n".join(lines) + "\n", encoding="ascii")
    Path(def_path).write_text("EXPORTS\n" + "".join(f"    {name}\n" for name in SEQUENCES), encoding="ascii")


# ---- Translation -----------------------------------------------------------------------------------

def checked_entry(name, code):
    return (f'extern "C" __declspec(dllexport) uint32_t {name}(word a, word b, word* result) {{\n'
            "    uint32_t x87_status = 0;\n"
            f"    const word value = [&]() -> word {{\n{code}\n    }}();\n"
            "    if (x87_status == 0) *result = value;\n"
            "    return x87_status;\n}\n")


def _mutate(pattern, replacement, only=None):
    """A control: replace pattern everywhere, or only in the checked entry `only` (its last match)."""
    def apply(text):
        if only is None:
            text, count = re.subn(pattern, replacement, text)
        else:
            start = text.index(f"uint32_t {only}(")
            end = text.index('extern "C"', start) if 'extern "C"' in text[start:] else len(text)
            matches = list(re.finditer(pattern, text[start:end]))
            count = 1 if matches else 0
            if matches:
                m = matches[-1]
                text = text[:start + m.start()] + m.expand(replacement) + text[start + m.end():]
        if not count:
            raise AssertionError(f"control pattern {pattern!r} changed nothing")
        return text, count
    return apply


RETURN_CHECK = r"if \(x87_cw != 0x027f\) \{ x87_status = 3; return 0; \} return rax;"
CONTROLS = {
    # The rounding control is ignored: every rounding site rounds to nearest-even.
    "rcnearest": _mutate(r"return \(cw >> 10\) & 3u;", "return 0u;"),
    # The precision control is ignored: arithmetic rounds to 53 bits.
    "pc53": _mutate(r"return pc == 0 \? x87_arith24 : pc == 3 \? x87_arith64 : x87_arith;", "return x87_arith;"),
    # FIST and FISTP ignore the rounding control.
    "fistnearest": _mutate(r"const unsigned rc = chop \? 3u : x87_rounding\(cw\);", "const unsigned rc = chop ? 3u : 0u;"),
    # The constant loads ignore the rounding control.
    "constnearest": _mutate(r"X87Value\{x87_rounding\(x87_cw\) == 2 \? 0x[0-9a-f]+ull : x87_rounding\(x87_cw\) == 0 \? "
                            r"(0x[0-9a-f]+ull) : 0x[0-9a-f]+ull,", r"X87Value{\1,"),
    # FLDCW keeps the raw word, so FNSTCW returns it and the return check compares it.
    "rawword": _mutate(r"x87_cw = static_cast<uint16_t>\(\(w & 0x1f3f\) \| 0x40\);", "x87_cw = w;"),
    # FLDCW accepts a word with an exception unmasked.
    "nounmasked": _mutate(r"if \(\(w & 0x3f\) != 0x3f\) \{ x87_status = 1; return 0; \} ", ""),
    # FLDCW accepts precision control 01.
    "pc01": _mutate(r"if \(\(w & 0x300\) == 0x100\) \{ x87_status = 2; return 0; \} ", ""),
    # No return checks the control word.
    "noreturn": _mutate(RETURN_CHECK, "return rax;"),
    # Of the two returns of cw_branch, only the one that restores from the saved slot checks.
    "onereturn": _mutate(RETURN_CHECK, "return rax;", only="cw_branch"),
    # FNINIT leaves the control word as it was.
    "fninitword": _mutate(r"x87_cw = 0x037f; (x87_sw = 0;  // fninit)", r"\1"),
    # FNINIT leaves C0 to C3 as FCOM set them.
    "fninitsw": _mutate(r"(x87_cw = 0x037f;) x87_sw = 0;(  // fninit)", r"\1\2"),
    # FNSTSW m16 stores a TOP of 7 instead of the depth-derived 0.
    "swtop": _mutate(r"x87_sw \| 0x0000\)\); \}  // fnstsw m16", "x87_sw | 0x3800)); }  // fnstsw m16"),
    # FNSTSW m16 stores a byte, which leaves the second byte of the word untouched.
    "swbyte": _mutate(r"store<uint16_t>\(a, static_cast<uint16_t>\(x87_sw \|", "store<uint8_t>(a, static_cast<uint8_t>(x87_sw |"),
    # FNSTSW m16 stores a dword, which clobbers the two bytes after the word.
    "swwide": _mutate(r"store<uint16_t>\(a, static_cast<uint16_t>\(x87_sw \|", "store<uint32_t>(a, static_cast<uint32_t>(x87_sw |"),
    # FNSTSW m16 stores at offset 0 instead of its operand.
    "swaddr": _mutate(r"= rdx \+ \d+ull; store<uint16_t>\(a, static_cast<uint16_t>\(x87_sw", "= rdx + 0ull; store<uint16_t>(a, static_cast<uint16_t>(x87_sw"),
    # FNSTSW m16 stores nothing.
    "swnone": _mutate(r"store<uint16_t>\(a, static_cast<uint16_t>\(x87_sw \| 0x0000\)\);", ";"),
    # A rejected FLDCW records its status but runs on, so the guest stores after it happen too.
    "deferred": _mutate(r"\{ x87_status = ([12]); return 0; \}", r"{ x87_status = \1; }"),
}


def emit(image, output, control="lifted"):
    import pefile
    from cpu_leaf_aot import export_rva
    from cpu_ranges import function_ranges
    from ffx_version_query import INTEGER_HEADER, lower, region
    from x87_lower import X87_HEADER, Checked, is_x87

    parts = [INTEGER_HEADER, X87_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name, (_, steps) in SEQUENCES.items():
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            body = region(pe, *bounds)
            decoded = [decoded_form(i) for i in body if is_x87(i)]
            wanted = [form for s in steps for form in expected_forms(s)]
            if decoded != wanted:
                raise AssertionError(f"{name} assembled as {decoded}, expected {wanted}")
            code = lower(body, ["rcx = a; rdx = b;"], {}, {}, {}, x87=Checked(REJECT))
            parts.append(checked_entry(name, code))
    text = "".join(parts)
    if control != "lifted":
        text, count = CONTROLS[control](text)
        print(f"control {control}: {count} replacements")
    output.write_text(text, encoding="ascii")


# ---- Independent model -----------------------------------------------------------------------------
# Values are raw 80-bit pairs (sig, se). rc is the rounding control: 0 to nearest-even, 1 toward -inf,
# 2 toward +inf, 3 toward zero.

def round_bits(sign, q, precision, bias, emax, rc):
    """|value| q > 0 rounded to precision bits under rc: (m, biased) with m < 2^precision and biased 0 for
    a denormal (denormalized at the same bit positions, m possibly 0), or None for infinity. A masked
    overflow gives infinity when rounding toward it, and the largest finite value otherwise."""
    e = floor_log2(q)
    quantum = max(e, 1 - bias) - (precision - 1)
    x = q / Fraction(2) ** quantum
    n = x.numerator // x.denominator
    rest = x - n
    if rc == 0:
        up = rest > Fraction(1, 2) or (rest == Fraction(1, 2) and n & 1)
    elif rc == 1:
        up = bool(sign) and rest != 0
    elif rc == 2:
        up = not sign and rest != 0
    else:
        up = False
    m = n + up
    if m == 1 << precision:
        m >>= 1
        quantum += 1
    biased = 0 if m < 1 << (precision - 1) else quantum + precision - 1 + bias
    if biased > emax:
        if rc == 0 or (rc == 1 and sign) or (rc == 2 and not sign):
            return None
        return (1 << precision) - 1, emax
    return m, biased


def round_m80(sign, q, precision, rc):
    """The m80 encoding of sign * q rounded to precision bits in the extended exponent range."""
    r = round_bits(sign, q, precision, 16383, 0x7FFE, rc)
    if r is None:
        return J, sign << 15 | 0x7FFF
    m, biased = r
    return m << (64 - precision), sign << 15 | biased


def controls_of(cw):
    """(precision bits, rounding control) of a control word; PC 01 gives 64 bits, as measured natively."""
    return PRECISION.get((cw >> 8) & 3, 64), (cw >> 10) & 3


def arith(name, a, b, cw):
    """FADD, FSUB, FMUL and FDIV of a and b under cw."""
    precision, rc = controls_of(cw)
    ca, cb = classify(a), classify(b)
    fixed = {"add": base.add, "sub": fixed_sub.sub, "mul": fixed_mul.mul, "div": fixed_div.div}[name]
    kinds = {ca[0], cb[0]}
    if kinds & {"unsupported", "nan", "inf"} or (name in ("mul", "div") and "zero" in kinds):
        return fixed(a, b)  # independent of the control word
    if name in ("add", "sub"):
        sb = cb[1] ^ (name == "sub")
        if ca[0] == cb[0] == "zero":
            return (0, ca[1] << 15) if ca[1] == sb else (0, 0x8000 if rc == 1 else 0)
        total = sum(-c[2] if s else c[2] for c, s in ((ca, ca[1]), (cb, sb)) if c[0] == "finite")
        if total == 0:
            return 0, 0x8000 if rc == 1 else 0
        return round_m80(int(total < 0), abs(total), precision, rc)
    q = ca[2] * cb[2] if name == "mul" else ca[2] / cb[2]
    return round_m80(ca[1] ^ cb[1], q, precision, rc)


def sqrt(v, cw):
    precision, rc = controls_of(cw)
    c = classify(v)
    if c[0] != "finite" or c[1]:
        return fixed_sqrt.sqrt(v)
    q = c[2]
    k = 160 - floor_log2(q) // 2  # an integer root of about 160 bits; see tests/cpu/x87_sqrt.py
    scaled = q * Fraction(4) ** k
    n = scaled.numerator // scaled.denominator
    root = isqrt(n)
    exact = root * root == n and scaled.denominator == 1
    unit = Fraction(2) ** -k
    return round_m80(0, root * unit if exact else (2 * root + 1) * unit / 2, precision, rc)


def to_integral(sign, q, rc):
    """The magnitude of sign * q rounded to an integer under rc."""
    n = q.numerator // q.denominator
    rest = q - n
    if rc == 0:
        return n + (rest > Fraction(1, 2) or (rest == Fraction(1, 2) and n & 1))
    if rc == 3:
        return n
    return n + (rest != 0 and bool(sign) == (rc == 1))


def rndint(v, cw):
    c = classify(v)
    if c[0] != "finite":
        return fixed_rndint.rndint(v)
    if c[2] >= 2 ** 63:
        return v  # already integral, kept as encoded
    n = to_integral(c[1], c[2], controls_of(cw)[1])
    return (0, c[1] << 15) if n == 0 else m80_exact(c[1], Fraction(n))


def fscale(a, b, cw):
    ca, cb = classify(a), classify(b)
    if ca[0] != "finite" or cb[0] not in ("finite", "zero"):
        return fixed_scale.scale(a, b)
    n = 0 if cb[0] == "zero" else min(int(cb[2]), fixed_scale.LIMIT)  # truncated toward zero
    return round_m80(ca[1], ca[2] * Fraction(2) ** (-n if cb[1] else n), 64, controls_of(cw)[1])


def to_integer(v, bits, rc):
    """FIST/FISTP under rc, or FISTTP with rc 3."""
    c = classify(v)
    if c[0] != "finite":
        return fixed_integers.to_integer(v, bits, False)
    indefinite = 1 << (bits - 1)
    n = to_integral(c[1], c[2], rc)
    if n > indefinite - (0 if c[1] else 1):
        return indefinite
    return (-n if c[1] else n) & ((1 << bits) - 1)


def to_binary(kind, v, rc):
    """FST m32/m64 under rc: rounded into the destination's precision and range."""
    c = classify(v)
    if c[0] != "finite":
        return base.to_binary(kind, v)
    precision, bias, exp_bits = base.BINARY[kind]
    fb, all_ones = precision - 1, (1 << exp_bits) - 1
    top = c[1] << (fb + exp_bits)
    r = round_bits(c[1], c[2], precision, bias, all_ones - 1, rc)
    if r is None:
        return top | all_ones << fb
    m, biased = r
    return top | biased << fb | (m - (1 << fb) if biased else m)


def constant(name, rc):
    """A constant load under rc, from the exact series of tests/cpu/x87_constants.py."""
    if name == "fldz":
        return 0, 0
    x = constants.EXACT[name]
    if rc == 0:
        return constants.round64(x)
    sig, se = constants.round64(x, "chop")  # every constant is positive
    if rc == 2 and x & ((1 << (x.bit_length() - 64)) - 1):
        sig += 1
        assert sig >> 64 == 0
    return sig, se


class Outcome:
    def __init__(self, status, out, cw=None, rax=None):
        self.status, self.out, self.cw, self.rax = status, bytes(out), cw, rax


def run(name, data, pc01=False):
    """The model of an export. With pc01 set, a word with precision control 01 is followed as 64 bits, as
    the native observation; otherwise it is rejected, as by the checked entry."""
    steps = SEQUENCES[name][1] + [("ret",)]
    labels = {s[1]: k for k, s in enumerate(steps) if s[0] == "label"}
    cw, slots, out, frame, rax, zero = 0x027F, [], bytearray(b"\xcc" * OUTPUT), {}, 0, False

    def read(kind, offset):
        return int.from_bytes(data[offset:offset + SIZE[kind]], "little")

    def write(offset, value, size):
        out[offset:offset + size] = value.to_bytes(size, "little")

    k = 0
    while True:
        op, *args = steps[k]
        k += 1
        rc = controls_of(cw)[1]
        if op == "fldcw":
            w = read("i16", args[1]) if args[0] == "rcx" else frame[args[1]]
            if w & 0x3F != 0x3F:
                return Outcome(STATUS["unmasked"], out)
            if (w >> 8) & 3 == 1 and not pc01:
                return Outcome(STATUS["precision"], out)
            cw = (w & 0x1F3F) | 0x40  # as FNSTCW reads it back
        elif op in ("fninit", "finit"):
            cw, slots = 0x037F, []
        elif op == "sw_c":  # only after FNINIT, which clears C0 to C3
            write(args[0], 0, 1)
            rax = 0
        elif op == "fnstsw_m":  # the SDM: FNINIT clears the whole status word, and TOP is 8 minus the depth
            write(args[0], (8 - len(slots)) % 8 << 11, 2)
        elif op in ("fnstcw", "fstcw"):
            if args[0] == "rdx":
                write(args[1], cw, 2)
            else:
                frame[args[1]] = cw
        elif op == "ret":
            return Outcome(STATUS["unrestored"] if cw != 0x027F else STATUS["ok"], out, cw, rax)
        elif op in ("label", "fcom"):  # C0, C2 and C3 of FCOM are only read after FNINIT has cleared them
            pass
        elif op == "test_flag":
            zero = data[FLAG] & 1 == 0
        elif op == "je":
            if zero:
                k = labels[args[0]]
        elif op == "movzx_eax":
            rax = frame[args[0]]
        elif op == "or_eax_c00":
            rax |= 0xC00
        elif op == "mov_ax":
            frame[args[0]] = rax & 0xFFFF
        elif op == "mov_byte":
            write(args[0], args[1], 1)
        elif op == "fld":
            raw = read(*args)
            slots.append((raw & (2 ** 64 - 1), raw >> 64) if args[0] == "m80" else from_binary(args[0], raw))
        elif op == "fld_st":
            slots.append(slots[-1 - args[0]])
        elif op in CONSTS or op == "fldz":
            slots.append(constant(op, rc))
        elif op in ("fst", "fstp"):
            kind, offset = args
            v = slots.pop() if op == "fstp" else slots[-1]
            write(offset, v[0] | v[1] << 64 if kind == "m80" else to_binary(kind, v, rc), SIZE[kind])
        elif op in ("fist", "fistp", "fisttp"):
            kind, offset = args
            v = slots[-1] if op == "fist" else slots.pop()
            write(offset, to_integer(v, 8 * SIZE[kind], 3 if op == "fisttp" else rc), SIZE[kind])
        elif op in POP_ARITH:
            slots[-2] = arith(POP_ARITH[op], slots[-2], slots[-1], cw)
            slots.pop()
        elif op == "fadd":
            slots[-1] = arith("add", slots[-1], fixed_mul.operand(args[0], read(*args)), cw)
        elif op == "fdivr":
            slots[-1] = arith("div", fixed_mul.operand(args[0], read(*args)), slots[-1], cw)
        elif op in ("fimul", "fidivr"):
            bits = 8 * SIZE[args[0]]
            n = read(*args)
            n -= (n >> (bits - 1)) << bits
            value = fixed_integers.from_integer(n)
            slots[-1] = (arith("mul", slots[-1], value, cw) if op == "fimul" else arith("div", value, slots[-1], cw))
        elif op == "fsqrt":
            slots[-1] = sqrt(slots[-1], cw)
        elif op == "frndint":
            slots[-1] = rndint(slots[-1], cw)
        elif op == "fscale":
            slots[-1] = fscale(slots[-1], slots[-2], cw)
        else:
            raise AssertionError(op)


def check_generic_model():
    """At 0x027F the generic model must give the results of the fixed-word models of the earlier tests."""
    cw = 0x027F
    count = 0
    for a, b in base.pairs80():
        for name, fixed in (("add", base.add), ("sub", fixed_sub.sub), ("mul", fixed_mul.mul), ("div", fixed_div.div)):
            if arith(name, a, b, cw) != fixed(a, b):
                raise AssertionError(f"generic {name}{(a, b)} differs from the fixed-word model")
            count += 1
        if fscale(a, b, cw) != fixed_scale.scale(a, b):
            raise AssertionError(f"generic fscale{(a, b)} differs from the fixed-word model")
        count += 1
    for v in base.m80_singles() + [m80(n, -2) for n in range(-40, 41) if n]:
        checks = [(sqrt(v, cw), fixed_sqrt.sqrt(v)), (rndint(v, cw), fixed_rndint.rndint(v)),
                  (to_binary("m32", v, 0), base.to_binary("m32", v)), (to_binary("m64", v, 0), base.to_binary("m64", v))]
        checks += [(to_integer(v, bits, rc), fixed_integers.to_integer(v, bits, rc == 3))
                   for bits in (16, 32, 64) for rc in (0, 3)]
        for got, want in checks:
            if got != want:
                raise AssertionError(f"generic model differs from the fixed-word model for {v}")
            count += 1
    for name in CONSTS:
        if constant(name, 0) != constants.VALUES[name]:
            raise AssertionError(f"generic {name} differs from the fixed-word model")
        count += 1
    return count


# ---- Inputs ----------------------------------------------------------------------------------------

VALID = [0x007F | pc << 8 | rc << 10 for pc in (0, 2, 3) for rc in range(4)]  # all masked, PC 24/53/64
RESERVED = (0, 0x0040, 0x0080, 0x1000, 0xE000)  # xor masks: bit 6 clear, bit 7, bit 12, bits 13 to 15 set
UNMASKED = [0x027F & ~(1 << k) for k in range(6)]  # one exception unmasked each
PC01 = [0x017F | rc << 10 for rc in range(4)]
RESTORED = (0x027F, 0x023F, 0x02FF, 0xE27F)  # each reads back as 0x027F
UNRESTORED = (0x037F, 0x0E7F, 0x127F)


def block(prefix, cw1, restore=0x027F, cw2=0x027F, flag=0):
    words = cw1.to_bytes(2, "little") + restore.to_bytes(2, "little") + cw2.to_bytes(2, "little")
    return bytes(prefix).ljust(CW1, b"\0") + words + b"\0\0" + bytes([flag]) + b"\0" * 7


def finite(rng, lo=0x3FFF - 40, hi=0x3FFF + 40):
    return J | rng.getrandbits(63), rng.randint(lo, hi) | rng.choice((0, 0x8000))


def negate(v):
    return v[0], v[1] ^ 0x8000


def arith_pairs(rng):
    specials = base.special_m80()
    rows = [(finite(rng), finite(rng)) for _ in range(30)]
    for _ in range(30):  # close exponents: cancellation and carries
        a = finite(rng)
        rows.append((a, (J | rng.getrandbits(63), (a[1] & 0x7FFF) + rng.randint(-3, 3) | rng.choice((0, 0x8000)))))
    rows += [(finite(rng, 0x7FF0, 0x7FFE), finite(rng, 0x7FF0, 0x7FFE)) for _ in range(12)]  # overflow
    rows += [(finite(rng, 0x7FF0, 0x7FFE), finite(rng, 1, 40)) for _ in range(6)]
    rows += [(finite(rng, 1, 40), finite(rng, 1, 40)) for _ in range(12)]  # underflow
    rows += [(finite(rng, 1, 40), finite(rng, 0x7FF0, 0x7FFE)) for _ in range(6)]
    rows += [((rng.getrandbits(63), rng.choice((0, 0x8000))), finite(rng, 1, 70)) for _ in range(6)]  # denormals
    for _ in range(8):  # exact cancellations, and zeros of both signs
        a = finite(rng)
        rows += [(a, a), (a, negate(a))]
    rows += [((0, 0), (0, 0x8000)), ((0, 0x8000), (0, 0)), ((0, 0x8000), (0, 0x8000)), ((0, 0), finite(rng))]
    rows += [(rng.choice(specials), rng.choice(specials)) for _ in range(16)]
    return rows


def singles(rng):
    rows = [finite(rng) for _ in range(30)] + [finite(rng, 0x3FFF - 5000, 0x3FFF + 5000) for _ in range(10)]
    rows += [finite(rng, 1, 70) for _ in range(6)] + [(rng.getrandbits(63), rng.choice((0, 0x8000))) for _ in range(4)]
    rows += [finite(rng, 0x7FF0, 0x7FFE) for _ in range(4)] + base.special_m80()[::2]
    for exp in (127, -126, -149, -150, 128, 1023, -1022, -1074, -1075, 1024):  # binary32 and binary64 edges
        rows += [(J | rng.getrandbits(63), (0x3FFF + exp) | s) for s in (0, 0x8000)]
    for n in (32767, 32768, 2147483647, 2147483648):  # integer limits with fractions
        for f in (Fraction(1, 2), Fraction(1, 4), Fraction(3, 4)):
            rows += [m80(n + f), m80(-(n + f))]
    rows += [m80(2 ** 62 + Fraction(1, 2)), m80(-(2 ** 62 + Fraction(1, 2)))]
    rows += [m80(Fraction(2 * k + 1, 2)) for k in range(-4, 4)] + [m80(Fraction(k, 8)) for k in (1, -1, 3, -3, 5)]
    rows += [(J | rng.getrandbits(63), 0x3FFF + 63 | s) for s in (0, 0x8000)]  # 2^63 and above
    return rows


def scale_pairs(rng):
    rows = []
    for n in (Fraction(-5, 2), Fraction(37, 8), Fraction(-701, 8), Fraction(3, 4), Fraction(-3, 4), 70, -16440,
              -16400, -16380, 16000, 16383, -16445):
        for _ in range(4):
            a = finite(rng, 0x3FFF - 30, 0x3FFF + 30) if abs(n) < 100 else finite(rng)
            rows.append((a, m80(Fraction(n))))
    rows += [(rng.choice(base.special_m80()), m80(Fraction(-3, 2))) for _ in range(6)]
    return rows


def cases():
    """(name, input block) pairs: every value under each supported word, then the rejection words."""
    rng = random.Random(406)
    pairs, ones, scaled = arith_pairs(rng), singles(rng), scale_pairs(rng)
    f64, f32 = base.f64_bits(), base.f32_bits()
    prefix = {
        "pair": [m80_bytes(a) + m80_bytes(b) for a, b in pairs],
        "single": [m80_bytes(v) for v in ones],
        "memory": [m80_bytes(finite(rng)) + b"\0" * 16 + rng.choice(f64).to_bytes(8, "little")
                   + rng.choice(f32).to_bytes(4, "little") for _ in range(60)],
        "iarith": [m80_bytes(finite(rng) if k % 4 else rng.choice(ones)) + b"\0" * 16
                   + rng.getrandbits(32).to_bytes(4, "little") + rng.choice((1, 3, 7, 0x7FFF, 0x8000, rng.getrandbits(16)))
                   .to_bytes(2, "little") for k in range(60)],
        "none": [b""] * 5,
    }
    prefix["scale"] = [m80_bytes(a) + m80_bytes(b) for a, b in scaled]
    for name, (kind, _) in SEQUENCES.items():
        values = prefix["scale" if name == "cw_scale" else kind]
        if name in ("cw_fnstcw", "cw_branch", "cw_writes", "cw_msvc", "cw_fnstsw", "cw_finit_fnstsw"):
            values = values[:24]
        if name == "cw_finit":  # loads no word: each value once, and nothing to reject
            yield from ((name, block(p, VALID[0])) for p in values)
            continue
        for k, p in enumerate(values):
            for j, w in enumerate(VALID):
                cw1 = w ^ RESERVED[(k + j) % len(RESERVED)] if name in ("cw_constants", "cw_fnstcw", "cw_msvc") else w
                if name == "cw_fnstcw":
                    yield name, block(p, cw1, (RESTORED + UNRESTORED)[(k + j) % 7])
                elif name == "cw_branch":
                    for flag in (0, 1):
                        yield name, block(p, cw1, (RESTORED + UNRESTORED)[(k + j + flag) % 7], flag=flag)
                elif name == "cw_writes":
                    yield name, block(p, cw1, cw2=VALID[(k + 5 * j) % len(VALID)])
                else:
                    yield name, block(p, cw1)
        # Words the profile rejects, at the first load and at the restore.
        for w in UNMASKED + PC01:
            for p in values[:3]:
                yield name, block(p, w)
                yield name, block(p, VALID[5], restore=w)
        if name == "cw_writes":
            for w in UNMASKED + PC01:
                for p in values[:5]:
                    yield name, block(p, VALID[7], cw2=w)
        if name in ("cw_add", "cw_msvc"):
            for w in UNRESTORED:
                for p in values[:5]:
                    yield name, block(p, VALID[3], restore=w)


# ---- Check -----------------------------------------------------------------------------------------

def native_safe(data):
    """Whether every word the export could load has all six exceptions masked."""
    return all(int.from_bytes(data[k:k + 2], "little") & 0x3F == 0x3F for k in (CW1, RESTORE, CW2))


class CheckedRunner:
    """Calls the checked entries of a translated DLL with guarded buffers."""

    def __init__(self, harness_path, path):
        self.buffers = base.Runner(harness_path, path)
        self.lib = self.buffers.lib

    def __call__(self, name, data):
        guard, source, target = self.buffers._buffers(data)
        result = ctypes.c_uint64(SENTINEL)
        function = getattr(self.lib, name)
        function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)]
        function.restype = ctypes.c_uint32
        status = function(ctypes.addressof(source) + base.GUARD, ctypes.addressof(target) + base.GUARD,
                          ctypes.byref(result))
        return status, self.buffers._output(name, data, guard, source, target), result.value


def expected_lifted(model):
    return model.status, model.out, model.rax if model.status == STATUS["ok"] else SENTINEL


def check(harness_path, native_path, lifted_path, *control_paths):
    agreed = check_generic_model()
    print(f"the generic model at control word 0x027f agrees with the fixed-word models of the earlier x87 tests "
          f"on {agreed} operations")
    expected = [(name, data, run(name, data)) for name, data in cases()]
    lifted = CheckedRunner(harness_path, lifted_path)
    statuses = dict.fromkeys(STATUS.values(), 0)
    bad = 0
    for name, data, model in expected:
        got = lifted(name, data)
        statuses[model.status] += 1
        if got != expected_lifted(model):
            if bad < 3:
                print(f"lifted mismatch: {name}(input {data.hex()})\n  got      status {got[0]}, {got[1][:40].hex()}, "
                      f"{got[2]:#x}\n  expected status {model.status}, {model.out[:40].hex()}")
            bad += 1
    if bad:
        raise AssertionError(f"lifted: {bad}/{len(expected)} mismatches")
    print(f"{len(expected)} cases over {len(SEQUENCES)} exports matched the model in the checked translation (status, "
          "output block and guards, and *result, which only status 0 writes)")
    print("translated statuses: " + ", ".join(f"{name} {statuses[code]}" for name, code in STATUS.items()))
    native = base.Runner(harness_path, native_path)
    compared = skipped = observed = observed_agree = unrestored = 0
    for name, data, model in expected:
        if not native_safe(data):
            skipped += 1  # an unmasked exception would trap natively
            continue
        got, state = native(name, data)
        control, tags, top = state & 0xFFFF, (state >> 32) & 0xFFFF, (state >> 16) & 0x3800
        if model.status == STATUS["precision"]:
            seen = run(name, data, pc01=True)
            observed += 1
            observed_agree += got == seen.out and control == seen.cw
            continue
        if got != model.out or (control, tags, top) != (model.cw, 0xFFFF, 0):
            raise AssertionError(f"native {name}(input {data.hex()}) gave {got[:40].hex()} with control {control:#06x}, "
                                 f"expected {model.out[:40].hex()} with control {model.cw:#06x}")
        compared += 1
        unrestored += model.status == STATUS["unrestored"]
    print(f"native: {compared} cases matched the model, with the control word each export left; {unrestored} of them "
          f"return with a word other than 0x027f, which the checked entry rejects. {skipped} cases with an "
          "unmasked exception ran only translated")
    print(f"native precision control 01 (observation, rejected by the profile): {observed_agree} of {observed} cases "
          "matched a 64-bit model")
    print(f"processor: {platform.processor()}")
    for path in control_paths:
        control = Path(path).stem.rsplit("_", 1)[-1]
        run_control = CheckedRunner(harness_path, path)
        bad = sum(1 for name, data, model in expected if run_control(name, data) != expected_lifted(model))
        print(f"control {control}: {bad}/{len(expected)} mismatches")
        if not bad:
            raise AssertionError(f"control {control} matched every case")
    check_rejections()


def check_rejections():
    from iced_x86 import Decoder
    from ffx_version_query import lower
    from x87_lower import Checked

    def attempt(hex_bytes, checked=True, calls=None, exits=None):
        body = list(Decoder(64, bytes.fromhex(hex_bytes), ip=0x1000))
        try:
            lower(body, [], {}, calls or {}, exits or {}, x87=Checked(REJECT) if checked else True)
        except ValueError as exc:
            return str(exc)
        return None

    fldcw, fld, fstp = "d9 29", "db 29", "db 3a"  # fldcw word ptr [rcx]; fld/fstp tbyte ptr [rcx]/[rdx]
    live = "x87 control word may be live"
    unknown = "stores status-word bits"
    rejected = {
        "fldcw in the fixed profile": (attempt(f"{fldcw} {fld} {fstp} c3", checked=False), "unsupported x87 operation"),
        "fldcw and a call": (attempt(f"{fldcw} e8 00 00 00 00 c3", calls={0x1002: ";"}), live),
        "fldcw and an indirect jump": (attempt(f"{fldcw} ff e0"), live),
        "fldcw and an external edge": (attempt(f"{fldcw} e9 00 01 00 00", exits={0x1107: "return 0;"}),
                                       "control word may be live at the external edge"),
        "fldcw and a return with a value": (attempt(f"{fldcw} {fld} c3"), "x87 stack not empty at return"),
        "fnstsw m16 after a load": (attempt(f"{fldcw} {fld} dd 3a {fstp} c3"), unknown),
        "fnstsw m16 at the entry": (attempt("dd 3a c3"), unknown),
        "fnstsw m16 in the fixed profile": (attempt(f"{fld} dd 3a {fstp} c3", checked=False), unknown),
        "fnstsw m16 after fninit and wait (fstsw m16)": (attempt("db e3 9b dd 3a c3"), unknown),
        "fnstsw m16 after fninit and fnstcw": (attempt("db e3 d9 3a dd 3a c3"), unknown),
        "fnstsw m16 after fninit and fnstsw ax": (attempt("db e3 df e0 dd 3a c3"), unknown),
        "fnstsw m16 twice after fninit": (attempt("db e3 dd 3a dd 3a c3"), unknown),
        "fnstsw m16 after fninit, two loads and fcomi": (attempt(f"db e3 {fld} {fld} db f1 dd 3a dd d8 dd d8 c3"), unknown),
        "fnstsw m16 after fninit, two loads and fprem": (attempt(f"db e3 {fld} {fld} d9 f8 dd 3a dd d8 dd d8 c3"), unknown),
        "fnstsw m16 where a path skips fninit": (attempt("85 c0 74 02 db e3 dd 3a c3"), unknown),
        "fninit in the fixed profile": (attempt(f"{fld} db e3 c3", checked=False), "unsupported x87 operation"),
        "fninit and a call": (attempt(f"db e3 e8 00 00 00 00 c3", calls={0x1002: ";"}), live),
        "a store after fninit": (attempt(f"{fld} db e3 {fstp} c3"), "x87 stack underflow"),
    }
    for case, (message, needle) in rejected.items():
        if message is None or needle not in message:
            raise AssertionError(f"{case}: expected a rejection containing {needle!r}, got {message!r}")
    print(f"rejected {len(rejected)} sequences: " + "; ".join(rejected))
    accepted = {
        "a call in a checked function without fldcw": attempt(f"e8 00 00 00 00 {fld} {fstp} c3", calls={0x1000: ";"}),
        "fldcw around a load and a store": attempt(f"{fldcw} {fld} {fstp} d9 69 02 c3"),
        "fninit with a value on the stack, then a restore": attempt(f"{fld} db e3 d9 69 02 c3"),
        "fnstsw m16 right after fninit": attempt("db e3 dd 3a c3"),
        "fnstsw m16 after fninit and a flag-neutral store": attempt("db e3 89 02 dd 3a c3"),
        "fnstsw m16 after two paths that each run fninit": attempt("85 c0 74 04 db e3 eb 02 db e3 dd 3a c3"),
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
