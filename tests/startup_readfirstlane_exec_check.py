"""v_readfirstlane_b32 of a buffer word loaded from a lane-invariant address, also under a partial exec.

Synthetic gfx1013 words only. The shape is what the run's pixel programs use for a constant read through a
typed or raw buffer inside a branch: `buffer_load_* vN, off, s[..]` (no VGPR address, so every lane reads the
same address) followed by `v_readfirstlane_b32 sM, vN` to move the word to an SGPR.

AMD RDNA 2 ISA: V_READFIRSTLANE_B32 copies S0 of the first active lane (lane 0 when exec is zero) to the SGPR.
A VGPR write under a partial exec changes only the active lanes, so the first active lane holds the loaded value
as long as exec has not been written between the load and the read. The SGPR belongs to the wave, so the
translation gives every lane that value from a shadow variable assigned in all lanes at the load (lanes that are
inactive keep their own VGPR value).

1. exec known (entry mask): a load with no VGPR address is lane-invariant, so readfirstlane reads it directly.
2. exec partial (after s_and_saveexec_b64): the load's line becomes `xv1 = ...; if (exec & 1) v1 = xv1;` and the
   SGPR takes xv1.
Controls (refused): a VGPR address (offen v0); exec written between the load and the read; the read at a branch
target; a partial-exec value that is not lane-invariant (v0). Negative control of the whole change: with the
shadow record dropped after every VGPR write, case 2 is refused again.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class TestPipeline:
    root = {'params': [{'type': 2}], 'flags': 0}

    def resource(self, key, kind):
        assert key == (('root', 0), None) and kind == 'buf', (key, kind)
        return 'r0'

    def ud(self, source):
        return 0


def load_off(vdst, offset):            # buffer_load_dword vD, off, s[8:11], 0 offset:N
    return [0xe0000000 | 12 << 18 | offset, vdst << 8 | 2 << 16 | 128 << 24]


def load_offen(vdst, vaddr):           # buffer_load_dword vD, vA, s[8:11], 0 offen
    return [0xe0000000 | 12 << 18 | 1 << 12, vaddr | vdst << 8 | 2 << 16 | 128 << 24]


V_CMP_LT_U32_VCC_V0 = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129     # v_cmp_lt_u32 vcc, 1, v0
AND_SAVEEXEC = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106           # s_and_saveexec_b64 s[20:21], vcc
RESTORE_EXEC = 0xbe800000 | 126 << 16 | 0x04 << 8 | 20           # s_mov_b64 exec, s[20:21]
READ = 0x7e000000 | 10 << 17 | 2 << 9 | 257                      # v_readfirstlane_b32 s10, v1
READ_V0 = 0x7e000000 | 10 << 17 | 2 << 9 | 256                   # v_readfirstlane_b32 s10, v0
V_MOV_V1_V0 = 0x7e000000 | 1 << 17 | 1 << 9 | 256                # v_mov_b32 v1, v0
USE = 0x7e000000 | 2 << 17 | 1 << 9 | 10                         # v_mov_b32 v2, s10
NOP_BRANCH = 0xbf820000                                          # s_branch to the next instruction


def stage(words):
    return G.Stage(TestPipeline(), 'vs', struct.pack('<%dI' % (len(words) + 1), *words, 0xbf810000))


def asm(words):
    return [x[3] for x in G.decode(struct.pack('<%dI' % len(words), *words))]


def body(s):
    return '\n'.join(line for block in s.blocks for line in block[1])


def refused(words, label):
    try:
        stage(words)
    except G.Unsupported as error:
        assert 'requires a proven uniform source' in str(error), (label, str(error))
        return
    raise AssertionError('accepted: ' + label)


def main():
    assert asm(load_off(1, 16)) == ['buffer_load_dword v1, off, s[8:11], 0 offset:16'], asm(load_off(1, 16))
    assert asm(load_offen(1, 0)) == ['buffer_load_dword v1, v0, s[8:11], 0 offen'], asm(load_offen(1, 0))
    assert asm([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC, RESTORE_EXEC, READ]) == [
        'v_cmp_lt_u32_e32 vcc_lo, 1, v0', 's_and_saveexec_b64 s[20:21], vcc', 's_mov_b64 exec, s[20:21]',
        'v_readfirstlane_b32 s10, v1'], asm([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC, RESTORE_EXEC, READ])

    # 1. exec known
    text = body(stage(load_off(1, 16) + [READ, USE]))
    assert 's10 = v1;' in text, text

    # 2. exec partial
    partial = [V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC] + load_off(1, 16) + [READ, RESTORE_EXEC, USE]
    text = body(stage(partial))
    assert 'xv1 = r0.Load(a + 0); if (exec & 1) v1 = xv1;' in text, text
    assert 's10 = xv1;' in text, text
    lines = text.split('\n')
    assert lines.index('a = 16;') < lines.index('xv1 = r0.Load(a + 0); if (exec & 1) v1 = xv1;'), text
    s = stage(partial)
    assert 'uint xv1 = 0;' in '\n'.join(s.declarations()), s.declarations()

    # Controls.
    refused(load_offen(1, 0) + [READ], 'VGPR address, exec known')
    refused([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC] + load_offen(1, 0) + [READ], 'VGPR address, exec partial')
    refused([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC] + load_off(1, 16) + [RESTORE_EXEC, READ],
            'exec written between the load and the read')
    refused([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC] + load_off(1, 16) + [NOP_BRANCH, READ], 'read at a branch target')
    refused([V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC, V_MOV_V1_V0, READ], 'per-lane value under partial exec')

    # Negative control: without the shadow the partial-exec case is the old refusal.
    original = G.Stage.set_v

    def no_shadow(self, n, expr):
        original(self, n, expr)
        self.partial_uniform.clear()
    G.Stage.set_v = no_shadow
    try:
        refused(partial, 'shadow disabled')
    finally:
        G.Stage.set_v = original
    print('readfirstlane_exec: invariant-address load read with exec known and partial (shadow xv1); '
          '5 controls refused; negative control (shadow off) refused')


if __name__ == '__main__':
    main()
