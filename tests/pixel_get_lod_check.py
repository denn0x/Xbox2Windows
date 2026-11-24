"""Check both gfx1030 image_get_lod results against a CPU footprint model."""
import hashlib
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from compute_store_check import assemble
from pixel_aot_check import envelope, pixel_aot, translate, write_record
from pixel_sample_lod_check import base_prefix, compile_ps


QUERY = "image_get_lod v[12:13], v[8:9], s[28:35], s[40:43] dmask:0x3 dim:SQ_RSRC_IMG_2D"


def program():
    lines = base_prefix(quad_scale=True)
    i = lines.index("v_fma_f32 v8, s62, v8, 0")
    lines.insert(i + 1, "v_fma_f32 v9, s63, v9, 0")
    return lines + [QUERY, "s_waitcnt vmcnt(0)", "v_mov_b32_e32 v14, 0.5", "v_mov_b32_e32 v15, 1.0",
                    "exp mrt0 v12, v13, v14, v15 done vm", "s_endpgm"]


def upper_clamp_program():
    # On the two-mip fixture, the horizontal UV scale is 4. Export raw LOD - 2 in
    # red and clamped LOD in green, so the expected color is (0, 1, 0, 1).
    return base_prefix(quad_scale=True) + [
        QUERY, "s_waitcnt vmcnt(0)", "v_subrev_f32_e32 v13, 2.0, v13",
        "v_mov_b32_e32 v14, 0", "v_mov_b32_e32 v15, 1.0",
        "exp mrt0 v13, v12, v14, v15 done vm", "s_endpgm",
    ]


def controls():
    for old, new, diagnostic in (
        ("s_waitcnt vmcnt(0)", "s_nop 0", "Unsupported export"),
        ("s[40:43] dmask", "s[44:47] dmask", "Unsupported texture LOD image or sampler binding"),
        ("dmask:0x3", "dmask:0xf", "Unsupported instruction"),
        ("dim:SQ_RSRC_IMG_2D", "dim:SQ_RSRC_IMG_3D", "Unsupported instruction"),
        (QUERY, "v_mov_b32_e32 v8, 0.5\n" + QUERY, "quad provenance"),
    ):
        changed = [part for line in program() for part in line.replace(old, new).splitlines()]
        try:
            translate(changed)
        except ValueError as error:
            assert diagnostic.casefold() in str(error).casefold(), str(error)
        else:
            raise AssertionError(f"Expected rejection: {diagnostic}")


def binary(directory, assembler, objcopy, library):
    for stem, lines in (("footprint", program()), ("upper-clamp", upper_clamp_program())):
        native = assemble(directory, assembler, objcopy, lines, cpu="gfx1030", features="+wavefrontsize64")
        decoded = pixel_aot.disassemble(native, library, cpu=b"gfx1030", features=b"+wavefrontsize64", offsets=True)
        assert [line for _, line in decoded if line.startswith("image_")] == [QUERY]
        expected = translate(lines)
        assert translate(decoded) == expected
        archive, result = directory / f"{stem}.rpf", directory / f"{stem}-cli.hlsl"
        archive.write_bytes(envelope(native))
        subprocess.run([sys.executable, "-B", pixel_aot.__file__, archive, "fixture", "--llvm", library,
                        "--texture-sgpr", "20", "--interp-sgpr", "24", "--sampler-words", "1", "2", "3", "4",
                        "--constant-descriptor", "0x40", "--output", result], check=True)
        assert result.read_text() == expected
    controls()
    return 0


def compare_draws(renderer, backend, mode, operands, shaders, directory, label):
    draws = []
    for name, shader in shaders:
        path = directory / f"{label}-{name}.rgba32f"
        print(f"{label} path={name}", flush=True)
        result = subprocess.run([renderer, backend, mode, shader, *operands, path],
                                capture_output=True, text=True)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            print(result.stderr, end="", flush=True)
        data = path.read_bytes() if path.is_file() else None
        if data is not None:
            assert len(data) == 8 * 8 * 16, len(data)
            first = struct.unpack_from("<4f", data)
            bits = struct.unpack_from("<4I", data)
            print(f"first_pixel_rgba={first} bits={','.join(f'{word:08x}' for word in bits)} "
                  f"readback_sha256={hashlib.sha256(data).hexdigest()}", flush=True)
        draws.append((result.returncode, data))

    (translated_code, translated), (direct_code, direct) = draws
    if translated is not None and direct is not None:
        if translated == direct:
            print(f"{label} translated/direct readbacks identical (256 FP32 channels)", flush=True)
        else:
            for index, (left, right) in enumerate(zip(struct.iter_unpack("<I", translated),
                                                      struct.iter_unpack("<I", direct))):
                if left != right:
                    y, x = divmod(index // 4, 8)
                    print(f"{label} first translated/direct mismatch x={x} y={y} channel={index % 4} "
                          f"translated={left[0]:08x} direct={right[0]:08x}", flush=True)
                    break
            return 1
    if any(code not in (0, 77) for code in (translated_code, direct_code)):
        return 1
    if translated_code == 77 or direct_code == 77:
        return 77
    assert translated is not None and direct is not None, "Missing GPU readback"
    return 0


def draw(directory, compiler, renderer, backend):
    controls()
    direct_source = Path(__file__).with_name("pixel_get_lod_direct.hlsl")
    direct_shaders = {}
    for entry in ("lower_scale", "upper_clamp"):
        direct_shaders[entry] = directory / f"direct_{entry}.cso"
        subprocess.run([compiler, "-Gis", "-T", "ps_6_0", "-E", entry,
                        "-Fo", direct_shaders[entry], direct_source], check=True)

    source = translate(program())
    cso = compile_ps(compiler, source, directory, "lod")
    case = directory / "lod.case"
    status = 0
    for scale in (0.5, 0.25, 1.0, 2.0):
        raw_lod = math.log2(scale)
        # The viewport and texture are both 8x8. The point sampler has MinLOD=0,
        # and the view has one mip. Negative derivative LODs must stay negative in Y.
        reference = struct.pack("<4f", 0, raw_lod, 0.5, 1)
        case.write_bytes(struct.pack("<4f", 0, 0, scale, scale) + bytes(240)
                         + reference * 64 + struct.pack("<B4I", 2, 0, 0, 8, 8))
        result = compare_draws(renderer, backend, "--check-pixel-case", [case],
                               [("translated", cso), ("direct", direct_shaders["lower_scale"])],
                               directory, f"scale-{scale}")
        if result == 77:
            return status or 77
        status |= result
        if scale == 0.5:
            mutant = source.replace("CalculateLevelOfDetailUnclamped", "CalculateLevelOfDetail")
            assert mutant != source
            bad_cso = compile_ps(compiler, mutant, directory, "clamp_both")
            bad = subprocess.run([renderer, backend, "--check-pixel-case", bad_cso, case],
                                 capture_output=True, text=True)
            if (bad.returncode != 1 or "pixel_mismatch x=0 y=0" not in bad.stderr
                    or "channel=1 expected=-1 actual=0" not in bad.stderr):
                raise AssertionError(f"Clamping both LODs escaped detection\n{bad.stderr}")
            print(bad.stderr, end="", flush=True)

    upper = translate(upper_clamp_program())
    cso = compile_ps(compiler, upper, directory, "lod_upper")
    mode = "--check-pixel-get-lod-upper"
    result = compare_draws(renderer, backend, mode, [],
                           [("translated", cso), ("direct", direct_shaders["upper_clamp"])],
                           directory, "upper-clamp")
    if result == 77:
        return status or 77
    status |= result
    mutant = upper.replace("CalculateLevelOfDetail(", "CalculateLevelOfDetailUnclamped(")
    assert mutant != upper
    bad_cso = compile_ps(compiler, mutant, directory, "unclamp_both")
    mutant_readback = directory / "unclamp-both.rgba32f"
    bad = subprocess.run([renderer, backend, mode, bad_cso, mutant_readback], capture_output=True, text=True)
    if bad.returncode != 1 or "pixel_mismatch x=0 y=0" not in bad.stderr or not mutant_readback.is_file():
        raise AssertionError(f"Removing the upper mip clamp escaped detection\n{bad.stderr}")
    mutated_clamped = struct.unpack_from("<f", mutant_readback.read_bytes(), 4)[0]
    if not (abs(mutated_clamped - 2.0) <= 1 / 256):
        raise AssertionError(f"Upper mutant clamped output is {mutated_clamped}, expected within 1/256 of 2")
    print(bad.stderr, end="", flush=True)

    # Check the bound even on adapters whose native raw result is exact.
    direct = direct_source.read_text(encoding="ascii")
    expression = "return float4(raw - 2.0,"
    assert direct.count(expression) == 1
    for steps, expected_code in ((1, 0), (2, 1)):
        stem = f"raw_lod_{steps}_steps"
        control = direct.replace(expression, f"return float4(-{steps}.0 / 256.0,")
        hlsl, cso = directory / f"{stem}.hlsl", directory / f"{stem}.cso"
        hlsl.write_text(control, encoding="ascii")
        subprocess.run([compiler, "-Gis", "-T", "ps_6_0", "-E", "upper_clamp", "-Fo", cso, hlsl], check=True)
        probe = subprocess.run([renderer, backend, mode, cso, directory / f"{stem}.rgba32f"],
                               capture_output=True, text=True)
        if probe.returncode != expected_code or (expected_code and
                ("pixel_mismatch x=0 y=0" not in probe.stderr or
                 "channel=0 expected=0 actual=-0.0078125" not in probe.stderr)):
            raise AssertionError(f"Upper raw LOD {steps}-step control did not behave as expected\n{probe.stderr}")
        print(f"{stem}: {'rejected' if expected_code else 'accepted'}", flush=True)
        if expected_code:
            print(probe.stderr, end="", flush=True)
    if status:
        return status
    write_record(os.environ.get("FORM_COVERAGE_RECORD"))
    return 0


if __name__ == "__main__":
    mode, *args = sys.argv[1:]
    out = Path(__file__).resolve().parents[1] / "out"
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pixel-get-lod-", dir=out) as tmp:
        sys.exit({"binary": binary, "draw": draw}[mode](Path(tmp), *args))
