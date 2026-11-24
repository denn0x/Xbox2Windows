"""Native/AOT differential checks and rejection controls for BSWAP in cpu_switch_aot.py."""

import ctypes
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_switch_aot import translate
from direct_check import GUARDS, MASK, SENTINELS, input_pairs, observe, register_probe

M32 = (1 << 32) - 1
ALL = "o no b ae e ne be a s ns p np l ge le g".split()
REGISTERS = ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15")
NAMES = [f"bswap{bits}_{register}" for register in REGISTERS for bits in (32, 64)]
EXTRA = ("bswap_mixed", "jcc_bswap_b", "cmov_bswap")
WIDTHS = {name: 8 for name in (*NAMES, *EXTRA)}

REJECTIONS = (
    ("neg_bswap16", "the 16-bit form is undefined"),
    ("neg_bswap_rep", "unsupported prefix"),
    ("neg_bswap_undef", "undefined register r10"),
    ("neg_bswap_rsp", "dynamic frame"),
)

# Faults in the lowering, each as the reference it would have to match. The words of an observation are
# 0 = RAX, 1 and 5 = the guards, 2 and 3 = the conditions, 4 = the register after BSWAP. A fault must
# disagree with the native run in exactly the words listed and in no other.
MUTATIONS = {
    "swap_32_bits_for_64": {0, 4},
    "keep_high_half_for_32": {0, 4},
    "swap_16_bit_words": {0, 4},
    "destination_unswapped": {0, 4},
    "flags_from_result": {2, 3},
}


def swap_bytes(value, size):
    """The bytes of value, reversed; int.to_bytes and int.from_bytes keep it independent of shifts."""
    return int.from_bytes(value.to_bytes(size, "little"), "big")


def conditions(x, y):
    """The sixteen conditions of CMP x, y from signed and unsigned comparison, not from the flags."""
    signed_x, signed_y = x - (x >> 63 << 64), y - (y >> 63 << 64)
    difference = (x - y) & MASK
    overflow = not -(1 << 63) <= signed_x - signed_y < 1 << 63
    sign = bool(difference >> 63)
    parity = bin(difference & 0xff).count("1") % 2 == 0
    base = {"o": overflow, "b": x < y, "e": x == y, "be": x <= y, "s": sign, "p": parity,
            "l": signed_x < signed_y, "le": signed_x <= signed_y}
    values = dict(base)
    for negated, name in (("no", "o"), ("ae", "b"), ("ne", "e"), ("a", "be"), ("ns", "s"), ("np", "p"),
                          ("ge", "l"), ("g", "le")):
        values[negated] = not base[name]
    return values


def flag_words(values):
    data = bytes(int(values[name]) for name in ALL)
    return int.from_bytes(data[:8], "little"), int.from_bytes(data[8:], "little")


def result_register(bits, y, mutation):
    if mutation == "swap_32_bits_for_64" and bits == 64:
        return y & ~M32 | swap_bytes(y & M32, 4)
    if mutation == "keep_high_half_for_32" and bits == 32:
        return y & ~M32 | swap_bytes(y & M32, 4)
    if mutation == "swap_16_bit_words":
        words = (y & ((1 << bits) - 1)).to_bytes(bits // 8, "little")
        return int.from_bytes(b"".join(words[n:n + 2] for n in range(bits // 8 - 2, -2, -2)), "little")
    if mutation == "destination_unswapped":
        return y & ((1 << bits) - 1)
    return swap_bytes(y & M32, 4) if bits == 32 else swap_bytes(y, 8)


def reference(name, x, y, mutation=None):
    if name == "bswap_mixed":
        return swap_bytes(swap_bytes(y, 8) & M32, 4), (GUARDS[0], MASK, MASK, MASK, GUARDS[1])
    if name == "jcc_bswap_b":
        result = swap_bytes(y, 8)
        return result ^ MASK if conditions(x, y)["b"] else result, (GUARDS[0], MASK, MASK, MASK, GUARDS[1])
    if name == "cmov_bswap":
        result = swap_bytes(x & M32, 4)
        return swap_bytes(y, 8) if conditions(x, y)["l"] else result, (GUARDS[0], MASK, MASK, MASK, GUARDS[1])
    bits = int(name[5:7])
    result = result_register(bits, y, mutation)
    flags = conditions(x, y)
    if mutation == "flags_from_result":
        # TEST result, result instead of the unchanged flags of CMP x, y.
        sign, zero = bool(result >> 63), result == 0
        flags = {"o": False, "b": False, "e": zero, "be": zero, "s": sign, "p": bin(result & 0xff).count("1") % 2 == 0,
                 "l": sign, "le": sign or zero}
        for negated, base in (("no", "o"), ("ae", "b"), ("ne", "e"), ("a", "be"), ("ns", "s"), ("np", "p"),
                              ("ge", "l"), ("g", "le")):
            flags[negated] = not flags[base]
    low, high = flag_words(flags)
    return result, (GUARDS[0], low, high, result, GUARDS[1])


def bswap_inputs():
    values = {0, 1, 0x80, 0xff, 0x100, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
              0x0123456789abcdef, 0xefcdab8967452301, MASK, 0xff00000000000000, 0x00ff00ff00ff00ff,
              0x8000000000000000, 0x7fffffffffffffff, 0x00000000ff000000, 0x000000ff00000000}
    values |= {0xff << (8 * byte) for byte in range(8)} | {(1 << bit) for bit in range(64)}
    rng = random.Random(0x42535741)
    pairs = [(x, y) for y in sorted(values) for x in (0, 0x80, 0x0123456789abcdef, MASK)]
    pairs.extend((rng.getrandbits(64), rng.getrandbits(64)) for _ in range(256))
    return pairs + input_pairs()


def check_generated(original_path):
    _, functions = translate(original_path, [*NAMES, *EXTRA])
    widths = {name: fn.return_width for name, fn in zip((*NAMES, *EXTRA), functions)}
    if widths != WIDTHS:
        raise AssertionError(f"return widths {widths} != {WIDTHS}")


def check_rejections(original_path):
    for name, reason in REJECTIONS:
        try:
            translate(original_path, [name])
        except ValueError as exc:
            if reason not in str(exc):
                raise AssertionError(f"{name}: expected {reason!r}, got {exc}") from exc
            print(f"rejected {name}: {exc}")
        else:
            raise AssertionError(f"accepted negative control {name}")
    print(f"{len(REJECTIONS)} rejection controls passed")


def check_mutations(probe, native, pairs):
    """Each fault, run as a reference, must disagree with the native fixture in its own words only."""
    for mutation, expected in MUTATIONS.items():
        differing = set()
        for name in NAMES:
            for x, y in pairs:
                result, trace = reference(name, x, y, mutation)
                original = observe(probe, getattr(native, name), x, y)
                words = (original[0], *original[1])
                differing |= {n for n, (a, b) in enumerate(zip(words, (result, *trace))) if a != b}
        if differing != expected:
            raise AssertionError(f"{mutation}: words {sorted(differing)} differ, expected {sorted(expected)}")
        print(f"mutation {mutation}: words {sorted(differing)} disagree with the original")


def check(probe_path, original_path, lifted_path):
    check_generated(original_path)
    check_rejections(original_path)
    probe = register_probe(probe_path)
    native, lifted = ctypes.CDLL(original_path), ctypes.CDLL(lifted_path)
    pairs = bswap_inputs()
    total = 0
    for name in (*NAMES, *EXTRA):
        source, translation = getattr(native, name), getattr(lifted, "lifted_" + name)
        for x, y in pairs:
            want, trace = reference(name, x, y)
            original, actual = observe(probe, source, x, y), observe(probe, translation, x, y)
            if original != (want, trace, SENTINELS):
                raise AssertionError(f"original {name}({x:#x}, {y:#x}): {original} != {want:#x} {trace}")
            if actual != original:
                raise AssertionError(f"lifted_{name}({x:#x}, {y:#x}): {actual} != {original}")
            total += 1
        print(f"{name}: {len(pairs)} inputs match native and reference")
    check_mutations(probe, native, pairs[::7])
    print(f"{total} differential comparisons passed for BSWAP at 32 and 64 bits in 15 registers")


if __name__ == "__main__":
    check(*sys.argv[1:])
