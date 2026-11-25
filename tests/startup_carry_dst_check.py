"""A carry-out or compare destination that is not an SGPR, vcc or exec is refused (it used to crash the translation).

Synthetic VOP3B word: v_add_co_ci_u32_e64 v100, <sdst>, 0, v100, vcc_lo (op 0x128). With sdst vcc_hi (107) the
translator raised TypeError in write_compare (reg_range('vcc_hi') is None), which ends a whole translation as a
crash; it is now the refusal "compare or carry destination vcc_hi". Controls: sdst s20 is lowered and s20 becomes a
lane mask; sdst vcc_lo writes vcc.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import asm, stage  # noqa: E402


def word(sdst):
    return [0xd4000000 | 0x128 << 16 | sdst << 8 | 100, 128 | (256 + 100) << 9 | 106 << 18]


def main():
    assert asm(word(107)) == ['v_add_co_ci_u32_e64 v100, vcc_hi, 0, v100, vcc_lo'], asm(word(107))
    try:
        stage(word(107))
    except G.Unsupported as error:
        assert str(error) == 'compare or carry destination vcc_hi', str(error)
    else:
        raise AssertionError('vcc_hi carry-out accepted')
    s = stage(word(20))
    assert 20 in s.masks, s.masks
    s = stage(word(106))
    assert not s.masks
    print('carry_dst: vcc_hi carry-out refused (was a TypeError crash); s20 and vcc_lo controls lowered')


if __name__ == '__main__':
    main()
