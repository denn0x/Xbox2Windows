"""Check one exact mip plain-Sample MinLod readback against a desktop HLSL control."""
from pathlib import Path
import subprocess
import sys
import tempfile


CLAMP = "1"
EXPECTED_MIP = 1
NEGATIVE_MIP = 2
SKIP = 77


def draw(renderer, adapter, dxil, level):
    result = subprocess.run(
        [renderer, adapter, "--check-pixel-mip-exact", dxil, str(level)],
        capture_output=True,
        text=True,
    )
    return result.returncode, result.stdout + result.stderr


def main(assembler, validator, cross, dxc, renderer, adapter):
    print("MinLod exact mip profile ps_6_0", flush=True)
    here = Path(__file__).parent
    template = (here / "minlod.spvasm").read_text(encoding="ascii")
    anchor = "%one = OpConstant %float 1\n"
    if anchor not in template or "@sample@" not in template:
        raise RuntimeError("minlod.spvasm no longer has the expected constant and sample markers")

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        asm = path / "translated.spvasm"
        module = path / "translated.spv"
        hlsl = path / "translated.hlsl"
        dxil = path / "translated.dxil"

        source = template.replace(anchor, f"{anchor}%clamp = OpConstant %float {CLAMP}\n")
        asm.write_text(
            source.replace(
                "@sample@",
                "OpImageSampleImplicitLod %v4 %sampled %uv MinLod %clamp",
            ),
            encoding="ascii",
        )
        subprocess.run(
            [assembler, "--target-env", "vulkan1.2", asm, "-o", module],
            check=True,
        )
        subprocess.run(
            [validator, "--target-env", "vulkan1.2", module],
            check=True,
        )
        translated = subprocess.run(
            [cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl],
            capture_output=True,
            text=True,
        )
        if "MinLod texture operand not supported in HLSL." in translated.stderr:
            print(
                "skip: SPIRV-Cross lacks MinLod support; run tools/setup-spirv-tools.ps1",
                flush=True,
            )
            return SKIP
        if translated.returncode:
            print(translated.stderr, file=sys.stderr, flush=True)
            translated.check_returncode()
        subprocess.run(
            [dxc, "-T", "ps_6_0", "-E", "main", "-Fo", dxil, hlsl],
            check=True,
        )

        control_dxil = path / "control.dxil"
        subprocess.run(
            [
                dxc,
                "-T",
                "ps_6_0",
                "-E",
                "main",
                f"-DCLAMP={CLAMP}",
                "-Fo",
                control_dxil,
                here / "minlod_control.hlsl",
            ],
            check=True,
        )

        for kind, shader in (
            ("translated", dxil),
            ("control", control_dxil),
        ):
            code, output = draw(renderer, adapter, shader, EXPECTED_MIP)
            print(
                f"{kind} clamp={CLAMP} expected_mip={EXPECTED_MIP}\n{output}",
                end="",
                flush=True,
            )
            if code == SKIP:
                return SKIP
            if code or "pixels=64 channels=256 max_absolute_error=0" not in output:
                raise RuntimeError(f"{kind} MinLod clamp {CLAMP} did not read mip {EXPECTED_MIP}")

            code, output = draw(renderer, adapter, shader, NEGATIVE_MIP)
            print(
                f"{kind} negative clamp={CLAMP} stale_mip={NEGATIVE_MIP}\n{output}",
                end="",
                flush=True,
            )
            if code != 1 or "pixel_mismatch x=0 y=0" not in output:
                raise RuntimeError(
                    f"{kind} negative control did not fail at pixel (0, 0)"
                )

    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
