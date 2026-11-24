"""Check explicit PS input masks with a fullscreen triangle and a per-pixel integer oracle."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from startup_image_3d_check import FULLSCREEN_VS, VS_WORDS, G, compile_hlsl, root_blob


def pixel_shader(reg, masks):
    # exp mrt0 vN, vN, vN, vN done vm; s_endpgm
    words = struct.pack('<III', 0xf800180f, reg * 0x01010101, 0xbf810000)
    return G.Pipeline(VS_WORDS, words, root_blob(), ps_hw_inputs=masks).hlsl_ps()


def main(dxc, renderer, adapter, fxc=None):
    try:
        pixel_shader(1, (0x8100, 0x8000))
    except G.Unsupported as err:
        assert 'enable/address' in str(err)
    else:
        raise AssertionError('contradictory metadata accepted')
    compilers = [('dxc', dxc, '6_0')]
    if fxc:
        compilers.append(('fxc', fxc, '5_1'))
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        for kind, compiler, profile in compilers:
            vs = compile_hlsl(compiler, kind, FULLSCREEN_VS, 'vs', 'vs_' + profile, folder)
            for label, reg, masks, good in (
                ('one earlier word', 1, (0x8100, 0x8100), True),
                ('one earlier pair', 2, (0x8002, 0x8002), True),
                ('missing metadata', 1, None, False),
                ('wrong register', 1, (0x8002, 0x8002), False),
                ('packed position disabled', 1, (0x0100, 0x0100), False),
            ):
                try:
                    text = pixel_shader(reg, masks)
                except G.Unsupported as err:
                    # The translator maps every entry VGPR to its SPI input and refuses a read of one it does not
                    # lower (persp_center, an unallocated register): a stronger outcome than a wrong image.
                    assert not good and ('not lowered' in str(err) or 'past the SPI inputs' in str(err)), str(err)
                    print(f'{kind} {adapter} {label}: refused at translation ({err})')
                    continue
                ps = compile_hlsl(compiler, kind, text, 'ps', 'ps_' + profile, folder)
                run = subprocess.run([renderer, adapter, '--startup-packed-position', vs, ps],
                                     capture_output=True, text=True)
                if run.returncode == 77:
                    print(run.stdout + run.stderr)
                    return 77
                if good:
                    assert run.returncode == 0 and 'all 91 packed positions matched' in run.stdout, (run.stdout, run.stderr)
                else:
                    assert run.returncode != 0 and 'position mismatch (1,0): expected 00000001 actual 00000000' in run.stderr, run.stderr
                print(f'{kind} {adapter} {label}: ' + ('91 pixels matched' if good else 'expected mismatch at (1,0)'))
    return 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
