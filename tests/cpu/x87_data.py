"""Compare native and translated x87 data movement: the register stack, loads, stores, integer
conversions, constants, exchanges and sign changes.

Usage:
    python tests/cpu/x87_data.py asm <asm> <def>            regenerate the MASM source and exports
    python tests/cpu/x87_data.py emit <dll> <cpp> [control]  translate (and optionally mutate) the fixture
    python tests/cpu/x87_data.py check <state probe> <native> <lifted> [controls...]

Each export runs one instruction sequence from SEQUENCES on a 160-byte input block at RCX and writes a
192-byte output block at RDX, leaving the x87 stack empty. The check compares the whole output block,
prefilled with 0xcc, against the native DLL and an independent Python model that uses exact rational
arithmetic.
"""
from decimal import Decimal, getcontext
from fractions import Fraction
import ctypes
from pathlib import Path
import random
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

INPUT, OUTPUT = 160, 192

# name: (input kind, sequence). Memory operands are (kind, offset) into the input (loads) or output (stores).
SEQUENCES = {
    "m64_roundtrip": ("f64", [("fld", "m64", 0), ("fstp", "m64", 0)]),
    "m32_roundtrip": ("f32", [("fld", "m32", 0), ("fstp", "m32", 0)]),
    "m32_to_m64": ("f32", [("fld", "m32", 0), ("fstp", "m64", 0)]),
    "m64_to_m32": ("f64", [("fld", "m64", 0), ("fstp", "m32", 0)]),
    "m64_to_m80": ("f64", [("fld", "m64", 0), ("fstp", "m80", 0)]),
    "m32_to_m80": ("f32", [("fld", "m32", 0), ("fstp", "m80", 0)]),
    "m80_roundtrip": ("m80", [("fld", "m80", 0), ("fstp", "m80", 0)]),
    "m80_to_m64": ("m80", [("fld", "m80", 0), ("fstp", "m64", 0)]),
    "m80_to_m32": ("m80", [("fld", "m80", 0), ("fstp", "m32", 0)]),
    "m80_fst": ("m80", [("fld", "m80", 0), ("fst", "m32", 0), ("fst", "m64", 8), ("fstp", "m80", 16)]),
    "m80_to_int": ("m80", [("fld", "m80", 0), ("fist", "m16", 0), ("fist", "m32", 8),
                           ("fld", "st", 0), ("fistp", "m64", 16), ("fld", "st", 0), ("fistp", "m16", 24),
                           ("fld", "st", 0), ("fistp", "m32", 32), ("fld", "st", 0), ("fisttp", "m16", 40),
                           ("fld", "st", 0), ("fisttp", "m32", 48), ("fisttp", "m64", 56)]),
    "int_to_m80": ("int", [("fild", "m16", 0), ("fstp", "m80", 0), ("fild", "m32", 8), ("fstp", "m80", 16),
                           ("fild", "m64", 16), ("fstp", "m80", 32)]),
    "constants": ("none", [("fld1",), ("fstp", "m80", 0), ("fldz",), ("fstp", "m80", 16), ("fldpi",),
                           ("fstp", "m80", 32), ("fldl2e",), ("fstp", "m80", 48), ("fldl2t",), ("fstp", "m80", 64),
                           ("fldlg2",), ("fstp", "m80", 80), ("fldln2",), ("fstp", "m80", 96)]),
    "sign_ops": ("m80", [("fld", "m80", 0), ("fchs",), ("fstp", "m80", 0), ("fld", "m80", 0), ("fabs",),
                         ("fstp", "m80", 16), ("fld", "m80", 0), ("fabs",), ("fchs",), ("fstp", "m80", 32)]),
    "stack_moves": ("m80x4", [("fld", "m80", 0), ("fld", "m80", 16), ("fld", "m80", 32), ("fld", "m80", 48),
                              ("fxch", 2), ("fld", "st", 3), ("fst", "st", 2), ("fstp", "st", 4), ("fnop",),
                              ("wait",), ("fstp", "m80", 0), ("fstp", "m80", 16), ("fxch", 1), ("fstp", "m80", 32),
                              ("fstp", "m80", 48)]),
    "stack_free": ("m80x4", [("fld", "m80", 48), ("ffreep", 0), ("fld", "m80", 0), ("fld", "m80", 16),
                             ("fld", "m80", 32), ("ffree", 1), ("fld", "st", 1), ("fstp", "m80", 0), ("fincstp",),
                             ("fstp", "m80", 16), ("fstp", "m80", 32), ("fdecstp",), ("fdecstp",), ("fdecstp",),
                             ("fstp", "m80", 48), ("fincstp",), ("fincstp",)]),
    "stack_overflow": ("m80x9", [*(("fld", "m80", 16 * k) for k in range(9)),
                                 *(("fstp", "m80", 16 * k) for k in range(8))]),
    "stack_underflow": ("none", [("fst", "m64", 0), ("fistp", "m32", 8), ("fld", "st", 5), ("fchs",),
                                 ("fstp", "m80", 16), ("fxch", 1), ("fstp", "m80", 32), ("fstp", "m80", 48),
                                 ("fabs",), ("fstp", "m80", 64), ("fistp", "m16", 80), ("fisttp", "m64", 88),
                                 ("fstp", "m32", 96), ("fincstp",)]),
}

PTR = {"m16": "word", "m32": "dword", "m64": "qword", "m80": "tbyte"}

# Source mutations of the generated C++ (the header text is part of it). Each control DLL must mismatch.
CONTROLS = {
    # Stores to m32 and m64 truncate instead of rounding by the control word.
    "truncate": (re.compile(r"x87_round\(u\.sign, u\.e, u\.sig, 0, format, x87_rc\(f\)\)"),
                 "x87_round(u.sign, u.e, u.sig, 0, format, 3u)"),
    # A result below the destination's normal range keeps its significand instead of being denormalized.
    "nodenormal": (re.compile(r"x87_shr_sticky\(hi, lo, 1 - biased\);"), ";"),
    # Loading a signaling m32 or m64 NaN keeps it signaling.
    "noquiet": (re.compile(r"return x87_quiet\(\{x87_integer_bit \| \(fraction"), "return X87Value({x87_integer_bit | (fraction"),
    # Reading an empty register returns its stale contents instead of the indefinite.
    "nounderflow": (re.compile(r"return x87_empty\(f, i\) \? x87_indefinite : "), "return "),
    # A push onto a full stack keeps the pushed value instead of the indefinite.
    "nooverflow": (re.compile(r"if \(\(f\.valid >> f\.top\) & 1u\) v = x87_indefinite;"), ";"),
    # The load-constant instructions truncate their constants instead of rounding to nearest.
    "constants": (re.compile(r"rc == 0 \? c\.nearest_up :"), "rc == 0 ? false :"),
}


# ---- MASM generation -------------------------------------------------------------------------------

def asm_line(step):
    op, *args = step
    if op in ("fld", "fst", "fstp", "fild", "fist", "fistp", "fisttp") and args[0] != "st":
        base = "rcx" if op in ("fld", "fild") else "rdx"
        return f"    {op} {PTR[args[0]]} ptr [{base}+{args[1]}]"
    if op in ("fld", "fst", "fstp"):
        return f"    {op} st({args[1]})"
    if op in ("fxch", "ffree", "ffreep"):
        return f"    {op} st({args[0]})"
    return f"    {op}"


def write_asm(asm_path, def_path):
    lines = ["; Generated by tests/cpu/x87_data.py asm; edit SEQUENCES there and regenerate.",
             "; Source-owned differential fixture for x87 data movement. Each export reads an input block at RCX,",
             "; writes an output block at RDX and returns with the x87 stack empty.", ".code", ""]
    for name, (_, steps) in SEQUENCES.items():
        lines += [f"{name} PROC FRAME", "    .endprolog", *(asm_line(s) for s in steps), "    ret", f"{name} ENDP", ""]
    lines.append("END")
    Path(asm_path).write_text("\n".join(lines) + "\n", encoding="ascii")
    Path(def_path).write_text("EXPORTS\n" + "".join(f"    {name}\n" for name in SEQUENCES), encoding="ascii")


# ---- Translation -----------------------------------------------------------------------------------

def emit(image, output, control="lifted"):
    import pefile
    from cpu_leaf_aot import export_rva
    from cpu_ranges import function_ranges
    from ffx_version_query import INTEGER_HEADER, lower, region
    from x87_lower import X87_HEADER

    parts = [INTEGER_HEADER, X87_HEADER]
    with pefile.PE(str(image), fast_load=True) as pe:
        for name in SEQUENCES:
            (bounds,) = function_ranges(pe, export_rva(pe, name))
            code = lower(region(pe, *bounds), ["rcx = a; rdx = b;"], {}, {}, {})
            parts.append(f'extern "C" __declspec(dllexport) word {name}(word a, word b) {{\n{code}\n}}\n')
    text = "".join(parts)
    if control != "lifted":
        expression, replacement = CONTROLS[control]
        text, count = expression.subn(replacement, text)
        if not count:
            raise AssertionError(f"control {control} changed nothing")
        print(f"control {control}: {count} replacements")
    output.write_text(text, encoding="ascii")


# ---- Independent model -----------------------------------------------------------------------------
# Values are raw 80-bit pairs (sig, se). Finite values are classified and converted through Fraction.

INDEFINITE = (0xC000000000000000, 0xFFFF)
J = 1 << 63
BINARY = {"m32": (24, 127, 8), "m64": (53, 1023, 11)}  # precision, bias, exponent bits


def floor_log2(q):
    e = q.numerator.bit_length() - q.denominator.bit_length()
    if Fraction(2) ** e > q:
        e -= 1
    elif Fraction(2) ** (e + 1) <= q:
        e += 1
    return e


def classify(v):
    sig, se = v
    exp, sign = se & 0x7FFF, se >> 15
    if exp != 0 and not sig & J:
        return ("unsupported",)
    if exp == 0x7FFF:
        return ("inf", sign) if sig == J else ("nan", sign, sig)
    if exp == 0 and sig == 0:
        return ("zero", sign)
    scale = 1 - 16383 - 63 if exp == 0 else exp - 16383 - 63
    return ("finite", sign, Fraction(sig) * Fraction(2) ** scale)


def m80_exact(sign, q):
    """Encode a nonzero Fraction that fits a 64-bit significand exactly."""
    e = floor_log2(q)
    sig = q / Fraction(2) ** (e - 63)
    assert sig.denominator == 1 and J <= sig < 2 * J, q
    return int(sig), (sign << 15) | (e + 16383)


def from_binary(kind, bits):
    precision, bias, exp_bits = BINARY[kind]
    fb = precision - 1
    sign = bits >> (fb + exp_bits)
    exp = (bits >> fb) & ((1 << exp_bits) - 1)
    frac = bits & ((1 << fb) - 1)
    if exp == (1 << exp_bits) - 1:
        if frac == 0:
            return J, (sign << 15) | 0x7FFF
        return J | 1 << 62 | frac << (63 - fb), (sign << 15) | 0x7FFF  # quieted
    if exp == 0:
        return (0, sign << 15) if frac == 0 else m80_exact(sign, Fraction(frac) * Fraction(2) ** (1 - bias - fb))
    return m80_exact(sign, (Fraction(frac) / 2 ** fb + 1) * Fraction(2) ** (exp - bias))


def round_binary(sign, q, precision, bias, emax_biased):
    """Round |value| q to nearest-even in a binary format with hidden bit; returns (biased, mantissa field)."""
    e = floor_log2(q)
    quantum = max(e, 1 - bias) - (precision - 1)
    scaled = q / Fraction(2) ** quantum
    m = scaled.numerator // scaled.denominator
    rest = scaled - m
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and m & 1):
        m += 1
    if m == 1 << precision:
        m >>= 1
        quantum += 1
    if m < 1 << (precision - 1):
        return 0, m
    biased = quantum + precision - 1 + bias
    if biased > emax_biased:
        return emax_biased + 1, 0  # nearest rounds an overflow to infinity
    return biased, m - (1 << (precision - 1))


def to_binary(kind, v):
    precision, bias, exp_bits = BINARY[kind]
    fb = precision - 1
    all_ones = (1 << exp_bits) - 1
    c = classify(v)
    if c[0] == "unsupported":
        return 1 << (fb + exp_bits) | all_ones << fb | 1 << (fb - 1)
    sign = c[1]
    top = sign << (fb + exp_bits)
    if c[0] == "nan":
        return top | all_ones << fb | 1 << (fb - 1) | (c[2] >> (63 - fb)) & ((1 << fb) - 1)
    if c[0] == "inf":
        return top | all_ones << fb
    if c[0] == "zero":
        return top
    biased, field = round_binary(sign, c[2], precision, bias, all_ones - 1)
    return top | biased << fb | field


def to_int(v, truncate, bits):
    indefinite = 1 << (bits - 1)
    c = classify(v)
    if c[0] in ("unsupported", "nan", "inf"):
        return indefinite
    if c[0] == "zero":
        return 0
    q = -c[2] if c[1] else c[2]
    n = int(q) if truncate else round(q)  # int() truncates; round() of a Fraction ties to even
    if not -(1 << (bits - 1)) <= n < 1 << (bits - 1):
        return indefinite
    return n & ((1 << bits) - 1)


def constant(name):
    getcontext().prec = 70
    ln2, ln10 = Decimal(2).ln(), Decimal(10).ln()
    pi = Decimal(0)  # Bailey-Borwein-Plouffe, 60 terms
    for k in range(60):
        pi += (Decimal(4) / (8 * k + 1) - Decimal(2) / (8 * k + 4) - Decimal(1) / (8 * k + 5)
               - Decimal(1) / (8 * k + 6)) / Decimal(16) ** k
    value = {"fldpi": pi, "fldl2e": 1 / ln2, "fldl2t": ln10 / ln2, "fldlg2": ln2 / ln10, "fldln2": ln2}[name]
    q = Fraction(value)
    e = floor_log2(q)
    scaled = q / Fraction(2) ** (e - 63)
    sig = scaled.numerator // scaled.denominator
    if scaled - sig > Fraction(1, 2):  # no constant is within 2^-60 of a tie
        sig += 1
    return sig, e + 16383


class Model:
    def __init__(self, data):
        self.data, self.out = data, bytearray(b"\xcc" * OUTPUT)
        self.regs, self.top = [None] * 8, 0

    def phys(self, i):
        return (self.top + i) & 7

    def st(self, i):
        v = self.regs[self.phys(i)]
        return INDEFINITE if v is None else v

    def set(self, i, v):
        self.regs[self.phys(i)] = v

    def push(self, v):
        self.top = (self.top - 1) & 7
        self.regs[self.top] = INDEFINITE if self.regs[self.top] is not None else v

    def pop(self):
        self.regs[self.top] = None
        self.top = (self.top + 1) & 7

    def load(self, kind, offset):
        raw = int.from_bytes(self.data[offset:offset + {"m16": 2, "m32": 4, "m64": 8, "m80": 10}[kind]], "little")
        return raw

    def store(self, offset, value, size):
        self.out[offset:offset + size] = value.to_bytes(size, "little")

    def step(self, step):
        op, *args = step
        if op == "fld":
            if args[0] == "st":
                self.push(self.st(args[1]))
            elif args[0] == "m80":
                raw = self.load("m80", args[1])
                self.push((raw & (2 ** 64 - 1), raw >> 64))
            else:
                self.push(from_binary(args[0], self.load(args[0], args[1])))
        elif op in ("fst", "fstp"):
            if args[0] == "st":
                self.set(args[1], self.st(0))
            elif args[0] == "m80":
                sig, se = self.st(0)
                self.store(args[1], sig | se << 64, 10)
            else:
                self.store(args[1], to_binary(args[0], self.st(0)), {"m32": 4, "m64": 8}[args[0]])
            if op == "fstp":
                self.pop()
        elif op == "fild":
            bits = {"m16": 16, "m32": 32, "m64": 64}[args[0]]
            n = self.load(args[0], args[1])
            n -= (n >> (bits - 1)) << bits
            self.push((0, 0) if n == 0 else m80_exact(int(n < 0), Fraction(abs(n))))
        elif op in ("fist", "fistp", "fisttp"):
            bits = {"m16": 16, "m32": 32, "m64": 64}[args[0]]
            self.store(args[1], to_int(self.st(0), op == "fisttp", bits), bits // 8)
            if op != "fist":
                self.pop()
        elif op == "fxch":
            a, b = self.st(0), self.st(args[0])
            self.set(0, b)
            self.set(args[0], a)
        elif op in ("fchs", "fabs"):
            if self.regs[self.phys(0)] is None:
                self.set(0, INDEFINITE)
            else:
                sig, se = self.st(0)
                self.set(0, (sig, se ^ 0x8000 if op == "fchs" else se & 0x7FFF))
        elif op in ("fnop", "wait"):
            pass
        elif op == "ffree":
            self.regs[self.phys(args[0])] = None
        elif op == "ffreep":
            self.regs[self.phys(args[0])] = None
            self.pop()
        elif op == "fincstp":
            self.top = (self.top + 1) & 7
        elif op == "fdecstp":
            self.top = (self.top - 1) & 7
        elif op == "fld1":
            self.push((J, 0x3FFF))
        elif op == "fldz":
            self.push((0, 0))
        else:
            self.push(constant(op))


def reference(name, data):
    model = Model(data)
    for step in SEQUENCES[name][1]:
        model.step(step)
    return bytes(model.out)


# ---- Inputs ----------------------------------------------------------------------------------------

def m80_values():
    values = [
        (0, 0), (0, 0x8000), (1, 0), (J - 1, 0), (J, 0), (0xFFFFFFFFFFFFFFFF, 0x8000),  # denormals, pseudo-denormals
        (J, 0x3FFF), (J, 0xBFFF), (0xFFFFFFFFFFFFFFFF, 0x7FFE), (J, 1), (J | 1, 0x3FFF),
        (J, 0x7FFF), (J, 0xFFFF), INDEFINITE, (0xC000000000000001, 0x7FFF), (J | 1, 0x7FFF),  # inf, QNaN, SNaN
        (0xA000000000000000, 0xFFFF), (0xFFFFFFFFFFFFFFFF, 0x7FFF),
        (0x4000000000000000, 0x3FFF), (0, 0x3FFF), (0, 0x7FFF), (0x4000000000000000, 0x7FFF),  # unnormal, pseudo
        (0x7FFFFFFFFFFFFFFF, 0xC010),
    ]
    for exp in (0x3FFF - 1022, 0x3FFF - 1023, 0x3FFF - 1074, 0x3FFF - 1075, 0x3FFF - 1076, 0x3FFF + 1023, 0x3FFF + 1024,
                0x3FFF - 126, 0x3FFF - 149, 0x3FFF - 150, 0x3FFF + 127, 0x3FFF + 128, 0x3FFF - 127):
        for sig in (J, J | 1, J | 0x3FF, J | 0x400, J | 0x7FF, J | 0x800, 0xFFFFFFFFFFFFF800, 0xFFFFFFFFFFFFFFFF,
                    J | 0xFFFFFFFFFF, J | 0x8000000000, 0xFFFFFF0000000000, 0xFFFFFF8000000000):
            values += [(sig, exp), (sig, exp | 0x8000)]
    for n in (0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.25, 0.75, 32767, 32767.5, 32768, -32768, -32768.5, -32769,
              2 ** 31 - 1, 2 ** 31 - 0.5, 2 ** 31, -2 ** 31, -2 ** 31 - 0.5, -2 ** 31 - 1, 2 ** 63, -2 ** 63,
              2 ** 63 - 1024, 2 ** 62 + 0.5):
        q = Fraction(n)
        values.append(m80_exact(int(q < 0), abs(q)))
    values += [(J | (2 ** 63 - 1), 0x3FFF + 62), (J | 1, 0x3FFF + 63), (J, 0xBFFF + 63), (J | 1, 0xBFFF + 63)]
    rng = random.Random(87)
    for _ in range(60):
        values.append((J | rng.getrandbits(63), rng.choice((0x3FFF, 0x4005, 0x3FF0, 0x403E, 0x4040, 0x43FE, 0x3C00))
                       | rng.choice((0, 0x8000))))
    return values


def f64_values():
    bits = [0, 1 << 63, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x7FF0000000000000,
            0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0x7FF4000000000000,
            0xFFF0000000000123, 0x3FF0000000000000, 0xBFF0000000000001, 0x3810000000000000, 0x380FFFFFE0000000,
            0x380FFFFFF0000000, 0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x36A0000000000000, 0x3690000000000000,
            0x3690000000000001, 0x3FF0000010000000, 0x3FF0000030000000, 0x8000000000000001]
    rng = random.Random(64)
    bits += [rng.getrandbits(64) for _ in range(40)]
    return bits


def f32_values():
    bits = [0, 0x80000000, 1, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
            0x7F800001, 0x7FA00000, 0xFF801234, 0x3F800000, 0xBF800001]
    rng = random.Random(32)
    bits += [rng.getrandbits(32) for _ in range(30)]
    return bits


def int_blocks():
    rng = random.Random(16)
    rows = [(0, 0, 0), (1, -1, 1), (-1, 1, -1), (32767, 2 ** 31 - 1, 2 ** 63 - 1), (-32768, -2 ** 31, -2 ** 63),
            (12345, 2 ** 24 + 1, 2 ** 53 + 1), (-2, -(2 ** 24) - 1, 2 ** 63 - 2 ** 10 - 1)]
    rows += [(rng.randint(-32768, 32767), rng.randint(-2 ** 31, 2 ** 31 - 1), rng.randint(-2 ** 63, 2 ** 63 - 1))
             for _ in range(20)]
    return [a.to_bytes(2, "little", signed=True) + b"\0" * 6 + b.to_bytes(4, "little", signed=True) + b"\0" * 4
            + c.to_bytes(8, "little", signed=True) for a, b, c in rows]


def m80_bytes(v):
    return v[0].to_bytes(8, "little") + v[1].to_bytes(2, "little") + b"\0" * 6


def cases():
    m80 = m80_values()
    blocks = {
        "f64": [b.to_bytes(8, "little") for b in f64_values()],
        "f32": [b.to_bytes(4, "little") for b in f32_values()],
        "m80": [m80_bytes(v) for v in m80],
        "int": int_blocks(),
        "none": [b""],
        "m80x4": [b"".join(m80_bytes(m80[(7 * k + j) % len(m80)]) for j in range(4)) for k in range(40)],
        "m80x9": [b"".join(m80_bytes(m80[(11 * k + 3 * j) % len(m80)]) for j in range(9)) for k in range(20)],
    }
    for name, (kind, _) in SEQUENCES.items():
        for block in blocks[kind]:
            yield name, block.ljust(INPUT, b"\0")


# ---- Check -----------------------------------------------------------------------------------------

def load_lib(path):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    for name in SEQUENCES:
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_uint64
    return lib


def mismatches(lib, expected):
    found = []
    for name, data, want in expected:
        source = (ctypes.c_uint8 * INPUT).from_buffer_copy(data)
        target = (ctypes.c_uint8 * OUTPUT).from_buffer_copy(b"\xcc" * OUTPUT)
        getattr(lib, name)(ctypes.addressof(source), ctypes.addressof(target))
        got = bytes(target)
        if got != want or bytes(source) != data:
            found.append((name, data, want, got))
    return found


def describe(case):
    name, data, want, got = case
    first = next(k for k in range(OUTPUT) if want[k] != got[k]) if want != got else None
    return (f"{name}(input {data[:32].hex()}): first differing output byte {first}; "
            f"got {got[:64].hex()}, expected {want[:64].hex()}")


def check_initial_state(probe_path):
    """The model starts from control word 0x027F and an empty stack; confirm a native export sees that.
    The probe (tests/cpu/x87_state.asm) is native only, so its FNSTENV and FLDCW are not counted as covered."""
    state = ctypes.CDLL(str(Path(probe_path).resolve())).fpu_state
    state.restype = ctypes.c_uint64
    value = state()
    control, status, tags = value & 0xFFFF, (value >> 16) & 0xFFFF, (value >> 32) & 0xFFFF
    if (control, tags) != (0x027F, 0xFFFF) or status & 0x3800:
        raise AssertionError(f"native x87 state: control {control:#06x}, status {status:#06x}, tags {tags:#06x}; "
                             "the model assumes control 0x027f, TOP 0 and an empty stack")
    print(f"native x87 state: control {control:#06x}, status {status:#06x}, tags {tags:#06x}")


def check(probe_path, native_path, lifted_path, *control_paths):
    check_initial_state(probe_path)
    expected =[(name, data, reference(name, data)) for name, data in cases()]
    for label, path in (("native", native_path), ("lifted", lifted_path)):
        bad = mismatches(load_lib(path), expected)
        if bad:
            raise AssertionError(f"{label}: {len(bad)} mismatches; first {describe(bad[0])}")
    print(f"{len(expected)} cases over {len(SEQUENCES)} exports (192 output bytes each) matched the reference "
          "in native and translated DLLs")
    for path in control_paths:
        control = Path(path).stem.rsplit("_", 1)[-1]
        bad = mismatches(load_lib(path), expected)
        print(f"control {control}: {len(bad)}/{len(expected)} mismatches")
        if not bad:
            raise AssertionError(f"control {control} matched every case")
    check_rejections()


def check_rejections():
    from iced_x86 import Decoder
    from ffx_version_query import lower

    # 9B DD /7 (FSTSW) decodes as WAIT, which lowers, followed by the rejected FNSTSW.
    for hex_bytes, text in (("df e0", "fnstsw ax"), ("9b dd 39", "fnstsw [rcx]"), ("d9 29", "fldcw"), ("d9 39", "fnstcw"),
                            ("d9 fe", "fsin"), ("d8 c1", "fadd"), ("db e3", "fninit"), ("d9 e4", "ftst")):
        try:
            lower(list(Decoder(64, bytes.fromhex(hex_bytes + " c3"), ip=0x1000)), [], {}, {}, {})
        except ValueError as exc:
            if "unsupported" not in str(exc) or text not in str(exc):
                raise AssertionError(f"unexpected diagnostic for {text}: {exc}") from exc
        else:
            raise AssertionError(f"accepted {text}")
    print("rejected fnstsw (alone and after WAIT), fldcw, fnstcw, fsin, fadd, fninit and ftst")


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
