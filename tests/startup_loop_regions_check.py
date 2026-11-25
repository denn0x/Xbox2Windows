"""Stage.body: only the back-edge regions of the block graph run inside a dispatch loop.

The old form put every block of a program with any back edge in one `[loop] for` dispatch over pc; the new one
puts the blocks of each merged [loop head, back-edge source] interval in its own loop and tests the others once, in
offset order. Claim: the same blocks run in the same order. Checked on 3000 random block graphs (3..14 blocks; each
block falls through, ends, jumps or branches to any block; conditions drawn per visit from a seeded generator, so
both forms see the same values): the emitted text of body() is interpreted (block markers `if (pc == i)`, region
loops `pc >= lo && pc <= hi`, the pc assignments) and its block trace compared with the old form's semantics, a
plain dispatch loop. Traces are cut at 300 block visits in both. Negative control: an interpreter that skips the
region loops (one pass) differs on graphs with loops. Also: a loop-free graph emits no `[loop]`, and the measured
case (two short waterfall loops inside a long program) gets two small loops.
"""
import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402

LIMIT = 300


class Fake:
    body = G.Stage.body

    def __init__(self, blocks):
        self.blocks = blocks


def make(rng, n):
    blocks = []
    for i in range(n):
        kind = rng.random()
        if i == n - 1 and kind < 0.3:
            term = ('end',)
        elif kind < 0.35:
            term = None
        elif kind < 0.45:
            term = ('end',)
        elif kind < 0.6:
            term = ('jump', 0x10 * rng.randrange(n))
        else:
            term = ('branch', 'c%d' % i, 0x10 * rng.randrange(n))
        blocks.append((0x10 * i, ['trace(%d);' % i], term))
    return blocks


class Conds:
    def __init__(self, seed):
        self.seed, self.visits = seed, {}

    def __call__(self, block):
        k = self.visits[block] = self.visits.get(block, 0) + 1
        return random.Random(self.seed * 100003 + block * 1009 + k).random() < 0.5


def reference(blocks, conds):
    trace, pc = [], 0
    while pc != 0xffff and len(trace) < LIMIT:
        _, _, term = blocks[pc]
        trace.append(pc)
        nxt = pc + 1 if pc + 1 < len(blocks) else 0xffff
        if not term:
            pc = nxt
        elif term[0] == 'end':
            pc = 0xffff
        elif term[0] == 'jump':
            pc = term[1] // 0x10
        else:
            pc = term[2] // 0x10 if conds(pc) else nxt
    return trace


def parse(lines):
    """[('block', i, pc_line)] and [('loop', lo, hi, [blocks])] in order."""
    items, cur, loop = [], None, None
    for line in lines:
        s = line.strip()
        m = re.match(r'\[loop\] for \(uint guard = 0; guard < 65536 && pc >= (\d+) && pc <= (\d+); \+\+guard\) \{', s)
        if m:
            loop = ('loop', int(m.group(1)), int(m.group(2)), [])
            items.append(loop)
            continue
        m = re.match(r'(?:else )?if \(pc == (\d+)\) \{', s)
        if m:
            cur = [int(m.group(1)), None]
            (loop[3] if loop else items).append(('block', cur))
            continue
        if s.startswith('pc = ') and cur is not None:
            cur[1] = s
        elif s == '}' and cur is None and loop is not None:
            loop = None
        elif s == '}':
            cur = None
    return items


def run_pc(line, i, conds):
    m = re.fullmatch(r'pc = \((c\d+)\) \? (\d+) : (\d+);', line)
    if m:
        return int(m.group(2)) if conds(i) else int(m.group(3))
    return 0xffff if line == 'pc = 0xffffu;' else int(re.fullmatch(r'pc = (\d+);', line).group(1))


def emitted(lines, conds, loops=True):
    trace, pc = [], 0

    def block(b):
        nonlocal pc
        trace.append(b[0])
        pc = run_pc(b[1], b[0], conds)
    for item in parse(lines):
        if item[0] == 'block':
            if pc == item[1][0] and len(trace) < LIMIT:
                block(item[1])
        else:
            _, lo, hi, members = item
            guard = 0
            while lo <= pc <= hi and guard < 65536 and len(trace) < LIMIT:
                guard += 1
                for _, b in members:
                    if pc == b[0]:
                        block(b)
                        break
                if not loops:
                    break
    return trace


def main():
    rng = random.Random(11)
    looped = differ = 0
    for seed in range(3000):
        blocks = make(rng, rng.randrange(3, 15))
        lines = Fake(blocks).body()
        want = reference(blocks, Conds(seed))
        got = emitted(lines, Conds(seed))
        assert got == want, (seed, blocks, lines, want[:20], got[:20])
        if any('[loop]' in x for x in lines):
            looped += 1
            differ += emitted(lines, Conds(seed), loops=False) != want
    assert looped > 500 and differ > 100, (looped, differ)
    straight = [(0x10 * i, ['trace(%d);' % i], ('branch', 'c', 0x10 * (i + 2)) if i < 8 else None) for i in range(10)]
    assert not any('[loop]' in x for x in Fake(straight).body())
    measured = [(0x10 * i, ['x;'], None) for i in range(40)]
    measured[12] = (0xc0, ['x;'], ('branch', 'scc', 0xc0))
    measured[20] = (0x140, ['x;'], ('branch', 'scc', 0x130))
    lines = Fake(measured).body()
    heads = [x.strip() for x in lines if '[loop]' in x]
    assert heads == ['[loop] for (uint guard = 0; guard < 65536 && pc >= 12 && pc <= 12; ++guard) {',
                     '[loop] for (uint guard = 0; guard < 65536 && pc >= 19 && pc <= 20; ++guard) {'], heads
    print('loop_regions: %d random graphs run the same block trace as one dispatch loop (%d with loops; the '
          'one-pass negative control differs on %d); loop-free graph has no loop; two waterfall loops get two '
          'loops of 1 and 2 blocks' % (3000, looped, differ))


if __name__ == '__main__':
    main()
