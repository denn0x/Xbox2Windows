"""Native/AOT differential checks and rejection controls for BSF and BSR in cpu_switch_aot.py."""

import ctypes
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_switch_aot import translate
from direct_check import GUARDS, MASK, SENTINELS, input_pairs, observe, register_probe

M32 = (1 << 32) - 1
SCANS = ("bsf", "bsr")
NAMES = ([f"{op}{bits}_{form}" for op in SCANS for bits in (16, 32, 64) for form in ("reg", "mem")]
         + ["bsr32_self", "bsf16_self", "jcc_bsf_e", "jcc_bsr_ne", "cmov_scans"])
WIDTHS = {name: 8 for name in NAMES}
# The exports that the SWAP_SCANS build assembles with the other scan, and the scan they run there.
SWAPPED = {"bsf32_reg": "bsr", "bsf64_mem": "bsr", "bsr16_reg": "bsf"}
# Destination values that a zero source must leave in place, in every bit.
DESTINATIONS = (0, 0xa5a5a5a5a5a5a5a5, 0xdeadbeefcafebabe, MASK)

REJECTIONS = (
    ("neg_bsf_setb", "only ZF is defined after BSF"),
    ("neg_bsr_seta", "only ZF is defined after BSR"),
    ("neg_bsf_cmovae", "only ZF is defined after BSF"),
    ("neg_bsr_sets", "only ZF is defined after BSR"),
    ("neg_bsf_setl", "only ZF is defined after BSF"),
    ("neg_bsr_seto", "only ZF is defined after BSR"),
    ("neg_bsf_setp", "only ZF is defined after BSF"),
    ("neg_bsr_cmovg", "only ZF is defined after BSR"),
    ("neg_bsf_undef_dest", "undefined register rax"),
    ("neg_bsf_partial_dest", "undefined register rax"),
    ("neg_bsr_undef_src", "undefined register r10"),
)


def scan(op, bits, x, y):
    """(destination, ZF) after BSF or BSR of the low bits of y into the low bits of x.

    A zero source sets ZF and leaves the whole destination register as it was, 32-bit destinations
    included. AMD documents that; the Intel SDM calls the destination undefined. The model is what
    the differential run measures, and the comparison with the original shows where a host differs.
    """
    y &= (1 << bits) - 1
    if y == 0:
        return x, True
    index = (y & -y).bit_length() - 1 if op == "bsf" else y.bit_length() - 1
    if bits == 16:
        return (x & ~0xffff | index) & MASK, False
    return index, False


def flag_word(zero):
    """The first trace word: SETE and SETNE bytes, then the bytes the harness preset to 0xff."""
    return int(zero) | int(not zero) << 8 | 0xffffffffffff0000


def reference(name, x, y, swapped=False):
    if name == "jcc_bsf_e":
        result, zero = scan("bsf", 64, x, y)
        return result ^ MASK if zero else result, (GUARDS[0], MASK, MASK, MASK, GUARDS[1])
    if name == "jcc_bsr_ne":
        result, zero = scan("bsr", 32, x, y)
        return result if zero else result ^ MASK, (GUARDS[0], MASK, MASK, MASK, GUARDS[1])
    if name == "cmov_scans":
        highest = scan("bsr", 64, y, x)[0]
        first = y if x == 0 else x
        second = highest & M32 if x & M32 else y & M32
        return y, (GUARDS[0], first, second, highest, GUARDS[1])
    operation, form = name.split("_")
    op, bits = operation[:3], int(operation[3:])
    if swapped:
        op = SWAPPED[name]
    if form == "self":
        result, zero = scan(op, bits, y, y)
        return result, (GUARDS[0], flag_word(zero), MASK, result, GUARDS[1])
    result, zero = scan(op, bits, x, y)
    if form == "reg":
        return result, (GUARDS[0], flag_word(zero), MASK, result, GUARDS[1])
    return result, (GUARDS[0], flag_word(zero), MASK, y, GUARDS[1])


def scan_inputs():
    values = {0, 1, 2, 3, 0x8000, 0x7fff, 0xffff, 0x10000, 0x18000, 0x7fffffff, 0x80000000, 0xffffffff,
              0x100000000, 0x180000000, 0x8000000000000000, 0x7fffffffffffffff, MASK, 0x123456789abcdef0,
              0xdeadbeefcafebabe, 0xffff0000, 0xffff00000000, 0xffffffff00000000, 0xffffffffffff0000,
              0x0001000000010000, 0xa5a5a5a5a5a5a5a5}
    values |= {1 << bit for bit in range(64)} | {MASK ^ (1 << bit) for bit in range(64)}
    values |= {(1 << bit) - 1 for bit in range(1, 65)} | {MASK << bit & MASK for bit in range(64)}
    pairs = [(x, v) for v in sorted(values) for x in DESTINATIONS]
    rng = random.Random(0x42534653)
    pairs.extend((rng.getrandbits(64), rng.getrandbits(64)) for _ in range(256))
    # Sources whose low 16 or 32 bits are zero but whose upper bits are not, and the reverse.
    pairs.extend((rng.getrandbits(64), rng.getrandbits(48) << 16) for _ in range(128))
    pairs.extend((rng.getrandbits(64), rng.getrandbits(32) << 32) for _ in range(128))
    return pairs + input_pairs()


def check_generated(original_path):
    source, functions = translate(original_path, NAMES)
    widths = {name: fn.return_width for name, fn in zip(NAMES, functions)}
    if widths != WIDTHS:
        raise AssertionError(f"return widths {widths} != {WIDTHS}")
    for name in (f"{op}{bits}_mem" for op in SCANS for bits in (16, 32, 64)):
        body = source.split(f"lifted_{name}(", 1)[1].split("\n}", 1)[0]
        if body.count("load_always<") != 1:
            raise AssertionError(f"expected one volatile read of the source in {name}:\n{body}")


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


def check_swapped(probe, lifted, swapped_lifted, pairs):
    """The build that exchanges BSF and BSR must follow the exchanged reference and differ from the original."""
    for name in SWAPPED:
        original, altered = getattr(lifted, "lifted_" + name), getattr(swapped_lifted, "lifted_" + name)
        differing = 0
        for x, y in pairs:
            want, trace = reference(name, x, y, swapped=True)
            actual = observe(probe, altered, x, y)
            if actual != (want, trace, SENTINELS):
                raise AssertionError(f"swapped lifted_{name}({x:#x}, {y:#x}): {actual} != {want:#x} {trace}")
            differing += observe(probe, original, x, y) != actual
        if differing == 0:
            raise AssertionError(f"swapping BSF and BSR did not change {name}")
        print(f"swapped {name}: {differing} of {len(pairs)} inputs differ from the original and match the exchanged reference")


def check(probe_path, original_path, lifted_path, swapped_lifted_path):
    check_generated(original_path)
    check_rejections(original_path)
    probe = register_probe(probe_path)
    native, lifted = ctypes.CDLL(original_path), ctypes.CDLL(lifted_path)
    pairs = scan_inputs()
    total = 0
    for name in NAMES:
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
    check_swapped(probe, lifted, ctypes.CDLL(swapped_lifted_path), pairs)
    print(f"{total} differential comparisons passed for BSF and BSR at 16/32/64 bits")


if __name__ == "__main__":
    check(*sys.argv[1:])
