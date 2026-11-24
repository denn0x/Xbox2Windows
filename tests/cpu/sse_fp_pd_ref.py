"""Independent double-precision reference for the packed SSE2 arithmetic checks.

Exact rational arithmetic (fractions.Fraction) with the four MXCSR rounding modes, DAZ and FTZ, written
without any host floating-point instruction and without the translator, so the processor and the lifted
code are both compared with arithmetic they cannot share. The control word model (Mode) is the one of
sse_fp_ref.py. A lane the reference cannot decide is None and stays None through later operations:
  - FTZ, when the exact result is below the smallest normal number but rounds up to it (the manuals leave
    open whether tininess is judged before or after rounding);
  - MINPD and MAXPD under DAZ when the returned operand is a denormal (whether the hardware hands back
    the original bits or a flushed zero).
"""

from fractions import Fraction
from math import isqrt

from sse_fp_ref import ALL_MODES, DEFAULT_MODE, RD, RN, RU, RZ, Mode  # noqa: F401  (re-exported)

MASK64 = 0xFFFFFFFFFFFFFFFF
SIGN = 1 << 63
INFINITY = 0x7FF0000000000000
DEFAULT_NAN = 0xFFF8000000000000
QUIET_BIT = 0x0008000000000000
FRACTION_BITS = 52
MIN_EXP = -1022
MIN_NORMAL = Fraction(2) ** MIN_EXP
HUGE = Fraction(10 ** 400)


def is_nan(bits):
    return (bits & ~SIGN) > INFINITY


def is_denormal(bits):
    return (bits & INFINITY) == 0 and (bits & (QUIET_BIT * 2 - 1)) != 0


def flush_denormal(bits, mode):
    return bits & SIGN if mode.daz and is_denormal(bits) else bits


def decode(bits):
    """(sign, None) for an infinity, (sign, Fraction) for a finite value (zero included)."""
    sign, exp, frac = bits >> 63, (bits >> 52) & 0x7FF, bits & (QUIET_BIT * 2 - 1)
    if exp == 0x7FF:
        return sign, None
    if exp == 0:
        return sign, Fraction(frac, 2 ** 1074)
    return sign, Fraction(frac | (1 << 52)) * Fraction(2) ** (exp - 1075)


def magnitude_key(bits):
    sign, value = decode(bits)
    return -HUGE if sign and value is None else HUGE if value is None else (-value if sign else value)


def binary_exponent(value):
    """The e with 2**e <= value < 2**(e + 1) for a positive Fraction."""
    exp = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** exp > value:
        exp -= 1
    while Fraction(2) ** (exp + 1) <= value:
        exp += 1
    return exp


def round_exact(value, mode, quirk=None):
    """Round a nonzero exact value to binary64 bits, or None when the lane is undecidable."""
    rc = RN if quirk == "ignore_rounding_mode" else mode.rc
    sign = 1 if value < 0 else 0
    mag = abs(value)
    exp = max(binary_exponent(mag), MIN_EXP)
    scaled = mag / Fraction(2) ** (exp - FRACTION_BITS)
    whole = scaled.numerator // scaled.denominator
    rest = scaled - whole
    if rest and (rc == RN and (rest > Fraction(1, 2) or (rest == Fraction(1, 2) and whole & 1))
                 or rc == RD and sign or rc == RU and not sign):
        whole += 1
    if mag < MIN_NORMAL and mode.ftz and quirk != "ignore_ftz":
        return None if whole == 1 << FRACTION_BITS else sign << 63
    if whole == 1 << (FRACTION_BITS + 1):
        whole, exp = 1 << FRACTION_BITS, exp + 1
    if whole == 0:
        return sign << 63
    biased = exp + 1023 if whole >= 1 << FRACTION_BITS else 0
    if biased >= 0x7FF:
        to_infinity = rc == RN or (rc == RD and sign) or (rc == RU and not sign)
        return (sign << 63) | (INFINITY if to_infinity else INFINITY - 1)
    return (sign << 63) | (biased << 52) | (whole - (1 << FRACTION_BITS) if biased else whole)


def nan_source(a, b, quirk):
    first, second = (b, a) if quirk == "nan_second_source" else (a, b)
    if is_nan(first):
        return first if quirk == "no_quieting" else first | QUIET_BIT
    if is_nan(second):
        return second if quirk == "no_quieting" else second | QUIET_BIT
    return None


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
            return (sa if x is None else sb) << 63 | INFINITY
        total = (-x if sa else x) + (-y if sb else y)
        if total == 0:
            if x == 0 and y == 0 and sa == sb:
                return sa << 63
            return (1 if quirk == "mixed_zero_negative" else 1 if mode.rc == RD else 0) << 63
        return round_exact(total, mode, quirk)
    sign = sa ^ sb
    if op == "mul":
        if x is None or y is None:
            return DEFAULT_NAN if x == 0 or y == 0 else sign << 63 | INFINITY
        return round_exact((-1 if sign else 1) * x * y, mode, quirk) if x * y else sign << 63
    if x is None:
        return DEFAULT_NAN if y is None else sign << 63 | INFINITY
    if y is None:
        return sign << 63
    if y == 0:
        if quirk == "divide_zero_nan":
            return DEFAULT_NAN
        return DEFAULT_NAN if x == 0 else sign << 63 | INFINITY
    return round_exact((-1 if sign else 1) * x / y, mode, quirk) if x else sign << 63


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
        return (0 if quirk == "sqrt_negative_zero_positive" else sign) << 63
    if sign:
        return 0x7FF8000000000000 if quirk == "sqrt_default_nan_positive" else DEFAULT_NAN
    root_exp = binary_exponent(value) // 2
    ratio = value / Fraction(4) ** (root_exp - FRACTION_BITS)
    whole = isqrt(ratio.numerator // ratio.denominator)
    if Fraction(whole * whole) != ratio:
        rc = RN if quirk == "ignore_rounding_mode" else mode.rc
        if rc == RU or (rc == RN and ratio > Fraction(whole * whole + whole) + Fraction(1, 4)):
            whole += 1
    if whole == 1 << (FRACTION_BITS + 1):
        whole, root_exp = 1 << FRACTION_BITS, root_exp + 1
    return ((root_exp + 1023) << 52) | (whole - (1 << FRACTION_BITS))


def cmp_lane(x, y, pred, mode=DEFAULT_MODE, quirk=None):
    """The 64-bit mask CMPPD predicate 0..7 (EQ LT LE UNORD NEQ NLT NLE ORD) gives two bit patterns.

    Written from the rules of the predicates, not from a host comparison: DAZ reads a denormal as a
    zero of the same sign, any NaN makes the pair unordered (EQ LT LE false, UNORD NEQ NLT NLE true,
    ORD false), and two ordered values compare by exact value so that +0 equals -0. FTZ plays no part:
    a compare does not produce a number."""
    if quirk != "ignore_daz":
        x, y = flush_denormal(x, mode), flush_denormal(y, mode)
    unordered = (is_nan(x) or is_nan(y)) and quirk != "nan_ordered"
    kx, ky = (magnitude_key(x), magnitude_key(y)) if not (is_nan(x) or is_nan(y)) else (0, 0)
    equal, less = (not unordered) and kx == ky, (not unordered) and kx < ky
    if quirk == "zero_signed" and kx == ky == 0 and x != y:
        equal, less = False, bool(x >> 63)
    if quirk == "eq_for_neq" and pred == 4:
        pred = 0
    if quirk == "lt_for_le" and pred == 2:
        pred = 1
    if quirk == "swap_ord" and pred in (3, 7):
        pred ^= 4
    result = (
        equal, less, less or equal, unordered, not equal, not less, not (less or equal), not unordered,
    )[pred]
    if quirk == "numeric":
        return int(result)
    return (0xFFFFFFFF if quirk == "mask32" else MASK64) if result else 0


def compare(a16, b16, pred, mode=DEFAULT_MODE, quirk=None):
    """Two mask lanes of one packed double compare on 16-byte operands."""
    return [cmp_lane(p, q, pred, mode, quirk) for p, q in zip(lanes(a16), lanes(b16))]


def lanes(buf):
    return [int.from_bytes(buf[i:i + 8], "little") for i in range(0, 16, 8)]


def apply(name, a16, b16, mode=DEFAULT_MODE, quirk=None):
    """Two result lanes (None where undecidable) of one packed operation on 16-byte operands."""
    out = []
    for p, q in zip(lanes(a16), lanes(b16)):
        if name == "sqrt":
            out.append(lane_sqrt(q, mode, quirk))
        elif name in ("min", "max"):
            out.append(lane_minmax(name, p, q, mode, quirk))
        else:
            out.append(lane_arith(name, p, q, mode, quirk))
    return out


def pack(result):
    """(bytes, wildcard mask) for lanes that may be None."""
    data = b"".join((0 if v is None else v).to_bytes(8, "little") for v in result)
    wild = b"".join((MASK64 if v is None else 0).to_bytes(8, "little") for v in result)
    return data, wild


def chain(a, b, mode=DEFAULT_MODE, quirk=None, multiply="mul"):
    """Lanes of the two output vectors of op_pd_pipeline (xmm3 at +0, xmm2 at +16)."""
    def step(name, p, q):
        if None in p or None in q:
            return [None] * 2
        return apply(name, pack(p)[0], pack(q)[0], mode, quirk)
    x0, x1 = lanes(a[0:16]), lanes(b[0:16])
    x0 = step("add", x0, x1)
    x0 = step(multiply, x0, lanes(b[16:32]))
    x0 = step("sub", x0, x1)
    x2 = step("div", lanes(a[16:32]), x0)
    x2 = step("min", x2, x1)
    x0 = step("max", x0, x2)
    return step("sqrt", x0, x0), x2
