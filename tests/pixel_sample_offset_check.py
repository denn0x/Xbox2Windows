"""Exercise four gfx1030 2D sample forms with immediate texel offsets."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from compute_store_check import assemble
from pixel_aot_check import RAMP, envelope, pixel_aot, translate, write_record
from pixel_sample_lod_check import base_prefix, compile_ps, export16, export_tail, single


FORMS = {
    "image_sample_o": ("v[7:9]", "Sample"),
    "image_sample_b_o": ("v[6:9]", "SampleBias"),
    "image_sample_l_o": ("v[7:10]", "SampleLevel"),
    "image_sample_lz_o": ("v[7:9]", "SampleLevel"),
}


def program(op, *, offset="0x3e01", level="0", quad_scale=False, swap_rg=False):
    address, _ = FORMS[op]
    offset_register = "v6" if op == "image_sample_b_o" else "v7"
    body = [f"v_mov_b32_e32 {offset_register}, {offset}"]
    if op == "image_sample_b_o":
        body.append(f"v_mov_b32_e32 v7, {level}")
    elif op == "image_sample_l_o":
        body.append(f"v_mov_b32_e32 v10, {level}")
    body.append(f"{op} v[12:15], {address}, s[28:35], s[40:43] dmask:0xf dim:SQ_RSRC_IMG_2D")
    return base_prefix(quad_scale=quad_scale) + body + export_tail(swap_rg=swap_rg)


def reject(program, diagnostic):
    try:
        translate(program)
    except ValueError as error:
        assert diagnostic in str(error), str(error)
    else:
        raise AssertionError(f"Expected rejection with {diagnostic!r}")


def check_lowering():
    for op, (_, method) in FORMS.items():
        level_arg = {"image_sample_o": "", "image_sample_lz_o": r", 0\.0f"}.get(op, r", alu\d+")
        source = translate(program(op))
        call = rf"tex\.{method}\(samp, float2\(uv\.x, uv\.y\){level_arg}, int2\(1, -2\)\);"
        assert re.search(call, source), op
    op = "image_sample_lz_o"
    source = translate(program(op, offset="0x0738"))
    assert "int2(-8, 7)" in source
    copied = program(op, offset="v6")
    copied.insert(copied.index("v_mov_b32_e32 v7, v6"), "v_mov_b32_e32 v6, 0x3e01")
    assert "int2(1, -2)" in translate(copied)
    reject(program(op, offset="0x8"), "outside the DXIL immediate range")
    reject(program(op, offset="0x3700"), "outside the DXIL immediate range")
    reject(program(op, offset="0x10000"), "canonical 2D offset word")
    reject(program(op, offset="0x40"), "canonical 2D offset word")
    undefined = [line for line in program(op) if not line.startswith("v_mov_b32_e32 v7,")]
    reject(undefined, "known packed integer word")
    runtime = [line.replace("v_mov_b32_e32 v7, 0x3e01", "v_mov_b32_e32 v7, s60")
               for line in program(op)]
    # Bind a runtime cbuffer value at s60 through the existing constant-descriptor path.
    runtime[:len(base_prefix())] = base_prefix(quad_scale=True)
    reject(runtime, "known packed integer word")
    pending = [line.replace("v[12:15]", "v[7:10]") if line.startswith(op) else line
               for line in program(op)]
    pending.insert(next(i for i, line in enumerate(pending) if line.startswith(op)) + 1,
                   next(line for line in program(op) if line.startswith(op)))
    reject(pending, "offset used before vmcnt wait")
    for old, new, diagnostic in (
        ("v[7:9]", "v[8:9]", "Descriptor or vector width mismatch"),
        ("v[7:9]", "[v7, v8, v9]", "Unsupported instruction"),
        ("dmask:0xf", "dmask:0x7", "requires dmask:0xf"),
        ("SQ_RSRC_IMG_2D", "SQ_RSRC_IMG_3D", "Unsupported instruction"),
        ("SQ_RSRC_IMG_2D", "SQ_RSRC_IMG_2D a16", "Unsupported instruction"),
        ("SQ_RSRC_IMG_2D", "SQ_RSRC_IMG_2D d16", "Unsupported instruction"),
        ("s[40:43]", "s[44:47]", "Unsupported image or sampler binding"),
    ):
        reject([line.replace(old, new) for line in program(op)], diagnostic)


def check_binary(directory, assembler, objcopy, library):
    features = "+wavefrontsize64,-wavefrontsize32"
    for op in FORMS:
        lines = program(op, level="1.0")
        data = assemble(directory, assembler, objcopy, lines, cpu="gfx1030", features=features)
        decoded = pixel_aot.disassemble(data, library, cpu=b"gfx1030", features=features.encode(), offsets=True)
        assert [line for _, line in decoded if line.startswith("image_")] == [
            line for line in lines if line.startswith("image_")
        ]
        expected = translate(lines)
        assert translate(decoded) == expected

        archive = directory / "fixture.bin"
        archive.write_bytes(envelope(data))
        output = directory / "fixture.hlsl"
        subprocess.run([sys.executable, "-B", pixel_aot.__file__, archive, "fixture", "--llvm", library,
                        "--cpu", "gfx1030", "--texture-sgpr", "20", "--interp-sgpr", "24",
                        "--sampler-words", "1", "2", "3", "4", "--constant-descriptor", "0x40",
                        "--output", output],
                       check=True, capture_output=True, text=True)
        assert output.read_text(encoding="utf-8") == expected


def check_compile(directory, compiler):
    return {op: compile_ps(compiler, translate(program(op)), directory, op) for op in FORMS}


def ramp_case(dx, dy):
    out = bytearray(256)  # unused b0 constants in the existing pixel renderer
    for y in range(8):
        for x in range(8):
            u = RAMP[max(0, min(7, x + dx))]
            v = RAMP[max(0, min(7, y + dy))]
            out += struct.pack("<4f", export16(single(u / 255)), export16(single(v / 255)),
                               export16(single(128 / 255)), 1.0)
    return out


def draw(renderer, adapter, mode, shader, *args, mismatch=False):
    result = subprocess.run([renderer, adapter, mode, shader, *args], capture_output=True, text=True)
    if result.returncode == 77:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise SystemExit(77)
    if mismatch:
        assert result.returncode == 1 and "pixel_mismatch" in result.stderr, (result.stdout, result.stderr)
        print(f"Negative control {Path(shader).stem}:\n{result.stderr}", end="")
    else:
        assert result.returncode == 0, (result.stdout, result.stderr)
    print(result.stdout, end="")


def check_draw(directory, compiler, renderer, adapter):
    case = directory / "offset.case"
    case.write_bytes(ramp_case(1, -2))
    for op, shader in check_compile(directory, compiler).items():
        draw(renderer, adapter, "--check-pixel-case", shader, case)
        mutant = compile_ps(compiler, translate(program(op, offset="0")), directory, f"{op}_zero_offset")
        draw(renderer, adapter, "--check-pixel-case", mutant, case, mismatch=True)

    # The spatial ramp exercises offsets at mip zero. Reuse the two-color texture
    # to distinguish a nonzero explicit LOD or bias from accidental mip-zero sampling.
    for op in ("image_sample_l_o", "image_sample_b_o"):
        for level in ("1.0", "0.0"):
            shader = compile_ps(compiler, translate(program(op, level=level)), directory, f"{op}_level_{level}")
            draw(renderer, adapter, "--check-pixel-quad-lod", shader, mismatch=level == "0.0")
    # A 4x horizontal derivative selects mip 1 for the implicit form. Swap R/G
    # so mip 0 from the LZ form matches the renderer's green reference.
    for op in ("image_sample_lz_o", "image_sample_o"):
        shader = compile_ps(compiler, translate(program(op, quad_scale=True, swap_rg=True)),
                            directory, f"{op}_quad")
        draw(renderer, adapter, "--check-pixel-quad-lod", shader, mismatch=op == "image_sample_o")
    write_record(os.environ.get("FORM_COVERAGE_RECORD"))


if __name__ == "__main__":
    check_lowering()
    if len(sys.argv) > 1:
        mode, *args = sys.argv[1:]
        out = Path(__file__).resolve().parents[1] / "out"
        out.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="sample-offset-", dir=out) as tmp:
            {"binary": check_binary, "compile": check_compile, "draw": check_draw}[mode](Path(tmp), *args)
    print("Four 2D sample offset forms checked")
