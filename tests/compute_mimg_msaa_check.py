"""Check the gfx1030 scalar-format, four-sample image_msaa_load profile."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from compute_store_check import MIMG_BINDING, SENTINEL, aot, gfx1030_binary, rejects, variant

HERE = Path(__file__).resolve().parent
NAME = "image_msaa_load"
BINDING = MIMG_BINDING | {"image_format": "R32_UINT"}
GROUP = (8, 8, 1)
LINES = (HERE / "compute_mimg_msaa.s").read_text(encoding="ascii").splitlines()
IMAGE = next(line for line in LINES if line.startswith(NAME))
ADDRESS = "int2(asint(tid.x), asint(tid.y))"


def reference(*, samples=(0, 1, 2, 3), swapped_xy=False):
    words = [SENTINEL] * 320
    for y in range(8):
        for x in range(8):
            col, row = (y, x) if swapped_xy else (x, y)
            for index, sample in enumerate(samples):
                words[32 * y + 4 * x + index] = 0x91000000 + sample * 0x10000 + row * 256 + col
    return words


def first_mismatch(expected, actual):
    return next((i, a, b) for i, (a, b) in enumerate(zip(expected, actual, strict=True)) if a != b)


def controls():
    source = aot.lower(LINES, GROUP, **BINDING)
    assert "Texture2DMS<uint, 4> imageData : register(t0);" in source
    for sample in range(4):
        assert f"uint image_msaa_0_{sample} = imageData.Load({ADDRESS}, {sample});" in source
    store = "uint4(image_msaa_0_0, image_msaa_0_1, image_msaa_0_2, image_msaa_0_3)"
    assert store in source
    for old, new, message in (
        ("dmask:0xf", "dmask:0x3", "Unsupported compute instruction"),
        ("dim:SQ_RSRC_IMG_2D_MSAA", "dim:SQ_RSRC_IMG_2D_MSAA_ARRAY", "Unsupported compute instruction"),
        (IMAGE, IMAGE.removesuffix(" unorm"), "Unsupported compute instruction"),
        (IMAGE, IMAGE + " a16", "Unsupported compute instruction"),
        (IMAGE, IMAGE + " d16", "Unsupported compute instruction"),
        (IMAGE, IMAGE + " tfe", "Unsupported compute instruction"),
        (IMAGE, IMAGE + " glc", "Unsupported compute instruction"),
        ("v[8:10]", "[v8, v9, v10]", "Unsupported compute instruction"),
        ("v[8:10]", "v[8:9]", "Invalid image_msaa_load VGPR tuple"),
        (IMAGE, IMAGE.replace("v[8:11]", "v[253:256]"), "Invalid image_msaa_load VGPR tuple"),
        ("v_mov_b32_e32 v10, 0", "v_mov_b32_e32 v10, 1", "requires a known zero fragment ID"),
        ("v_mov_b32_e32 v10, 0", "v_mov_b32_e32 v10, v8", "requires a known zero fragment ID"),
        ("s_waitcnt lgkmcnt(0)", "", "Unresolved image descriptor"),
        (IMAGE, IMAGE.replace("s[24:31]", "s[32:39]"), "Unverified image descriptor"),
        ("s_waitcnt vmcnt(0)", "", "before s_waitcnt vmcnt(0)"),
    ):
        rejects(message, aot.lower, variant(LINES, (old, new)), GROUP, **BINDING)
    rejects("requires gfx1030", aot.lower, LINES, GROUP, **(BINDING | {"cpu": "gfx1013"}))
    rejects("R32_UINT MSAA binding", aot.lower, LINES, GROUP, **MIMG_BINDING)
    ordinary = "image_load v8, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D"
    rejects("Unsupported image load target", aot.lower, variant(LINES, (IMAGE, ordinary)), GROUP, **BINDING)

    expected = reference()
    samples = (0, 0, 2, 3)
    repeated = (f"imageData.Load({ADDRESS}, 1)", f"imageData.Load({ADDRESS}, 0)")
    mutations = (
        ("repeated sample zero", *repeated, reference(samples=samples), (1, 0x91010000, 0x91000000)),
        ("reversed samples", store, "uint4(image_msaa_0_3, image_msaa_0_2, image_msaa_0_1, image_msaa_0_0)",
         reference(samples=(3, 2, 1, 0)), (0, 0x91000000, 0x91030000)),
        ("swapped coordinates", ADDRESS, "int2(asint(tid.y), asint(tid.x))", reference(swapped_xy=True),
         (4, 0x91000001, 0x91000100)),
    )
    for label, old, new, actual, mismatch in mutations:
        assert old in source and new != old, label
        i, a, b = first_mismatch(expected, actual)
        assert (i, a, b) == mismatch, label
        print(f"CPU control {label}: word={i} expected={a:08x} actual={b:08x}", flush=True)
    assert all(word != SENTINEL for word in expected[:256]) and expected[256:] == [SENTINEL] * 64
    return source, expected, mutations


def compile_shader(dxc, path, entry, target, output):
    subprocess.run([dxc, "-T", target, "-E", entry, "-Fo", output, path], check=True)


def desktop(directory, dxc, source, expected, mutations, renderer=None, adapter=None):
    input_hlsl = HERE / "compute_mimg_msaa_input.hlsl"
    vs, ps = directory / "input-vs.dxil", directory / "input-ps.dxil"
    compile_shader(dxc, input_hlsl, "vs_main", "vs_6_0", vs)
    compile_shader(dxc, input_hlsl, "ps_main", "ps_6_0", ps)
    hlsl, dxil, case = (directory / f"{NAME}.{ext}" for ext in ("hlsl", "dxil", "case"))
    case.write_bytes(struct.pack("<321I", 1, *expected))
    cmd = [renderer, adapter, "--compute-texture-msaa-case", dxil, case, vs, ps]
    variants = [("translated", source, None)] + [
        (label, source.replace(old, new), mismatch) for label, old, new, _, mismatch in mutations]
    for label, shader, mismatch in variants:
        hlsl.write_text(shader, encoding="ascii")
        compile_shader(dxc, hlsl, "main", "cs_6_0", dxil)
        if renderer is None:
            continue
        result = subprocess.run(cmd, capture_output=True, text=True)
        print(f"{label}:\n{result.stdout}{result.stderr}", end="", flush=True)
        if mismatch is None:
            if result.returncode:
                return result.returncode
            assert "compared_words=320 mismatches=0" in result.stdout
        else:
            i, a, b = mismatch
            assert result.returncode == 1 and f"word={i} expected={a:08x} actual={b:08x}" in result.stderr, label
    if renderer is not None:
        # Verify that the comparator checks the untouched tail as well as the four outputs per pixel.
        wrong = expected.copy()
        wrong[300] ^= 1
        case.write_bytes(struct.pack("<321I", 1, *wrong))
        hlsl.write_text(source, encoding="ascii")
        compile_shader(dxc, hlsl, "main", "cs_6_0", dxil)
        result = subprocess.run(cmd, capture_output=True, text=True)
        print(f"wrong sentinel:\n{result.stdout}{result.stderr}", end="", flush=True)
        assert result.returncode == 1 and "word=300 expected=c5c5c5c4 actual=c5c5c5c5" in result.stderr
        if record := os.environ.get("FORM_COVERAGE_RECORD"):
            path = Path(record)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"{NAME}\n", encoding="ascii")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--binary", nargs=3, metavar=("LLVM_MC", "LLVM_OBJCOPY", "LIBLLVM"))
    modes.add_argument("--compile", metavar="DXC")
    modes.add_argument("--draw", nargs=3, metavar=("DXC", "RENDERER", "ADAPTER"))
    args = parser.parse_args()
    source, expected, mutations = controls()
    if not (args.binary or args.compile or args.draw):
        return 0
    out = HERE.parent / "out"
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="compute_mimg_msaa_", dir=out) as tmp:
        directory = Path(tmp)
        if args.binary:
            gfx1030_binary(directory, *args.binary, [(NAME, LINES, BINDING)], NAME, group=GROUP)
        elif args.compile:
            return desktop(directory, args.compile, source, expected, mutations)
        else:
            dxc, renderer, adapter = args.draw
            return desktop(directory, dxc, source, expected, mutations, renderer, adapter)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
