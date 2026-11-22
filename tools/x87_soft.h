// x87 register stack for code lowered by tools/ffx_version_query.py.
//
// Each register holds an exact extended-precision value: a sign, a 15-bit biased exponent and a 64-bit
// significand with an explicit integer bit. Conversions round by the rounding control of the control
// word. Every exception is masked, so an invalid operation, a stack overflow or a stack underflow
// produces the indefinite NaN instead of a trap. The status word is not modelled; the lowering rejects
// the instructions that would expose it.
//
// The initial state is the one measured at a Windows x64 ctypes call: control word 0x027F (53-bit
// precision, round to nearest, all exceptions masked), every register empty, TOP = 0.
#include <intrin.h>

struct X87Value { uint64_t sig; uint16_t se; };  // se = sign << 15 | biased exponent
struct X87 {
    X87Value st[8]{};
    unsigned top = 0;
    unsigned valid = 0;   // bit p is set when physical register p is not empty
    uint16_t cw = 0x027f;
};

static const X87Value x87_indefinite{0xc000000000000000ull, 0xffff};
static const uint64_t x87_integer_bit = 0x8000000000000000ull;

static inline unsigned x87_exp(X87Value v) { return v.se & 0x7fffu; }
static inline bool x87_sign(X87Value v) { return (v.se >> 15) != 0; }
static inline unsigned x87_rc(const X87& f) { return (f.cw >> 10) & 3u; }
// A nonzero exponent with a clear integer bit (unnormal, pseudo-infinity, pseudo-NaN) is an invalid operand.
static inline bool x87_unsupported(X87Value v) { return x87_exp(v) != 0 && (v.sig & x87_integer_bit) == 0; }
static inline bool x87_is_nan(X87Value v) { return x87_exp(v) == 0x7fff && (v.sig & x87_integer_bit) && (v.sig << 1) != 0; }
static inline bool x87_is_inf(X87Value v) { return x87_exp(v) == 0x7fff && v.sig == x87_integer_bit; }
static inline bool x87_is_zero(X87Value v) { return x87_exp(v) == 0 && v.sig == 0; }
static inline X87Value x87_quiet(X87Value v) { v.sig |= 0x4000000000000000ull; return v; }

static inline int x87_clz(uint64_t v) {
    unsigned long index;
    return _BitScanReverse64(&index, v) ? 63 - static_cast<int>(index) : 64;
}

// A finite nonzero value as sig * 2^(e - 63) with the integer bit of sig set. A denormal or
// pseudo-denormal has the exponent of biased exponent 1.
struct X87Unpacked { bool sign; int e; uint64_t sig; };
static inline X87Unpacked x87_unpack(X87Value v) {
    if (x87_exp(v) != 0) return {x87_sign(v), static_cast<int>(x87_exp(v)) - 16383, v.sig};
    const int shift = x87_clz(v.sig);
    return {x87_sign(v), 1 - 16383 - shift, v.sig << shift};
}

// Shift the 128-bit hi:lo right by n, folding every bit shifted out into bit 0 (the sticky bit).
static inline void x87_shr_sticky(uint64_t& hi, uint64_t& lo, int n) {
    if (n <= 0) return;
    if (n >= 128) { lo = (hi | lo) != 0; hi = 0; return; }
    uint64_t sticky;
    if (n >= 64) {
        const int m = n - 64;
        sticky = lo != 0 || (m != 0 && (hi << (64 - m)) != 0);
        lo = m != 0 ? hi >> m : hi;
        hi = 0;
    } else {
        sticky = (lo << (64 - n)) != 0;
        lo = (lo >> n) | (hi << (64 - n));
        hi >>= n;
    }
    lo |= sticky;
}

// A destination format: significand bits, exponent bias and the largest biased exponent of a finite value.
struct X87Format { int precision, bias, emax; };
static const X87Format x87_f32{24, 127, 0xfe};
static const X87Format x87_f64{53, 1023, 0x7fe};

// Round sign * (hi:lo / 2^127) * 2^e, with hi:lo normalized (bit 127 set) and the sticky bit in bit 0 of
// lo, into a format. The result significand keeps its precision bits at the top of a 64-bit word. A
// result below the normal range is denormalized first and then rounded at the same bit positions. A
// result above it becomes infinity or the largest finite value, as the masked overflow response gives
// for the rounding direction.
struct X87Rounded { uint64_t sig; int biased; };
static inline X87Rounded x87_round(bool sign, int e, uint64_t hi, uint64_t lo, X87Format format, unsigned rc) {
    int biased = e + format.bias;
    if (biased <= 0) {
        x87_shr_sticky(hi, lo, 1 - biased);
        biased = 0;
    }
    const int drop = 64 - format.precision;
    const uint64_t unit = 1ull << drop, mask = unit - 1;
    bool above, half, inexact;
    if (drop == 0) {
        above = lo > x87_integer_bit;
        half = lo == x87_integer_bit;
        inexact = lo != 0;
    } else {
        const uint64_t rest = hi & mask, middle = unit >> 1;
        above = rest > middle || (rest == middle && lo != 0);
        half = rest == middle && lo == 0;
        inexact = rest != 0 || lo != 0;
    }
    hi &= ~mask;
    bool up = false;
    switch (rc) {
    case 0: up = above || (half && (hi & unit) != 0); break;   // to nearest, ties to even
    case 1: up = sign && inexact; break;                       // toward negative infinity
    case 2: up = !sign && inexact; break;                      // toward positive infinity
    default: break;                                            // toward zero
    }
    if (up) {
        hi += unit;
        if (hi == 0) { hi = x87_integer_bit; ++biased; }
    }
    if (biased == 0 && (hi & x87_integer_bit) != 0) biased = 1;  // a denormal rounded up to the normal range
    if (biased > format.emax) {
        const bool to_infinity = rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
        return to_infinity ? X87Rounded{x87_integer_bit, format.emax + 1} : X87Rounded{~0ull << drop, format.emax};
    }
    return {hi, biased};
}

// Loads. An m32 or m64 value converts exactly; a signaling NaN is quieted (the masked invalid response).
static inline X87Value x87_from_binary(uint64_t bits, int fraction_bits, int bias, int exponent_max) {
    const bool sign = (bits >> (fraction_bits + (fraction_bits == 23 ? 8 : 11))) & 1;
    const uint16_t s = sign ? 0x8000 : 0;
    const uint64_t fraction = bits & ((1ull << fraction_bits) - 1);
    const int exponent = static_cast<int>((bits >> fraction_bits) & static_cast<uint64_t>(exponent_max));
    if (exponent == exponent_max) {
        if (fraction == 0) return {x87_integer_bit, static_cast<uint16_t>(s | 0x7fff)};
        return x87_quiet({x87_integer_bit | (fraction << (63 - fraction_bits)), static_cast<uint16_t>(s | 0x7fff)});
    }
    if (exponent == 0) {
        if (fraction == 0) return {0, s};
        const int shift = x87_clz(fraction);  // value = fraction * 2^(1 - bias - fraction_bits)
        const int e = 1 - bias - fraction_bits + (63 - shift);
        return {fraction << shift, static_cast<uint16_t>(s | (e + 16383))};
    }
    return {x87_integer_bit | (fraction << (63 - fraction_bits)), static_cast<uint16_t>(s | (exponent - bias + 16383))};
}
static inline X87Value x87_from_f32(uint32_t bits) { return x87_from_binary(bits, 23, 127, 0xff); }
static inline X87Value x87_from_f64(uint64_t bits) { return x87_from_binary(bits, 52, 1023, 0x7ff); }
static inline X87Value x87_from_m80(uint64_t sig, uint16_t se) { return {sig, se}; }  // loaded unchanged

static inline X87Value x87_from_int(int64_t value) {
    if (value == 0) return {0, 0};
    const bool sign = value < 0;
    const uint64_t magnitude = sign ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    const int shift = x87_clz(magnitude);
    return {magnitude << shift, static_cast<uint16_t>((sign ? 0x8000 : 0) | (16383 + 63 - shift))};
}

// Stores to m32 and m64 round by the rounding control into the destination's precision and range. A NaN
// is quieted and its significand truncated; an invalid operand stores the destination's indefinite.
static inline uint64_t x87_to_binary(const X87& f, X87Value v, X87Format format, int fraction_bits) {
    const int exponent_shift = fraction_bits;
    const uint64_t sign = static_cast<uint64_t>(x87_sign(v)) << (fraction_bits == 23 ? 31 : 63);
    const uint64_t exponent_all = static_cast<uint64_t>(format.emax + 1);
    const uint64_t quiet_bit = 1ull << (fraction_bits - 1);
    const uint64_t fraction_mask = (1ull << fraction_bits) - 1;
    if (x87_unsupported(v)) return (1ull << (fraction_bits == 23 ? 31 : 63)) | (exponent_all << exponent_shift) | quiet_bit;
    if (x87_is_nan(v))
        return sign | (exponent_all << exponent_shift) | quiet_bit | ((v.sig >> (63 - fraction_bits)) & fraction_mask);
    if (x87_is_inf(v)) return sign | (exponent_all << exponent_shift);
    if (x87_is_zero(v)) return sign;
    const X87Unpacked u = x87_unpack(v);
    const X87Rounded r = x87_round(u.sign, u.e, u.sig, 0, format, x87_rc(f));
    return sign | (static_cast<uint64_t>(r.biased) << exponent_shift) | ((r.sig >> (63 - fraction_bits)) & fraction_mask);
}
static inline uint32_t x87_to_f32(const X87& f, X87Value v) { return static_cast<uint32_t>(x87_to_binary(f, v, x87_f32, 23)); }
static inline uint64_t x87_to_f64(const X87& f, X87Value v) { return x87_to_binary(f, v, x87_f64, 52); }

// FIST, FISTP and FISTTP: round by rc (FISTTP passes 3, toward zero) to a signed integer of the given width.
// A NaN, an infinity, an invalid operand or an out-of-range result stores the integer indefinite.
static inline uint64_t x87_to_int(X87Value v, unsigned rc, int bits) {
    const uint64_t indefinite = 1ull << (bits - 1);
    const uint64_t width_mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if (x87_unsupported(v) || x87_exp(v) == 0x7fff) return indefinite;
    if (x87_is_zero(v)) return 0;
    const X87Unpacked u = x87_unpack(v);
    if (u.e > 63) return indefinite;
    uint64_t magnitude;
    bool above, half, inexact;
    if (u.e < 0) {  // |v| < 1
        magnitude = 0;
        above = u.e == -1 && u.sig > x87_integer_bit;
        half = u.e == -1 && u.sig == x87_integer_bit;
        inexact = true;
    } else {
        const int shift = 63 - u.e;
        magnitude = shift != 0 ? u.sig >> shift : u.sig;
        const uint64_t rest = shift != 0 ? u.sig & ((1ull << shift) - 1) : 0;
        const uint64_t middle = shift != 0 ? 1ull << (shift - 1) : 0;
        above = shift != 0 && rest > middle;
        half = shift != 0 && rest == middle;
        inexact = rest != 0;
    }
    bool up = false;
    switch (rc) {
    case 0: up = above || (half && (magnitude & 1) != 0); break;
    case 1: up = u.sign && inexact; break;
    case 2: up = !u.sign && inexact; break;
    default: break;
    }
    if (up) ++magnitude;  // magnitude < 2^63 whenever it can round up
    if (magnitude > (u.sign ? indefinite : indefinite - 1)) return indefinite;
    return (u.sign ? 0 - magnitude : magnitude) & width_mask;
}

// FLD1, FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2 and FLDZ: the 64-bit significand truncated from the exact
// constant, its exponent, and whether the bits below are at least one half (nearest rounds up) or nonzero.
struct X87Constant { uint64_t sig; uint16_t se; bool nearest_up, inexact; };
static inline X87Value x87_constant(const X87& f, X87Constant c) {
    const unsigned rc = x87_rc(f);
    const bool up = rc == 0 ? c.nearest_up : (rc == 2 && c.inexact);  // every constant is positive
    return {c.sig + (up ? 1 : 0), c.se};
}
static const X87Constant x87_one{0x8000000000000000ull, 0x3fff, false, false};
static const X87Constant x87_zero{0, 0, false, false};
static const X87Constant x87_pi{0xc90fdaa22168c234ull, 0x4000, true, true};
static const X87Constant x87_l2e{0xb8aa3b295c17f0bbull, 0x3fff, true, true};
static const X87Constant x87_l2t{0xd49a784bcd1b8afeull, 0x4000, false, true};
static const X87Constant x87_lg2{0x9a209a84fbcff798ull, 0x3ffd, true, true};
static const X87Constant x87_ln2{0xb17217f7d1cf79abull, 0x3ffe, true, true};

// Stack access. ST(i) is physical register (TOP + i) mod 8. Reading an empty register is a stack
// underflow and pushing onto a full one a stack overflow; both produce the indefinite.
static inline unsigned x87_phys(const X87& f, unsigned i) { return (f.top + i) & 7u; }
static inline bool x87_empty(const X87& f, unsigned i) { return ((f.valid >> x87_phys(f, i)) & 1u) == 0; }
static inline X87Value x87_st(const X87& f, unsigned i) { return x87_empty(f, i) ? x87_indefinite : f.st[x87_phys(f, i)]; }
static inline void x87_set(X87& f, unsigned i, X87Value v) {
    const unsigned p = x87_phys(f, i);
    f.st[p] = v;
    f.valid |= 1u << p;
}
static inline void x87_push(X87& f, X87Value v) {
    f.top = (f.top - 1) & 7u;
    if ((f.valid >> f.top) & 1u) v = x87_indefinite;
    f.st[f.top] = v;
    f.valid |= 1u << f.top;
}
static inline void x87_pop(X87& f) {
    f.valid &= ~(1u << f.top);
    f.top = (f.top + 1) & 7u;
}
static inline void x87_free(X87& f, unsigned i) { f.valid &= ~(1u << x87_phys(f, i)); }
static inline void x87_xchg(X87& f, unsigned i) {
    const X87Value a = x87_st(f, 0), b = x87_st(f, i);
    x87_set(f, 0, b);
    x87_set(f, i, a);
}
