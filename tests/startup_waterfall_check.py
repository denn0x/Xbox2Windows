"""Waterfall loops (a VGPR descriptor index made scalar one value at a time) and s_nor_b32 of lane masks.

1. The loop form (LLVM SIInstrInfo.cpp emitLoadScalarOpsFromVGPRLoop, wave32; measured in title pixel stages that
   sample bindless textures):
       H: v_readfirstlane_b32 s13, v46; v_cmpx_eq_i32 s13, v46; s_lshl_b32 s13, s13, 5;
          s_load_dwordx8 s[32:39], s[10:11], s13; s_and_saveexec_b32 s13, s12; image_sample v44, ..., s[32:39];
          s_or_saveexec_b32 s26, s13; s_andn2_wrexec_b32 s25, s25; s_cbranch_scc1 H
   RDNA 2 ISA: V_READFIRSTLANE_B32 reads the lowest active lane (lane 0 when exec is 0); V_CMPX writes EXEC with
   the compare of the active lanes; S_AND_SAVEEXEC D = EXEC, EXEC = S0 & EXEC; S_OR_SAVEEXEC D = EXEC,
   EXEC = S0 | EXEC; S_ANDN2_WREXEC EXEC = S0 & ~EXEC, D = EXEC, SCC = EXEC != 0.
   A wave model (32 lanes) and the one-lane model (each lane alone, the readfirstlane giving its own index, as the
   translation emits) are run on random index, exec and s12 patterns: every lane samples the same descriptor and
   keeps the same v44. Negative control: a one-lane model whose readfirstlane takes lane 0's index differs.
   The translation: the readfirstlane emits s13 = v46 at a loop head of this form (b32, and the wave64 b64 form
   with an s_waitcnt head); refused without the v_cmpx_eq, with a loop-carried SGPR, with an SGPR read after the
   loop, with another branch or a scalar store in the body, and without the wrexec close.
   One pass: the emitted form runs the body once (the proven loop's s_cbranch_scc1 becomes fall-through). The
   one-lane model with one pass equals the wave on every lane; with a wrong index (the negative control) the looped
   one-lane form never exits, the one-pass form ends (a wrong value, not a hang).
2. s_nor_b32 of a mask SGPR: the lane's bit of ~(a | b) is ~(a_i | b_i) (value checked over the four bit pairs);
   no SCC is written (inactive lanes are 1 after the complement), so a following s_cbranch_scc1 is refused.
"""
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import stage  # noqa: E402

LOOP = ['v_readfirstlane_b32 s13, v46', 'v_cmpx_eq_i32_e32 s13, v46', 's_lshl_b32 s13, s13, 5',
        's_load_dwordx8 s[32:39], s[10:11], s13', 's_and_saveexec_b32 s13, s12', 's_waitcnt lgkmcnt(0)',
        'image_sample v44, v[40:41], s[32:39], s[20:23] dmask:0x1 dim:SQ_RSRC_IMG_2D',
        's_or_saveexec_b32 s26, s13', 's_andn2_wrexec_b32 s25, s25', 's_cbranch_scc1 label_0x%x']


def wave(index, exec_, s12, heap):
    """The loop on a 32-lane wave: v44 per lane (None = not written)."""
    v44 = [None] * 32
    s25, rounds = exec_, 0
    while True:
        rounds += 1
        assert rounds <= 33
        first = (exec_ & -exec_).bit_length() - 1 if exec_ else 0
        s13 = index[first]
        exec_ &= sum(1 << l for l in range(32) if index[l] == s13)
        desc = heap[(s13 << 5) >> 5]
        s13, exec_ = exec_, s12 & exec_
        for l in range(32):
            if exec_ >> l & 1:
                v44[l] = desc
        exec_ |= s13
        exec_ = s25 & ~exec_ & 0xffffffff
        s25 = exec_
        if not exec_:
            return v44


def one_lane(own, active, s12bit, heap, first_lane_index=None, passes=4):
    """The same loop in the one-lane model: exec is this lane's bit; readfirstlane gives own (or a forced index).
    passes=1 is the emitted form: the body once, the back edge dropped."""
    v44, exec_, s25 = None, active, active
    for _ in range(passes):
        if passes == 1:
            s13 = own if first_lane_index is None else first_lane_index
            exec_ &= int(s13 == own)
            desc = heap[(s13 << 5) >> 5]
            s13, exec_ = exec_, s12bit & exec_
            if exec_:
                v44 = desc
            return v44
        s13 = own if first_lane_index is None else first_lane_index
        exec_ &= int(s13 == own)
        desc = heap[(s13 << 5) >> 5]
        s13, exec_ = exec_, s12bit & exec_
        if exec_:
            v44 = desc
        exec_ |= s13
        exec_ = s25 & ~exec_ & 1
        s25 = exec_
        if not exec_:
            return v44
    return 'no exit'  # (only the negative control's forced index gets here)


def semantics():
    rng = random.Random(7)
    heap = {i: 1000 + i for i in range(16)}
    trials = 0
    for _ in range(400):
        index = [rng.choice((0, 3, 3, 5, 9, 15)) for _ in range(32)]
        exec_ = rng.getrandbits(32) | (1 << rng.randrange(32))
        s12 = rng.getrandbits(32) | rng.choice((0, 0xffffffff))
        hw = wave(index, exec_, s12, heap)
        for l in range(32):
            assert hw[l] == one_lane(index[l], exec_ >> l & 1, s12 >> l & 1, heap), (index, hex(exec_), l)
            assert hw[l] == one_lane(index[l], exec_ >> l & 1, s12 >> l & 1, heap, passes=1), (index, l)
        trials += 1
    # the looped one-lane form never exits under a wrong index (the hang shape); the one-pass form ends regardless
    assert one_lane(5, 1, 1, heap, first_lane_index=3) == 'no exit'
    assert one_lane(5, 1, 1, heap, first_lane_index=3, passes=1) is None
    index = [3] + [5] * 31
    hw = wave(index, 0xffffffff, 0xffffffff, heap)
    assert any(hw[l] != one_lane(index[l], 1, 1, heap, first_lane_index=index[0]) for l in range(32))
    return trials


def ins_of(text_lines, at=0x100):
    out, off = [], at
    for text in text_lines:
        name = text.split(' ', 1)[0]
        n = 8 if name.startswith(('s_load', 'image_', 'v_cmpx_eq_i32_e64')) else 4
        out.append((off, n, name, text))
        off += n
    return out


def build(body, head=0x100, prefix=('s_mov_b32 s25, exec_lo',), after=('s_endpgm',)):
    pre = ins_of(prefix, head - 4 * len(prefix))
    loop = ins_of([x % head if '%x' in x else x for x in body], head)
    post = ins_of(after, loop[-1][0] + loop[-1][1])
    return pre + loop + post


def readfirstlane(ins, pc):
    s = stage([])
    s.ins, s.pc, s.lines = ins, pc, []
    s.uniform_v, s.partial_uniform = set(), {}
    s.one('v_readfirstlane_b32', [t for o, _, _, t in ins if o == pc][0])
    return s.lines


def refused(ins, pc, why):
    try:
        readfirstlane(ins, pc)
    except G.Unsupported as error:
        assert why in str(error), (why, str(error))
        return
    raise AssertionError('accepted: ' + why)


def one_pass():
    """The proven loop's s_cbranch_scc1 is emitted as fall-through (no back edge, so no dispatch loop); an
    s_cbranch_scc1 elsewhere, or in a stage where no waterfall was proven, is still a branch."""
    ins = build(LOOP)
    back = [(o, t) for o, _, n, t in ins if n == 's_cbranch_scc1'][0]
    s = stage([])
    s.ins, s.pc, s.lines = ins, 0x100, []
    s.uniform_v, s.partial_uniform = set(), {}
    s.one('v_readfirstlane_b32', 'v_readfirstlane_b32 s13, v46')
    s.pc, s.lines = back[0], []
    assert s.one('s_cbranch_scc1', back[1]) is None and 'not taken' in s.lines[0], s.lines
    s.pc = back[0] + 8
    assert s.one('s_cbranch_scc1', back[1]) == ('branch', 'scc', 0x100)
    fresh = stage([])
    fresh.pc, fresh.lines = back[0], []
    assert fresh.one('s_cbranch_scc1', back[1]) == ('branch', 'scc', 0x100)


def translation():
    one_pass()
    assert readfirstlane(build(LOOP), 0x100) == ['s13 = v46;']
    wide = ['s_waitcnt vmcnt(0)', 'v_readfirstlane_b32 s10, v28', 'v_cmpx_eq_i32_e32 s10, v28',
            's_lshl_b32 s10, s10, 5', 's_load_dwordx8 s[48:55], s[30:31], s10', 's_waitcnt lgkmcnt(0)',
            'image_sample_lz v21, v[21:22], s[48:55], s[32:35] dmask:0x8 dim:SQ_RSRC_IMG_2D',
            's_andn2_wrexec_b64 s[44:45], s[44:45]', 's_cbranch_scc1 label_0x%x']
    assert readfirstlane(build(wide), 0x104) == ['s10 = v28;']
    controls = 0
    no_cmpx = [x for x in LOOP if not x.startswith('v_cmpx')]
    refused(build(no_cmpx), 0x100, 'requires a proven uniform source'); controls += 1
    carried = LOOP[:2] + ['s_add_u32 s40, s40, 1'] + LOOP[2:]
    refused(build(carried), 0x100, 'carries s40'); controls += 1
    refused(build(LOOP, after=('s_mov_b32 s5, s26', 's_endpgm')), 0x100, 's26 read after'); controls += 1
    # a read on the branch path only (loop exit at 0x130: the branch, s_endpgm, then the read at 0x138)
    refused(build(LOOP, after=('s_cbranch_scc0 label_0x138', 's_endpgm', 's_lshl_b32 s5, s13, 1', 's_endpgm')),
            0x100, 's13 read after'); controls += 1
    branchy = LOOP[:3] + ['s_cbranch_execz label_0x200'] + LOOP[3:]
    refused(build(branchy), 0x100, 'another branch'); controls += 1
    store = LOOP[:3] + ['s_store_dword s13, s[0:1], 0x0'] + LOOP[3:]
    refused(build(store), 0x100, 'body with s_store_dword'); controls += 1
    unclosed = [x for x in LOOP if 'wrexec' not in x]
    refused(build(unclosed), 0x100, 'not closed by s_andn2_wrexec'); controls += 1
    # written again before any read on every exit path: accepted
    assert readfirstlane(build(LOOP, after=('s_mov_b32 s13, 0', 's_mov_b32 s26, 1', 's_mov_b32 s5, s13',
                                            's_endpgm')), 0x100) == ['s13 = v46;']
    return controls


def sop2(op, dst, a, b):
    return 0x80000000 | op << 23 | dst << 16 | b << 8 | a


def nor():
    cmp_vcc = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129          # v_cmp_lt_u32 vcc, 1, v0
    save = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106             # s_and_saveexec_b64 s[20:21], vcc (s20 a mask)
    words = [cmp_vcc, save, sop2(0x1a, 5, 20, 20)]        # s_nor_b32 s5, s20, s20
    text = [x[3] for x in G.decode(struct.pack('<3I', *words))]
    assert text[2] == 's_nor_b32 s5, s20, s20', text
    s = stage(words)
    body = [line for _, lines, _ in s.blocks for line in lines]
    line = [x for x in body if x.startswith('s5 = ')]
    assert line == ['s5 = (~(s20 | s20)) & 1;'], body
    assert not any(x.startswith('scc') for x in body[body.index(line[0]):]), body
    for a in (0, 1):
        for b in (0, 1):
            assert (~(a | b)) & 1 == int(not (a or b))
    # a 0 / 0xffffffff operand (s_cselect_b32 s10, -1, 0) has every lane's bit equal: accepted with a mask
    splat = [cmp_vcc, sop2(0x0a, 10, 0xc1, 0x80), sop2(0x1a, 5, 106, 10)]
    text = [x[3] for x in G.decode(struct.pack('<3I', *splat))]
    assert text[1:] == ['s_cselect_b32 s10, -1, 0', 's_nor_b32 s5, vcc_lo, s10'], text
    body = [line for _, lines, _ in stage(splat).blocks for line in lines]
    assert 's5 = (~(vcc | s10)) & 1;' in body, body
    for value, lane in ((0, 0), (0, 1), (0xffffffff, 0), (0xffffffff, 1)):
        assert (~(lane | value)) & 1 == int(not (lane or value & 1))
    # a plain number with a mask (bit i is not lane i's) stays refused
    try:
        stage([cmp_vcc, sop2(0x1a, 5, 106, 11)])
    except G.Unsupported as error:
        assert 'on a lane mask' in str(error), str(error)
    else:
        raise AssertionError('s_nor_b32 of vcc_lo and a number accepted')
    branch = 0xbf850000 | 1                                     # s_cbranch_scc1 +1
    try:
        stage(words + [branch, 0xbf800000])
    except G.Unsupported as error:
        assert 'scc' in str(error).lower(), str(error)
    else:
        raise AssertionError('SCC of s_nor_b32 on a mask read')


def main():
    trials = semantics()
    controls = translation()
    nor()
    print('waterfall: wave and one-lane models agree on %d random (index, exec, s12) patterns x 32 lanes; '
          'lane-0 readfirstlane caught (negative control); b32 and b64 forms translate to sN = vM; %d controls '
          'refused; one pass (back edge dropped) matches the wave on every lane and ends under a wrong index where the '
          'looped form never exits; s_nor_b32 of a mask is the lane bit with no SCC (SCC read refused)' % (trials, controls))


if __name__ == '__main__':
    main()
