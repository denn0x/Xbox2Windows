"""Read a fixed element from a source-built shader and reject a lane-varying or stale VGPR."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/startup"))
import gfx2hlsl as gfx


class TestPipeline:
    root = {"params": [{"type": 2}], "flags": 0}

    def resource(self, key, kind):
        assert key == (("root", 0), None) and kind == "buf"
        return "r0"


def vop1(opcode, dst, src):
    return 0x7e000000 | dst << 17 | opcode << 9 | src


def translate(*extra, src=7, dest=4):
    # s_buffer_load_dword s7, s[8:11], 12
    # v_cvt_i32_f32 v4, src; [extra instructions]; v_readfirstlane_b32 s6, v[dest]
    words = [0xf4000000 | 8 << 18 | 7 << 6 | 4, 125 << 25 | 12,
             vop1(8, 4, src), *extra, vop1(2, 6, 256 + dest), 0xbf810000]
    return gfx.Stage(TestPipeline(), "ps", struct.pack(f"<{len(words)}I", *words))


stage = translate()
body = "\n".join(stage.blocks[0][1])
assert "s7 = r0.Load(0x0000000cu + 0);" in body
assert "v4 = uint(int(F(s7)));" in body
assert "s6 = v4;" in body
for words, args in [
    ([], {"src": 256}),             # Per-lane source.
    ([], {"dest": 0}),              # Read a different VGPR.
    ([vop1(1, 4, 256)], {}),        # Clobber the proven VGPR with a per-lane value.
    ([0xbe800300 | 126 << 16 | 128], {}),  # s_mov_b32 exec_lo, 0
    ([0xbf820000], {}),             # Branch target at the read.
]:
    try:
        translate(*words, **args)
    except gfx.Unsupported as error:
        assert "requires a proven uniform source" in str(error), str(error)
    else:
        raise AssertionError("accepted an unproven readfirstlane")
print("fixed scalar buffer value translated; five unproven reads rejected")
