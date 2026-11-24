"""Differential checks and rejection controls for jump tables behind an inverted unsigned bound check.

The source fixture branches to the dispatch block when the index is in range (JB or JBE) and a JMP to
the default follows the branch. Each export is run natively, through the translation and through a
translation of a build with two table entries exchanged, against a reference written here.
"""

import ctypes
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Decoder, FlowControl, Mnemonic

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_switch_aot import translate
from cpu_leaf_aot import export_rva

MASK32 = 0xffffffff
GUARDS = (0x5a5a1234, 0xa5a54321)
SENTINEL = 0x0badf00d
NO_STORE = None


def signed32(value):
    return value - (1 << 32) if value >> 31 else value


def rva_case(case, value):
    return (value + 11, value ^ 0x5a5a, value * 3, value - 2, value << 4)[case]


def rel_case(case, value):
    return (value * 5, value - 3, value ^ 0xc3c3, value >> 3, value ^ 0xc3c3, -value)[case]


def window_case(case, value):
    return (value + 0x1234, value - 0x10, value ^ 0xff00ff, value * 7, value << 1,
            signed32(value) >> 2, value - 0x10)[case]


# name: (compare width in bits, number of cases, case function, swapped pair, default function)
EXPORTS = {
    "inv_rva_jbe": (32, 5, rva_case, (1, 4), lambda value: ~value),
    "inv_rel_jb": (64, 6, rel_case, (1, 5), NO_STORE),
    "inv_rel_window_jbe": (32, 7, window_case, (0, 5), lambda value: value + 77),
}


def reference(name, selector, operand, swapped=False):
    """The returned RAX and the word stored at R8 (None when the export stores nothing)."""
    bits, count, case_of, pair, default = EXPORTS[name]
    value = operand & MASK32
    compared = selector & ((1 << bits) - 1)
    if compared < count:
        case = {pair[0]: pair[1], pair[1]: pair[0]}.get(compared, compared) if swapped else compared
        stored = case_of(case, value) & MASK32
    elif default is NO_STORE:
        return MASK32, None
    else:
        stored = default(value) & MASK32
    return (selector + stored) & MASK32, stored


def inputs(count):
    """Every case, the first selectors past the table and values at the 32-bit edges, with upper halves
    that neither the 32-bit compares nor any operand may use."""
    selectors = [*range(count + 3), 0x7fffffff, 0x80000000, MASK32 - 1, MASK32]
    operands = [0, 1, 2, 0x5a5a, 0x7fffffff, 0x80000000, MASK32]
    rng = random.Random(0x1b0)
    operands += [rng.getrandbits(32) for _ in range(8)]
    noise = ((0, 0), (1 << 32, 0), (0xdeadbeef << 32, 0xdeadbeef << 32))
    return [(s | selector_high, v | operand_high)
            for selector_high, operand_high in noise for s in selectors for v in operands]


def invoker(probe, library, prefix):
    from direct_check import Word

    def invoke(name, selector, operand):
        words = (ctypes.c_uint32 * 3)(GUARDS[0], SENTINEL, GUARDS[1])
        output = ctypes.cast(ctypes.byref(words, 4), ctypes.POINTER(Word))
        registers = (Word * 2)()
        rax = probe(getattr(library, prefix + name), selector, operand, output, registers)
        return rax, tuple(words), tuple(registers)
    return invoke


def observe(invoke, name, swapped=False):
    """Run every input; compare the return, all three words (the guards must stay) and RBX/RSI."""
    from direct_check import SENTINELS
    rows = []
    for selector, operand in inputs(EXPORTS[name][1]):
        rax, words, registers = invoke(name, selector, operand)
        want, stored = reference(name, selector, operand, swapped)
        expected = (GUARDS[0], SENTINEL if stored is None else stored, GUARDS[1])
        if (rax, words) != (want, expected) or registers != SENTINELS:
            raise AssertionError(f"{name}({selector:#x}, {operand:#x}): {rax:#x} {words} {registers}, "
                                 f"expected {want:#x} {expected}")
        rows.append((rax, words))
    return rows


def check(probe_path, original_path, lifted_path, swapped_path):
    from direct_check import register_probe
    probe = register_probe(probe_path)
    native, lifted, swapped = map(ctypes.CDLL, (original_path, lifted_path, swapped_path))
    total = 0
    for name, (_, count, _, pair, _) in EXPORTS.items():
        original = observe(invoker(probe, native, ""), name)
        if observe(invoker(probe, lifted, "lifted_"), name) != original:
            raise AssertionError(f"{name}: translation differs from the original")
        # The build with two entries exchanged must follow the exchanged reference, and only the
        # selectors of that pair may change.
        control = observe(invoker(probe, swapped, "lifted_"), name, swapped=True)
        changed = {selector & MASK32 for (selector, _), a, b in zip(inputs(count), original, control) if a != b}
        if changed != set(pair):
            raise AssertionError(f"{name}: the exchanged table changed selectors {sorted(changed)}, not {pair}")
        total += len(original)
        print(f"{name}: {len(original)} inputs match the original and the reference; "
              f"the exchanged table changed selectors {sorted(changed)} only")
    print(f"{total} native and translated comparisons passed")


def instructions(pe, name):
    entry = export_rva(pe, name)
    found = []
    for ins in Decoder(64, pe.get_data(entry, 0x100), ip=entry):
        found.append(ins)
        if ins.flow_control == FlowControl.INDIRECT_BRANCH:
            return entry, found
    raise AssertionError(f"no table dispatch in {name}")


def check_rejections(original_path):
    """Damaged copies of the fixture must be refused, and the fixture itself translated."""
    data = Path(original_path).read_bytes()
    names = list(EXPORTS)
    translate(original_path, names)
    controls = []
    with pefile.PE(data=data, fast_load=True) as pe:
        offset = pe.get_offset_from_rva
        for name in names:
            _, code = instructions(pe, name)
            bound = next(i for i in code if i.flow_control == FlowControl.CONDITIONAL_BRANCH)
            default_jump = code[code.index(bound) + 1]
            first = next(i for i in code if i.ip == bound.near_branch_target)
            second = code[code.index(first) + 1]
            dispatch = code[-1]
            after = Decoder(64, pe.get_data(dispatch.next_ip, 0x10), ip=dispatch.next_ip)
            case_jump = [after.decode(), after.decode()][1]
            if default_jump.mnemonic != Mnemonic.JMP or case_jump.mnemonic != Mnemonic.JMP or bound.len not in (2, 6):
                raise AssertionError(f"unexpected {name} encoding")

            def bump(ins, delta):
                raw = bytearray(data[offset(ins.ip):offset(ins.ip) + ins.len])
                raw[0 if ins.len == 2 else 1] += delta
                return bytes(raw)

            def branch(ins, target):
                raw = data[offset(ins.ip):offset(ins.ip) + ins.len]
                if ins.len == 2:
                    return raw[:1] + struct.pack("b", target - ins.next_ip)
                return raw[:-4] + struct.pack("<i", target - ins.next_ip)

            limit = "not an unsigned upper limit"
            controls += [
                # Entering the table on the wrong side of the limit, or on a signed compare.
                (name, [(bound.ip, bump(bound, 1))], limit),
                (name, [(bound.ip, bump(bound, 8))], limit),
                # Without the JMP the check falls through, where only JA and JAE bound the index.
                (name, [(default_jump.ip, b"\x90" * default_jump.len)], limit),
                # The JMP to the default now goes into the block: the default is unreachable.
                (name, [(default_jump.ip, branch(default_jump, first.ip))], "second entry into the dispatch block"),
                # A case that jumps back to the top of the block.
                (name, [(case_jump.ip, branch(case_jump, first.ip))], "second entry into the dispatch block"),
            ]
            if second.mnemonic != Mnemonic.MOVSXD:
                # The check branches past the first instruction of the block, which the JMP to the default
                # still reaches: the top of the block is no longer what the check enters.
                controls.append((name, [(bound.ip, branch(bound, second.ip)),
                                        (default_jump.ip, branch(default_jump, first.ip))],
                                 "unbounded jump-table index"))
            if name == "inv_rva_jbe":
                # The index is no longer the compared value.
                controls.append((name, [(first.ip, bytes.fromhex("4c 8b c9"))], "is not the bounded value"))
            if name == "inv_rel_window_jbe":
                controls.append((name, [(first.ip, bytes.fromhex("89 d1"))], "is not the bounded value"))
        controls = [(name, [(offset(rva), patch) for rva, patch in patches], reason)
                    for name, patches, reason in controls]
    with TemporaryDirectory() as folder:
        path = Path(folder) / "control.dll"
        for name, patches, reason in controls:
            damaged = bytearray(data)
            for start, patch in patches:
                damaged[start:start + len(patch)] = patch
            path.write_bytes(damaged)
            try:
                translate(path, [name])
            except ValueError as exc:
                if reason not in str(exc):
                    raise AssertionError(f"{name}: {reason}: {exc}") from exc
            else:
                raise AssertionError(f"{name} accepted control: {reason}")
    print(f"{len(controls)} rejection controls passed")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "rejections":
        check_rejections(sys.argv[2])
    else:
        check(*sys.argv[1:])
        check_rejections(sys.argv[2])
