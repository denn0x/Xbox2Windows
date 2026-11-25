"""s_cmp_eq_u64 / s_cmp_lg_u64 on 64-bit data pairs.

Synthetic gfx1013 words only. The shape is a pixel program testing a 64-bit value read from a constant buffer
against zero before using it (`s_mov_b64 s[26:27], 0` ... `s_cmp_lg_u64 s[38:39], s[26:27]`; s_cbranch_scc0).
RDNA 2 ISA, SOPC: SCC = (S0 == S1) or (S0 != S1) on the 64-bit values.

The emitted SCC is evaluated in Python for value pairs that differ in the low dword only, the high dword only,
both, and neither (an equality on the low dword alone would pass the first and fail the second).
Controls (refused): exec as an operand; a pair holding a copy of exec (s_mov_b64 s[26:27], exec); a pair holding
a lane mask (v_cmp into s[26:27]); a 64-bit literal. Negative control: without the handlers the compare is the
old refusal.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class TestPipeline:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def sop1(op, d, s):
    return 0xbe800000 | d << 16 | op << 8 | s


def sopc(op, a, b):
    return 0xbf000000 | op << 16 | b << 8 | a


MOV_B32, MOV_B64 = 3, 4
CMP_EQ_U64, CMP_LG_U64 = 0x12, 0x13
EXEC = 126
INLINE0 = 128
V_CMP_LT_U32_S26 = [0xd4c10000 | 26, 0x00020081]  # v_cmp_lt_u32_e64 s26, 1, v0 (wave32: the mask is s26)


def stage(words):
    return G.Stage(TestPipeline(), 'vs', struct.pack('<%dI' % (len(words) + 1), *words, 0xbf810000))


def asm(words):
    return [x[3] for x in G.decode(struct.pack('<%dI' % len(words), *words))]


def scc_line(words):
    s = stage(words)
    lines = [line for block in s.blocks for line in block[1] if line.startswith('scc = ')]
    assert len(lines) == 1, s.blocks
    return lines[0][len('scc = '):-1]


def evaluate(expr, env):
    py = expr.replace('&&', ' and ').replace('||', ' or ')
    import re
    py = re.sub(r'\b(0x[0-9a-f]+|\d+)u\b', r'\1', py)
    return bool(eval(py, {}, env))


def refused(words, label, text):
    try:
        stage(words)
    except G.Unsupported as error:
        assert text in str(error), (label, str(error))
        return
    raise AssertionError('accepted: ' + label)


def main():
    zero = [sop1(MOV_B64, 26, INLINE0)]
    lg = sopc(CMP_LG_U64, 38, 26)
    eq = sopc(CMP_EQ_U64, 38, 26)
    got = asm(zero + [lg, eq, sop1(MOV_B64, 26, EXEC), sopc(CMP_LG_U64, EXEC, 26)] + V_CMP_LT_U32_S26)
    assert got == ['s_mov_b64 s[26:27], 0', 's_cmp_lg_u64 s[38:39], s[26:27]', 's_cmp_eq_u64 s[38:39], s[26:27]',
                   's_mov_b64 s[26:27], exec', 's_cmp_lg_u64 exec, s[26:27]', 'v_cmp_lt_u32_e64 s26, 1, v0'], got

    # s[26:27] = 0 folds to constants; s38/s39 stay run-time registers.
    lg_expr, eq_expr = scc_line(zero + [lg]), scc_line(zero + [eq])
    assert 's38' in lg_expr and 's39' in lg_expr, lg_expr
    for lo, hi in ((0, 0), (5, 0), (0, 5), (5, 5), (0xffffffff, 0xffffffff)):
        env = {'s38': lo, 's39': hi}
        assert evaluate(lg_expr, env) == ((lo | hi << 32) != 0), (lg_expr, lo, hi)
        assert evaluate(eq_expr, env) == ((lo | hi << 32) == 0), (eq_expr, lo, hi)
    # Two run-time pairs.
    both = scc_line([sopc(CMP_EQ_U64, 38, 40)])
    for a, b in (((1, 2), (1, 2)), ((1, 2), (1, 3)), ((1, 2), (0, 2))):
        env = {'s38': a[0], 's39': a[1], 's40': b[0], 's41': b[1]}
        assert evaluate(both, env) == (a == b), (both, a, b)

    # Controls.
    refused([sopc(CMP_LG_U64, EXEC, 26)], 'exec operand', '64-bit operand')
    refused([sop1(MOV_B64, 26, EXEC), lg], 'copy of exec', 'on a lane mask')
    refused(V_CMP_LT_U32_S26 + [lg], 'v_cmp lane mask', 'on a lane mask')
    # (a literal with bit 31 clear has high dword 0 under either extension and is accepted by Stage.pair)
    refused([sopc(CMP_LG_U64, 38, 255), 0x92345678], '64-bit literal, bit 31 set', '64-bit literal operand')

    # Negative control: without the handlers the compare is refused as an unknown instruction.
    saved = G.Stage.op_s_cmp_lg_u64, G.Stage.op_s_cmp_eq_u64
    del G.Stage.op_s_cmp_lg_u64, G.Stage.op_s_cmp_eq_u64
    try:
        refused(zero + [lg], 'handlers removed', 's_cmp_lg_u64')
    finally:
        G.Stage.op_s_cmp_lg_u64, G.Stage.op_s_cmp_eq_u64 = saved
    print('scmp_u64: eq/lg on data pairs evaluated for low-only, high-only, both and equal values; '
          'exec, exec copy, v_cmp mask and literal refused; negative control (no handlers) refused')


if __name__ == '__main__':
    main()
