"""s_cselect_b32 into vcc_lo (wave32 lane mask) and into a non-SGPR destination.

Synthetic words. In wave32, vcc_lo is the whole VCC lane mask, so `s_cselect_b32 vcc_lo, -1, 0` sets the lane's
VCC bit from SCC, as the generic scalar path already treats vcc_lo. Before this the select path assumed an SGPR
destination and raised a TypeError. Control: s_cselect_b32 into an SGPR is unchanged; into m0 it is refused.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def one(word):
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<I', word))[0]
    s.pc = 0
    s.one(name, text)
    return text, '\n'.join(s.lines)


SOP2_CSELECT_B32 = 0x80000000 | 10 << 23  # s_cselect_b32 sdst, ssrc0, ssrc1


def main():
    # SOP2: sdst [22:16], ssrc1 [15:8], ssrc0 [7:0]
    text, body = one(SOP2_CSELECT_B32 | 106 << 16 | 128 << 8 | 193)  # vcc_lo, -1, 0
    assert text == 's_cselect_b32 vcc_lo, -1, 0', text
    assert body == 'vcc = (scc ? 0xffffffffu : 0x00000000u) & 1;', body
    text, body = one(SOP2_CSELECT_B32 | 5 << 16 | 193 << 8 | 128)  # s5, 0, -1
    assert text == 's_cselect_b32 s5, 0, -1', text
    assert 'scc ? 0x00000000u : 0xffffffffu' in body and 'vcc' not in body, body
    try:
        one(SOP2_CSELECT_B32 | 124 << 16 | 128 << 8 | 193)  # m0, -1, 0
    except G.Unsupported as error:
        assert 's_cselect into m0' in str(error), str(error)
    else:
        raise AssertionError('s_cselect into m0 accepted')
    print('cselect: vcc_lo is the lane mask; SGPR destination unchanged; m0 refused')


if __name__ == '__main__':
    main()
