"""Independent single-precision reference for the packed SSE arithmetic checks.

Exact rational arithmetic (fractions.Fraction) with the four MXCSR rounding modes, DAZ and FTZ.
It uses no host floating-point instruction and none of the translator, so the processor and the
lifted code are both compared with arithmetic they cannot share.

A lane the reference cannot decide is None and stays None through later operations:
  - FTZ, when the exact result is below the smallest normal number but rounds up to it (the
    manuals leave open whether tininess is judged before or after rounding);
  - MINPS and MAXPS under DAZ when the returned operand is a denormal (whether the hardware
    hands back the original bits or a flushed zero).
"""

from fractions import Fraction
from math import isqrt

MASK32 = 0xFFFFFFFF
DEFAULT_NAN = 0xFFC00000
QUIET_BIT = 0x00400000
RN, RD, RU, RZ = range(4)
MIN_NORMAL = Fraction(1, 2 ** 126)
HUGE = Fraction(10 ** 100)


class Mode:
    def __init__(self, rc=RN, daz=False, ftz=False):
        self.rc, self.daz, self.ftz = rc, daz, ftz

    def mxcsr(self):
        return 0x1F80 | (self.rc << 13) | (0x40 if self.daz else 0) | (0x8000 if self.ftz else 0)

    def label(self):
        return f"rc={'RN RD RU RZ'.split()[self.rc]} daz={int(self.daz)} ftz={int(self.ftz)}"


DEFAULT_MODE = Mode()
ALL_MODES = tuple(Mode(rc, flag, flag) for flag in (False, True) for rc in (RN, RD, RU, RZ))


def is_nan(bits):
    return (bits & 0x7FFFFFFF) > 0x7F800000


def is_denormal(bits):
    return (bits & 0x7F800000) == 0 and (bits & 0x7FFFFF) != 0


def flush_denormal(bits, mode):
    return bits & 0x80000000 if mode.daz and is_denormal(bits) else bits


def decode(bits):
    """(sign, None) for an infinity, (sign, Fraction) for a finite value (zero included)."""
    sign, exp, frac = bits >> 31, (bits >> 23) & 0xFF, bits & 0x7FFFFF
    if exp == 255:
        return sign, None
    if exp == 0:
        return sign, Fraction(frac, 2 ** 149)
    return sign, Fraction(frac | 0x800000) * Fraction(2) ** (exp - 150)


def magnitude_key(bits):
    sign, value = decode(bits)
    return -HUGE if sign and value is None else HUGE if value is None else (-value if sign else value)


def round_exact(value, mode, quirk=None):
    """Round a nonzero exact value to binary32 bits, or None when the lane is undecidable."""
    rc = RN if quirk == "ignore_rounding_mode" else mode.rc
    sign = 1 if value < 0 else 0
    mag = abs(value)
    exp = mag.numerator.bit_length() - mag.denominator.bit_length()
    while Fraction(2) ** exp > mag:
        exp -= 1
    while Fraction(2) ** (exp + 1) <= mag:
        exp += 1
    exp = max(exp, -126)
    scaled = mag / Fraction(2) ** (exp - 23)
    whole = scaled.numerator // scaled.denominator
    rest = scaled - whole
    if rest and (rc == RN and (rest > Fraction(1, 2) or (rest == Fraction(1, 2) and whole & 1))
                 or rc == RD and sign or rc == RU and not sign):
        whole += 1
    tiny = mag < MIN_NORMAL
    if tiny and mode.ftz and quirk != "ignore_ftz":
        return None if whole == 1 << 23 else sign << 31
    if whole == 1 << 24:
        whole, exp = 1 << 23, exp + 1
    if whole == 0:
        return sign << 31
    biased = exp + 127 if whole >= 1 << 23 else 0
    if biased >= 255:
        to_infinity = rc == RN or (rc == RD and sign) or (rc == RU and not sign)
        return (sign << 31) | (0x7F800000 if to_infinity else 0x7F7FFFFF)
    return (sign << 31) | (biased << 23) | (whole - (1 << 23) if biased else whole)


def nan_source(a, b, quirk):
    first, second = (b, a) if quirk == "nan_second_source" else (a, b)
    if is_nan(first):
        return first if quirk == "no_quieting" else first | QUIET_BIT
    if is_nan(second):
        return second if quirk == "no_quieting" else second | QUIET_BIT
    return None


def exact_zero_sign(mode, quirk):
    return 1 if mode.rc == RD else 0


def lane_arith(op, a, b, mode, quirk=None):
    if is_nan(a) or is_nan(b):
        return nan_source(a, b, quirk)
    if not (mode.daz and quirk == "ignore_daz"):
        a, b = flush_denormal(a, mode), flush_denormal(b, mode)
    sa, x = decode(a)
    sb, y = decode(b)
    if op == "sub":
        sb ^= 1
    if op in ("add", "sub"):
        if x is None or y is None:
            if x is None and y is None and sa != sb:
                return DEFAULT_NAN
            return (sa if x is None else sb) << 31 | 0x7F800000
        total = (-x if sa else x) + (-y if sb else y)
        if total == 0:
            if x == 0 and y == 0 and sa == sb:
                return sa << 31
            return (1 if quirk == "mixed_zero_negative" else exact_zero_sign(mode, quirk)) << 31
        return round_exact(total, mode, quirk)
    sign = sa ^ sb
    if op == "mul":
        if x is None or y is None:
            return DEFAULT_NAN if x == 0 or y == 0 else sign << 31 | 0x7F800000
        return round_exact((-1 if sign else 1) * x * y, mode, quirk) if x * y else sign << 31
    if x is None:
        return DEFAULT_NAN if y is None else sign << 31 | 0x7F800000
    if y is None:
        return sign << 31
    if y == 0:
        if quirk == "divide_zero_nan":
            return DEFAULT_NAN
        return DEFAULT_NAN if x == 0 else sign << 31 | 0x7F800000
    return round_exact((-1 if sign else 1) * x / y, mode, quirk) if x else sign << 31


def lane_minmax(op, a, b, mode, quirk=None):
    if is_nan(a) or is_nan(b):
        chosen = a if quirk == "nan_first_operand" and is_nan(a) else b
    else:
        ka = magnitude_key(flush_denormal(a, mode))
        kb = magnitude_key(flush_denormal(b, mode))
        if ka == 0 and kb == 0:
            chosen = a if quirk == "zero_returns_first" else b
        elif op == "min":
            chosen = a if (ka > kb if quirk == "min_as_max" else ka < kb) else b
        else:
            chosen = a if ka > kb else b
    if mode.daz and is_denormal(chosen):
        return None
    return chosen


def lane_sqrt(src, mode, quirk=None):
    if is_nan(src):
        return src | QUIET_BIT
    src = flush_denormal(src, mode)
    sign, value = decode(src)
    if value is None:
        return DEFAULT_NAN if sign else src
    if value == 0:
        return (0 if quirk == "sqrt_negative_zero_positive" else sign) << 31
    if sign:
        return 0x7FC00000 if quirk == "sqrt_default_nan_positive" else DEFAULT_NAN
    exp = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** exp > value:
        exp -= 1
    while Fraction(2) ** (exp + 1) <= value:
        exp += 1
    root_exp = exp // 2
    ratio = value / Fraction(4) ** (root_exp - 23)
    whole = isqrt(ratio.numerator // ratio.denominator)
    inexact = Fraction(whole * whole) != ratio
    if inexact:
        rc = RN if quirk == "ignore_rounding_mode" else mode.rc
        up = (rc == RU) or (rc == RN and ratio > Fraction(whole * whole + whole) + Fraction(1, 4))
        whole += 1 if up else 0
    if whole == 1 << 24:
        whole, root_exp = 1 << 23, root_exp + 1
    return ((root_exp + 127) << 23) | (whole - (1 << 23))


def lanes(buf):
    return [int.from_bytes(buf[i:i + 4], "little") for i in range(0, 16, 4)]


def apply(name, a16, b16, mode=DEFAULT_MODE, quirk=None):
    """Four result lanes (None where undecidable) of one packed operation on 16-byte operands."""
    x, y = lanes(a16), lanes(b16)
    out = []
    for p, q in zip(x, y):
        if name == "sqrt":
            out.append(lane_sqrt(q, mode, quirk))
        elif name in ("min", "max"):
            out.append(lane_minmax(name, p, q, mode, quirk))
        else:
            out.append(lane_arith(name, p, q, mode, quirk))
    return out


def pack(result):
    """(bytes, wildcard mask) for lanes that may be None."""
    data = b"".join((0 if v is None else v).to_bytes(4, "little") for v in result)
    wild = b"".join((MASK32 if v is None else 0).to_bytes(4, "little") for v in result)
    return data, wild


def chain(a, b, mode=DEFAULT_MODE, quirk=None, multiply="mul"):
    """Lanes of the two output vectors of op_arith_pipeline (xmm3 at +0, xmm2 at +16)."""
    def step(name, p, q):
        if None in p or None in q:
            return [None] * 4
        return apply(name, pack(p)[0], pack(q)[0], mode, quirk)
    x0, x1 = lanes(a[0:16]), lanes(b[0:16])
    x0 = step("add", x0, x1)
    x0 = step(multiply, x0, lanes(b[16:32]))
    x0 = step("sub", x0, x1)
    x2 = step("div", lanes(a[16:32]), x0)
    x2 = step("min", x2, x1)
    x0 = step("max", x0, x2)
    return step("sqrt", x0, x0), x2
