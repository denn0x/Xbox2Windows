"""Loop-carried SGPRs: at a loop head the translation drops what the entry state knew about registers the loop writes.

The bug (stall, board msgs 045500, run 109): at a loop head gfx2hlsl kept the linear (entry) state, so a counter
set to a constant before the loop folded as that constant inside it: `s_mov_b32 s14, 0` then, in the loop,
`s_add_u32 s14, s14, 1` was emitted as `s14 = 0x00000000u + 0x00000001u` on every pass; the exit compare never
fired and each pixel ran to the dispatch guard.

Synthetic programs (encoded here, translated by Stage, the emitted block statements interpreted in Python with the
emitted block terminators, so the result depends on the trip count):
1. counter: s14 = 0; s15 = 0; L: s14 += 1; s15 += s14; s_cmp_lg_u32 s14, 5; s_cbranch_scc1 L; v0 = s15. Expect
   s15 = 1 + 2 + 3 + 4 + 5 = 15 after 5 passes.
2. counter with a bound read from a constant set before the loop and NOT written in it (s20 = 7): that constant
   still folds (the fix forgets only loop-written registers); expect 1 + ... + 7 = 28.
3. two nested counters (inner loop of 3 inside an outer loop of 4, the inner counter reset in the outer body):
   expect 12 inner passes.
Negative control: with Stage.forget_loop_writes made a no-op (the old behaviour) program 1 does not terminate
within 1000 block visits.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import stage  # noqa: E402


def sop1(op, d, s):
    return 0xbe800000 | d << 16 | op << 8 | s


def sop2(op, d, a, b):
    return 0x80000000 | op << 23 | d << 16 | b << 8 | a


def sopc(op, a, b):
    return 0xbf000000 | op << 16 | b << 8 | a


def sopp(op, words_back):
    return 0xbf800000 | op << 16 | (-words_back) & 0xffff  # target = next instruction - 4 * words_back bytes


MOV, ADD, CMP_LG, SCC1 = 3, 0, 7, 5
INLINE = lambda n: 128 + n  # noqa: E731  inline integer constants 0..64
V_MOV_V0 = lambda s: 0x7e000200 | s  # noqa: E731  v_mov_b32 v0, s<s>


def program(kind):
    if kind == 'counter':
        return [sop1(MOV, 14, INLINE(0)), sop1(MOV, 15, INLINE(0)),
                sop2(ADD, 14, 14, INLINE(1)), sop2(ADD, 15, 15, 14), sopc(CMP_LG, 14, INLINE(5)), sopp(SCC1, 4),
                V_MOV_V0(15)]
    if kind == 'bound':
        return [sop1(MOV, 20, INLINE(7)), sop1(MOV, 14, INLINE(0)), sop1(MOV, 15, INLINE(0)),
                sop2(ADD, 14, 14, INLINE(1)), sop2(ADD, 15, 15, 14), sopc(CMP_LG, 14, 20), sopp(SCC1, 4),
                V_MOV_V0(15)]
    if kind == 'nested':
        return [sop1(MOV, 10, INLINE(0)), sop1(MOV, 15, INLINE(0)),
                sop1(MOV, 11, INLINE(0)),                                    # O: inner counter reset
                sop2(ADD, 11, 11, INLINE(1)), sop2(ADD, 15, 15, INLINE(1)),  # I: inner pass, total += 1
                sopc(CMP_LG, 11, INLINE(3)), sopp(SCC1, 4),
                sop2(ADD, 10, 10, INLINE(1)), sopc(CMP_LG, 10, INLINE(4)), sopp(SCC1, 8),
                V_MOV_V0(15)]
    raise ValueError(kind)


def c_to_py(expr):
    expr = re.sub(r'\b(0x[0-9a-f]+|\d+)u\b', r'\1', expr)
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    m = re.fullmatch(r'\((.*)\) \? (.*) : (.*)', expr)
    if m:
        expr = '(%s if %s else %s)' % (m.group(2), m.group(1), m.group(3))
    return expr


def run(s, limit=1000):
    env = {'exec': 1, 'scc': False, 'vcc': 0}
    order = [b[0] for b in s.blocks]
    i, visits = 0, 0
    while i is not None and i < len(s.blocks):
        visits += 1
        if visits > limit:
            return None, visits
        off, lines, term = s.blocks[i]
        for line in lines:
            m = re.fullmatch(r'(?:if \(exec & 1\) )?(\w+) = (.*);', line.strip())
            assert m, line
            value = eval(c_to_py(m.group(2)), {}, env)
            env[m.group(1)] = value if isinstance(value, bool) else value & 0xffffffff
        nxt = i + 1
        if term and term[0] == 'end':
            break
        if term and term[0] == 'jump':
            nxt = order.index(term[1])
        elif term and term[0] == 'branch':
            if eval(c_to_py(term[1]), {}, env):
                nxt = order.index(term[2])
        i = nxt
    return env.get('v0'), visits


def main():
    want = {'counter': 15, 'bound': 28, 'nested': 12}
    for kind, value in want.items():
        s = stage(program(kind))
        got, visits = run(s)
        assert got == value, (kind, got, visits, s.blocks)
    bound = stage(program('bound'))
    assert any('s14 != 0x00000007u' in line for _, lines, _ in bound.blocks for line in lines), bound.blocks
    # A loop-written register whose entry value is symbolic only (a descriptor: its HLSL variable was never
    # assigned) becomes a conflict that any use refuses; a run-time-backed constant is simply forgotten.
    probe = stage(program('counter'))
    probe.sym = {14: ('desc', 0, 0, 0), 15: ('const', 0), 20: ('desc', 0, 0, 0)}
    probe.forget_loop_writes(8, 20)  # loop head = word 2, back edge = word 5
    assert probe.sym == {14: ('conflict',), 20: ('desc', 0, 0, 0)}, probe.sym
    saved = G.Stage.forget_loop_writes
    G.Stage.forget_loop_writes = lambda self, head, last: None
    try:
        old = stage(program('counter'))
        got, visits = run(old)
        assert got is None, (got, visits, old.blocks)  # the old form never leaves the loop
        assert any('0x00000000u + 0x00000001u' in line for _, lines, _ in old.blocks for line in lines)
    finally:
        G.Stage.forget_loop_writes = saved
    print('loop_carried: counter loop 15 (5 passes), constant bound not written in the loop still folds (28), '
          'nested 4 x 3 = 12; old behaviour (entry constants kept at the head) never exits: negative control')


if __name__ == '__main__':
    main()
