"""v_mov_b32_sdwa: the selected source field, placed by dst_sel / dst_unused.

Synthetic VOP1 SDWA words (VOP1 op 1, src0 = 0xf9; SDWA dword: [7:0] source, [10:8] dst_sel, [12:11] dst_unused,
[18:16] src0_sel, [19] sext, [23] source is an SGPR). Reference (AMD RDNA 2 ISA, SDWA section; LLVM
VOP1Instructions.td gives V_MOV_B32 an SDWA form with integer input modifiers, sext only): the source field at
src0_sel is zero- or sign-extended to 32 bits; dst_sel DWORD writes it; a partial dst_sel writes its low bits into
that field and the rest of the register is 0 (UNUSED_PAD) or kept (UNUSED_PRESERVE).

The emitted expression is evaluated in Python for every src0_sel x dst_sel x {PAD, PRESERVE} x sext on a value
grid, against a reference written here from those rules. An SGPR source is accepted. Refused: UNUSED_SEXT (no
lowering), the neg bit (not an integer input modifier; the decoder flags it). Negative control: a reference that
ignores sext (or PRESERVE) is caught.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_sdwa_cvt_check import one  # noqa: E402

SEL = {0: (0, 8), 1: (8, 8), 2: (16, 8), 3: (24, 8), 4: (0, 16), 5: (16, 16), 6: (0, 32)}
GRID = [0, 0x80, 0x7f, 0x8000, 0xffffffff, 0x12345678, 0x89abcdef, 0x00ff00ff]
OLD = 0xa5a55a5a


def words(dsel, unused, ssel, sext=0, neg=0, sgpr=0, src=1):
    return [0x7e000000 | 2 << 17 | 1 << 9 | 0xf9,
            src | dsel << 8 | unused << 11 | ssel << 16 | sext << 19 | neg << 20 | sgpr << 23]


def s32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >> 31 else x


def evaluate(body, v1, v2):
    m = re.fullmatch(r'v2 = (.*);', body)
    assert m, body
    expr = re.sub(r'(0x[0-9a-f]+|\d+)u\b', r'\1', m.group(1)).replace('asuint(', 'u32(').replace('asint(', 's32(')
    return eval(expr, {'u32': lambda x: x & 0xffffffff, 's32': s32, '__builtins__': {}},
                {'v1': v1, 'v2': v2, 's3': v1}) & 0xffffffff


def reference(v1, old, dsel, unused, ssel, sext, ignore_sext=False, ignore_preserve=False):
    off, width = SEL[ssel]
    field = (v1 >> off) & ((1 << width) - 1)
    if sext and not ignore_sext and width < 32 and field >> (width - 1):
        field |= ~((1 << width) - 1) & 0xffffffff
    doff, dwidth = SEL[dsel]
    if dwidth == 32:
        return field
    placed = (field & ((1 << dwidth) - 1)) << doff
    if unused == 2 and not ignore_preserve:
        placed |= old & ~(((1 << dwidth) - 1) << doff) & 0xffffffff
    return placed


def main():
    forms = caught_sext = caught_preserve = 0
    for ssel in SEL:
        for dsel in SEL:
            for unused in (0, 2):
                for sext in (0, 1):
                    text, body = one(words(dsel, unused, ssel, sext))
                    assert text.startswith('v_mov_b32_sdwa v2, %sv1' % ('sext(' if sext else '')), text
                    for v in GRID:
                        want = reference(v, OLD, dsel, unused, ssel, sext)
                        assert evaluate(body, v, OLD) == want, (text, body, hex(v))
                    caught_sext += any(evaluate(body, v, OLD) != reference(v, OLD, dsel, unused, ssel, sext, True)
                                       for v in GRID)
                    caught_preserve += any(evaluate(body, v, OLD) != reference(v, OLD, dsel, unused, ssel, sext,
                                                                               ignore_preserve=True) for v in GRID)
                    forms += 1
    assert caught_sext > 0 and caught_preserve > 0, (caught_sext, caught_preserve)
    text, body = one(words(4, 0, 0, sgpr=1, src=3))
    assert text.startswith('v_mov_b32_sdwa v2, s3'), text
    assert all(evaluate(body, v, OLD) == reference(v, OLD, 4, 0, 0, 0) for v in GRID), body
    for label, w, why in (('UNUSED_SEXT', words(0, 1, 4), 'UNUSED_SEXT'), ('neg', words(6, 0, 1, neg=1), 'SDWA')):
        try:
            one(w)
        except G.Unsupported as error:
            assert why in str(error), (label, str(error))
        else:
            raise AssertionError('accepted: ' + label)
    print('sdwa_mov: %d v_mov_b32_sdwa forms match the SDWA rules on %d values (sext caught in %d, PRESERVE in %d); '
          'SGPR source accepted; UNUSED_SEXT and neg refused' % (forms, len(GRID), caught_sext, caught_preserve))


if __name__ == '__main__':
    main()
