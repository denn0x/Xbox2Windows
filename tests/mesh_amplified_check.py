"""Create an amplification plus mesh pipeline and check one deterministic draw.

The amplification shader dispatches a single mesh group and passes the depth
word it computed in the payload. The mesh shader exports the trace layout used
by the translated mesh fixtures, so the CPU reference compares the same 80-byte
trace and 32x32 image. A mesh-only draw through --mesh-entry is the positive
control for the pipeline form without an amplification stage.
"""

from pathlib import Path
import subprocess
import sys
import tempfile

from mesh_entry_check import TRIANGLE, fixture, triangle_inputs

PIXEL = "float4 main() : SV_Target { return float4(0,1,0,1); }"

AS_SOURCE = """
struct Payload { uint depth_bits; uint group; };

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Payload p;
    p.depth_bits = 0x3f000000u;  // 0.5f
    p.group = id.x + 1u;
    DispatchMesh(1, 1, 1, p);
}
""".strip() + "\n"

AS_WRONG_DEPTH = AS_SOURCE.replace("0x3f000000u", "0x3f400000u")  # 0.75f
assert "0x3f400000u" in AS_WRONG_DEPTH

MS_SOURCE = """
struct Payload { uint depth_bits; uint group; };
RWByteAddressBuffer trace : register(u0);
struct Vertex { float4 position : SV_Position; };

[outputtopology("triangle")]
[numthreads(3,1,1)]
void main(uint id : SV_GroupIndex, in payload Payload p,
          out vertices Vertex v[3], out indices uint3 tri[1])
{
    SetMeshOutputCounts(3, 1);
    // Subtracting one leaves the AS word untouched only when the payload arrived.
    float z = asfloat(p.depth_bits + p.group - 1u);
    float4 q = float4(id == 1 ? 3.0f : -1.0f, id == 2 ? 3.0f : -1.0f, z, 1.0f);
    v[id].position = q;
    trace.Store4(16 + id * 16, asuint(q));
    if (id == 0) {
        uint3 t = uint3(0, 1, 2);
        tri[0] = t;
        trace.Store2(0, uint2(3, 1));
        trace.Store3(64, t);
    }
}
""".strip() + "\n"

MESH_ONLY_SOURCE = """
RWByteAddressBuffer trace : register(u0);
struct Vertex { float4 position : SV_Position; };

[outputtopology("triangle")]
[numthreads(3,1,1)]
void main(uint id : SV_GroupIndex, out vertices Vertex v[3], out indices uint3 tri[1])
{
    SetMeshOutputCounts(3, 1);
    float4 q = float4(id == 1 ? 3.0f : -1.0f, id == 2 ? 3.0f : -1.0f, 0.5f, 1.0f);
    v[id].position = q;
    trace.Store4(16 + id * 16, asuint(q));
    if (id == 0) {
        uint3 t = uint3(0, 1, 2);
        tri[0] = t;
        trace.Store2(0, uint2(3, 1));
        trace.Store3(64, t);
    }
}
""".strip() + "\n"


def compile(dxc, work, name, target, source):
    hlsl = work / f"{name}.hlsl"
    hlsl.write_text(source)
    dxil = work / f"{name}.dxil"
    subprocess.run([dxc, "-T", target, "-E", "main", "-Fo", dxil, hlsl], check=True)
    return dxil


def run(renderer, adapter, operation, shaders, data):
    args = [renderer, adapter, operation, *map(str, shaders), str(data)]
    return subprocess.run(args, capture_output=True, text=True)


def refused(result, message, label):
    if result.returncode == 77:
        return 77
    if result.returncode != 1 or message not in result.stderr:
        raise AssertionError(
            f"{label}: expected exit 1 and {message!r}, got {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return 0


def main(dxc, renderer, adapter):
    with tempfile.TemporaryDirectory(prefix="mesh-amplified-") as directory:
        work = Path(directory)
        pixel = compile(dxc, work, "pixel", "ps_6_0", PIXEL)
        amplification = compile(dxc, work, "as", "as_6_5", AS_SOURCE)
        wrong_depth = compile(dxc, work, "as_wrong", "as_6_5", AS_WRONG_DEPTH)
        mesh = compile(dxc, work, "ms", "ms_6_5", MS_SOURCE)
        mesh_only = compile(dxc, work, "ms_only", "ms_6_5", MESH_ONLY_SOURCE)
        expected = fixture(triangle_inputs(), TRIANGLE, True, True)
        reference = work / "reference.bin"
        reference.write_bytes(expected)
        black = work / "black.bin"
        black.write_bytes(fixture(triangle_inputs(), TRIANGLE, False, True))

        for label, operation, shaders in (
                ("mesh-only positive", "--mesh-entry", (mesh_only, pixel)),
                ("amplification positive", "--mesh-amplified", (amplification, mesh, pixel))):
            print(f"{label}: {operation}", flush=True)
            result = run(renderer, adapter, operation, shaders, reference)
            if result.returncode == 77:
                return 77
            if result.returncode != 0:
                raise AssertionError(f"{operation}: {result.stdout}\n{result.stderr}")
            if "mesh_pipeline_ms=" not in result.stdout:
                raise AssertionError(f"{operation}: no mesh pipeline time\n{result.stdout}")
            if "pixels=1024 mismatches=0" not in result.stdout:
                raise AssertionError(f"{operation}: unexpected readback\n{result.stdout}")
            print(result.stdout, end="")

        # The amplification payload selects the depth: a stale 0.75 word must fail
        # the trace comparison while the raster stays green.
        result = run(renderer, adapter, "--mesh-amplified", (wrong_depth, mesh, pixel), reference)
        if refused(result, "Mesh export trace mismatch", "stale amplification payload"):
            return 77
        if "pixels=1024 mismatches=0" not in result.stdout:
            raise AssertionError(f"stale payload changed a pixel\n{result.stdout}")
        if "first_mesh_export_mismatch_byte=26" not in result.stdout:
            raise AssertionError(f"stale payload did not reach the first depth word\n{result.stdout}")
        print("Expected failure: stale amplification payload, trace mismatch", flush=True)
        # A wrong image leaves the trace intact and reports all 1,024 pixels.
        result = run(renderer, adapter, "--mesh-amplified", (amplification, mesh, pixel), black)
        if refused(result, "Draw reference mismatch", "wrong reference image"):
            return 77
        if "pixels=1024 mismatches=1024" not in result.stdout:
            raise AssertionError(f"wrong reference image did not change every pixel\n{result.stdout}")
        print("Expected failure: wrong reference image, 1024 pixel mismatches", flush=True)
        return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: mesh_amplified_check.py <dxc> <renderer> <hardware|warp>")
    raise SystemExit(main(*sys.argv[1:]))
