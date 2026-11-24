"""s_buffer_load with its offset register inside the destination range, an SGPR offset plus offset:N, and a load
into a register that held a lane mask (tools/startup/gfx2hlsl.py; reported by the mesh translator, board 191901).

1. s_buffer_load_dwordx2 s[4:5], s[16:19], s4: both words address with the old s4 (the ISA reads the operands
   before writing), so s5 must be written before s4. Control: s[6:7] with offset s4 keeps the plain order.
2. s_buffer_load_dword s0, s[16:19], s4 offset:0x10: the byte offset is s4 + 16. Another modifier is refused.
3. v_cmp into s6 makes s6 a lane mask; s_buffer_load_dword s6, ... replaces it with a buffer word, so a later
   s_or_b32 s8, s6, s7 is an ordinary OR, not a mask OR cut to bit 0. Control: without the load it is a mask OR.
All inputs are synthetic register states; no title data.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    sampler_heap = False
    root = {'params': [], 'flags': 0}

    def resource(self, key, kind):
        return 'r0'

    def ud(self, source):
        return 0


DESC = {16: ('desc', ('table', 0), 0x20, 0), 17: ('desc', ('table', 0), 0x20, 1),
        18: ('desc', ('table', 0), 0x20, 2), 19: ('desc', ('table', 0), 0x20, 3)}


def stage():
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    s.sym.update(DESC)
    s.pc = 0
    return s


def load(s, text):
    ops, mods = text.split(' ', 1)[1], []
    parts = ops.split(', ')
    last = parts[-1].split(' ')
    parts[-1], mods = last[0], last[1:]
    getattr(s, 'op_' + text.split(' ')[0])(parts, mods)


def main():
    s = stage()
    load(s, 's_buffer_load_dwordx2 s[4:5], s[16:19], s4')
    assert s.lines == ['s5 = r0.Load(s4 + 4);', 's4 = r0.Load(s4 + 0);'], s.lines
    s = stage()
    load(s, 's_buffer_load_dwordx2 s[6:7], s[16:19], s4')
    assert s.lines == ['s6 = r0.Load(s4 + 0);', 's7 = r0.Load(s4 + 4);'], s.lines
    print('overlap: offset register written last; control keeps the order')

    s = stage()
    load(s, 's_buffer_load_dword s0, s[16:19], s4 offset:0x10')
    assert s.lines == ['s0 = r0.Load(s4 + 16);'], s.lines
    try:
        load(stage(), 's_buffer_load_dword s0, s[16:19], s4 slc')
    except G.Unsupported as error:
        assert 'modifier slc' in str(error), str(error)
    else:
        raise AssertionError('unknown modifier accepted')
    print('offset:N added to the SGPR offset; unknown modifier refused')

    for with_load in (True, False):
        s = stage()
        s.masks.add(6)  # as after v_cmp_* s6, ...
        if with_load:
            load(s, 's_buffer_load_dword s6, s[16:19], 0x0')
        s.lines = []
        s.one('s_or_b32', 's_or_b32 s8, s6, s7')
        text = '\n'.join(s.lines)
        if with_load:
            assert '& 1' not in text and 8 not in s.masks and 6 not in s.masks, (text, s.masks)
        else:
            assert '& 1' in text and 8 in s.masks, (text, s.masks)
    print('mask mark: cleared by a load into the register; control keeps the mask OR')


if __name__ == '__main__':
    main()
