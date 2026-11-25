"""s_and_saveexec_b64 whose destination is its own source: S0 is read before D is written.

RDNA 2 ISA SOP1 S_AND_SAVEEXEC_B64: D = EXEC; EXEC = S0 & EXEC; SCC = (EXEC != 0), where S0 is the value at issue.
With D == S0 (s_and_saveexec_b64 s[6:7], s[6:7], seen in title vertex and pixel stages; found by the compute
agent's vs/ps review) the translation used to write s6 = exec first and then compute exec & s6 = exec & exec, so
the branch let the lane in whatever the condition. Synthetic words: v_cmp_lt_u32 vcc, 1, v0; s_mov_b64 s[6:7], vcc;
s_and_saveexec_b64 s[6:7], s[6:7]. The emitted lines are run as Python for v0 = 0 (condition false: exec must be
0, s6 the old exec 1) and v0 = 2 (exec 1). Control: with S0 = s[8:9] no temporary is used and the result is the
same. Negative control: the old line order (s6 = exec before the read) gives exec 1 for v0 = 0.
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import asm, sop1, stage  # noqa: E402

CMP_VCC = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129   # v_cmp_lt_u32 vcc, 1, v0


def lines(words):
    s = stage([])
    s.lines = []
    for i, (_, n, name, text) in enumerate(G.decode(struct.pack('<%dI' % len(words), *words))):
        s.pc = i
        s.one(name, text)
    return s, s.lines


def run(body, v0):
    env = {'v0': v0, 'exec': 1}
    for line in body:
        for stmt in filter(None, (x.strip() for x in line.split(';'))):
            py = re.sub(r'(0x[0-9a-f]+|\d+)u\b', r'\1', stmt)
            py = re.sub(r'\(\((.*)\) \? 1 : 0\)', r'int(\1)', py)
            name, expr = py.split(' = ', 1)
            env[name] = eval(expr, {}, env)
    return env


def main():
    same = [CMP_VCC, sop1(4, 6, 106), sop1(0x24, 6, 6)]
    assert asm(same)[2] == 's_and_saveexec_b64 s[6:7], s[6:7]', asm(same)
    s, body = lines(same)
    assert 'xsave = s6;' in body and s.uses_xsave, body
    assert '    uint xsave = 0;' in s.declarations(), s.declarations()
    for v0, want in ((0, 0), (2, 1)):
        env = run(body, v0)
        assert env['exec'] == want and env['s6'] == 1, (v0, env)
    other = [CMP_VCC, sop1(4, 8, 106), sop1(0x24, 6, 8)]
    s2, body2 = lines(other)
    assert not s2.uses_xsave and not any('xsave' in x for x in body2), body2
    for v0, want in ((0, 0), (2, 1)):
        assert run(body2, v0)['exec'] == want
    old = [x for x in body if not x.startswith('xsave')]
    old = [x.replace('xsave', 's6') for x in old]
    assert run(old, 0)['exec'] == 1  # the bug the fix removes: the lane enters with a false condition
    print('saveexec_same_reg: S0 read before D (exec 0 / 1 for a false / true condition, s6 = old exec); '
          'distinct-register control unchanged; the old order lets the lane in (negative control)')


if __name__ == '__main__':
    main()
