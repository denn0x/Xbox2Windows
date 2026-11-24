"""Translated draws with UAV stores and resource-heap reads (src/startup_stubs/d3d12_draw.inl, LOCAL draw-rt).

A synthetic pipeline (tools/startup/gfx2hlsl.py vertex stage, hand-written ps_5_1 pixel stage) gets extra
manifest lines; the fixture (tests/startup_draw_uav.cpp) binds a one-entry view table and draws two
one-column strips (columns 0 and 1).

- aligned / misaligned / uavbase: `uav 0 buf table 0 0` and `srv 0 buf table 0 0` name the same hardware
  buffer descriptor (title + 64 or + 68 of a private allocation, word 3 = MARK), plus `udsrc uavword3`,
  `udsrc bufword3` and (uavbase) `udsrc uavbase`. The fixture prints the title memory after the GPU ran.
- tex / tex-noflag / tex-uint: `uav 0 tex2d table 0 0` on a UAV-capable RGBA8 texture (drawn), a texture
  without unordered access (skipped) and an R8G8B8A8_UINT view (skipped).
- heap: `heap 1`; the pixel stage reads the view id from heapmem and the raw buffer of that view (dword 77).
- copy-same / copy-two / copy-none / copy-cpu: CopyBufferRegion (list slot 15) between the two draws. uav 0 = title + 64
  of allocation A (entry 0), srv 0 = title + 0x400 of A (copy-same) or of allocation B (copy-two) (entry 1). Draw 1
  stores into A, the list copies A + 64 (128 bytes) to the srv range, draw 2 reads the copied values as its srv
  (the constant-buffer path of translated draws: raw srvs). X2W_D3D12_COPY_BUFFER=gpu must order the copy after
  draw 1 on the GPU and let draw 2 read it in place. Controls: copy-none (no copy) and copy-cpu (same list,
  X2W_D3D12_COPY_BUFFER=1: the record-time copy runs before draw 1 has stored anything) must both read zeros.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from startup_stream_draw_check import gfx2hlsl, pixel, vertex

MARK = 0x12345678
CLEAR = bytes((0, 0, 255, 255))


def buffer_hlsl(count, base):
    uds = " ".join(f"uint ud{i};" for i in range(count + (3 if base else 2)))
    offset = f"ud{count + 2} + " if base else ""
    return (
        f"cbuffer entry : register(b0) {{ {uds} }};\n"
        "ByteAddressBuffer t0 : register(t0);\n"
        "RWByteAddressBuffer u0 : register(u0);\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "  uint2 q = uint2(p.xy);\n"
        "  uint i = q.y * 8 + q.x;\n"
        "  uint seen = t0.Load(q.y * 32);\n"  # what an earlier draw stored for column 0 of this row
        f"  uint marks = (ud{count} == {MARK}u ? 0u : 500000u) + (ud{count + 1} == {MARK}u ? 0u : 900000u);\n"
        f"  u0.Store({offset}i * 4, seen + 1000 + i + marks);\n"
        "  return float4(seen ? 1 : 0, 1, 0, 1);\n"
        "}\n"
    )


TEX_HLSL = (
    "RWTexture2D<float4> u0 : register(u0);\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "  u0[uint2(p.xy)] = float4(0.25, 0.5, 0.75, 1);\n"
    "  return float4(0, 1, 0, 1);\n"
    "}\n"
)

HEAP_HLSL = (
    "ByteAddressBuffer heapmem : register(t0, space1);\n"
    "ByteAddressBuffer vinfo : register(t1, space1);\n"
    "ByteAddressBuffer heapbuf[720008] : register(t0, space6);\n"
    "uint vid(uint h) { return heapmem.Load(h) == 0x56573258u ? heapmem.Load(h + 4) : 0xffffffffu; }\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "  uint i = vid(0);\n"
    "  uint k = i < 90000u ? 8u + 8u * i + 4u : 4u;\n"
    "  uint v = heapbuf[NonUniformResourceIndex(k)].Load(0);\n"
    "  return float4(v / 255.0, i < 90000u ? 1 : 0, 0, 1);\n"
    "}\n"
)


COPY_ENV = {"copy-same": "gpu", "copy-two": "gpu", "copy-none": "gpu", "copy-cpu": "1"}


def run_mode(directory, consumer, backend, mode):
    cache = directory / mode / "cache"
    cache.mkdir(parents=True)
    vs, ps, root, output = (directory / mode / name for name in ("vs", "ps", "root", "pixels"))
    vs.write_bytes(vertex())
    ps.write_bytes(pixel(False))
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1",
               X2W_D3D12_DEBUG="0", X2W_D3D12_DEVICE_NOOP="1", X2W_SHADER_CACHE=str(cache),
               X2W_TEST_DRAW_UAV="copy-same" if mode == "copy-cpu" else mode, X2W_TRAP_LOG=str(directory / mode / "trap.log"))
    if mode in COPY_ENV:
        env["X2W_D3D12_COPY_BUFFER"] = COPY_ENV[mode]
    args = [consumer, vs, ps, root]
    subprocess.run(args, env=env, check=True)
    subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], env=env, check=True)
    manifest, = cache.glob("*.txt")
    text = manifest.read_text(encoding="ascii")
    count = len(re.findall(r"^udsrc ", text, re.M))
    if re.search(r"^(srv|uav|heap) ", text, re.M):
        raise AssertionError(f"the synthetic manifest already binds resources:\n{text}")
    if mode.startswith("tex"):
        lines, hlsl = ["uav 0 tex2d table 0 0"], TEX_HLSL
    elif mode == "heap":
        lines, hlsl = ["heap 1"], HEAP_HLSL
    else:
        base = mode == "uavbase"
        lines = ["uav 0 buf table 0 0", "srv 0 buf table 0 32" if mode in COPY_ENV else "srv 0 buf table 0 0", f"udsrc {count} uavword3 0", f"udsrc {count + 1} bufword3 0"]
        if base:
            lines.append(f"udsrc {count + 2} uavbase 0")
        hlsl = buffer_hlsl(count, base)
    manifest.write_text(text.rstrip("\n") + "\n" + "\n".join(lines) + "\n", encoding="ascii")
    source = manifest.with_suffix(".ps.hlsl")
    source.write_text(hlsl)
    subprocess.run([env["X2W_FXC"], "/nologo", "/T", "ps_5_1", "/E", "main", "/Fo", manifest.with_suffix(".ps.cso"), source],
                   check=True, stdout=subprocess.DEVNULL)
    result = subprocess.run([*args, backend, output], env=env, capture_output=True, text=True, timeout=60)
    print(result.stdout, end="")
    if result.returncode == 77:
        return None
    if result.returncode:
        raise RuntimeError(f"{mode}: consumer failed ({result.returncode})\n{result.stderr[-5000:]}")
    found = re.search(r"^uav memory:((?: \d+)+)$", result.stdout, re.M)
    memory = [int(v) for v in found.group(1).split()] if found else None
    copied = re.search(r"^copied:((?: \d+)+)$", result.stdout, re.M)
    if copied:
        memory = (memory, [int(v) for v in copied.group(1).split()])
    return memory, output.read_bytes(), result.stderr + "\n" + result.stdout


def need(log, mode, *needles):
    for needle in needles:
        if needle not in log:
            raise AssertionError(f"{mode}: log lacks {needle!r}\n{log[-4000:]}")


def check(directory, consumer, backend):
    results = {}
    for mode in ("aligned", "misaligned", "uavbase", "tex", "tex-noflag", "tex-uint", "heap", "copy-same", "copy-two", "copy-none", "copy-cpu"):
        got = run_mode(directory, consumer, backend, mode)
        if got is None:
            print("no existing-heap support or no adapter: skip")
            return 77
        results[mode] = got
    # aligned: the view starts at title + 64 (dword 16). Draw 1 stores 1000 + i for column 0; draw 2 reads that
    # value in place (srv over the range draw 1 wrote) and adds it. Word 3 reached both entry values (marks 0).
    memory, image, log = results["aligned"]
    expected = [0] * 64
    for y in range(4):
        expected[16 + y * 8] = 1000 + y * 8
        expected[16 + y * 8 + 1] = (1000 + y * 8) + 1000 + y * 8 + 1
    if memory != expected:
        raise AssertionError(f"aligned: memory {memory}\nexpected {expected}")
    if image[:8] != bytes((0, 255, 0, 255, 255, 255, 0, 255)):
        raise AssertionError(f"aligned: pixels {image[:16].hex()} (column 0 must read 0, column 1 the earlier store)")
    need(log, "aligned", "x2w-draw-rt first draw with uavs", "from descriptor", "bound in place at window", ": bound")
    # misaligned without uavbase: a null view, nothing stored (negative control for the stores above).
    memory, image, log = results["misaligned"]
    if any(memory):
        raise AssertionError(f"misaligned: memory changed {memory}")
    need(log, "misaligned", "NULL (not 16-byte aligned, no uavbase)")
    # uavbase: the view starts at title + 64 and ud carries 4, so the stores land at title + 68 (dword 17).
    # The srv over the written range is misaligned: it keeps the record-time copy (zeros), counted as stale.
    memory, image, log = results["uavbase"]
    expected = [0] * 64
    for y in range(4):
        expected[17 + y * 8] = 1000 + y * 8
        expected[17 + y * 8 + 1] = 1000 + y * 8 + 1
    if memory != expected:
        raise AssertionError(f"uavbase: memory {memory}\nexpected {expected}")
    # tex: drawn (green columns 0 and 1) through a bound RGBA8 texture UAV; the others are skipped (clear).
    green = bytes((0, 255, 0, 255))
    memory, image, log = results["tex"]
    if image[:8] != green * 2 or image[8:12] != CLEAR:
        raise AssertionError(f"tex: pixels {image[:16].hex()}")
    need(log, "tex", "uav 0 texture: ", "format 28 mip 0: bound", "typed UAV store supported", "first draw with uavs")
    for mode, reason in (("tex-noflag", "texture without ALLOW_UNORDERED_ACCESS"),
                         ("tex-uint", "format not a float/unorm format of the texture")):
        memory, image, log = results[mode]
        if image[:8] != CLEAR * 2:
            raise AssertionError(f"{mode}: the draw was not skipped: {image[:16].hex()}")
        need(log, mode, f"draw skipped: texture uav: {reason}")
        if "first draw with uavs" in log:
            raise AssertionError(f"{mode}: a skipped draw was recorded")
    # heap: heapmem gave view id i of entry 0 (green 1) and heapbuf[8 + 8 i + 4] its first dword (77).
    # With a view increment other than the console's 32 bytes the draw must be skipped instead (WARP).
    memory, image, log = results["heap"]
    need(log, "heap", "heap 1 in root parameters")
    increment = int(re.search(r"^view increment (\d+)$", log, re.M).group(1))
    if increment == 32:
        if image[:8] != bytes((77, 255, 0, 255)) * 2:
            raise AssertionError(f"heap: pixels {image[:16].hex()}")
        need(log, "heap", "x2w-draw-rt first heap draw")
        heap = "heap draw reads view id and buffer"
    else:
        if image[:8] != CLEAR * 2:
            raise AssertionError(f"heap: increment {increment} but drawn: {image[:16].hex()}")
        need(log, "heap", "heap draw skipped: descriptor increment is not 32")
        heap = f"heap draw NOT exercised (view increment {increment}; skipped as required)"
    # copy-*: GPU order (copy after draw 1's stores, draw 2 reads the copy in place) against two controls.
    stored = [0] * 64
    for y in range(4):
        stored[16 + y * 8] = 1000 + y * 8
    copied_gpu = [0] * 32
    for y in range(4):
        copied_gpu[y * 8] = 1000 + y * 8
    for mode in ("copy-same", "copy-two", "copy-none", "copy-cpu"):
        (memory, copied), image, log = results[mode]
        gpu = mode in ("copy-same", "copy-two")
        expected = list(stored)
        for y in range(4):
            expected[16 + y * 8 + 1] = ((1000 + y * 8) if gpu else 0) + 1000 + y * 8 + 1
        if memory != expected:
            raise AssertionError(f"{mode}: memory {memory}\nexpected {expected}")
        if copied != (copied_gpu if gpu else [0] * 32):
            raise AssertionError(f"{mode}: copied range {copied}")
        column1 = bytes((255 if gpu else 0, 255, 0, 255))
        if image[:8] != bytes((0, 255, 0, 255)) + column1:
            raise AssertionError(f"{mode}: pixels {image[:16].hex()}")
        if gpu:
            need(log, mode, "runs on the GPU in list order", "x2w-copy-gpu CopyBufferRegion", ": on the GPU, window")
            if "record-time fallback" in log:
                raise AssertionError(f"{mode}: copy fell back\n{log[-3000:]}")
        elif mode == "copy-cpu":
            need(log, mode, "copies title memory at record time")
    print(f"{backend}: CopyBufferRegion on the GPU in list order (same window, two windows): draw 2 reads the bytes draw 1 "
          f"stored; controls without the copy and with the record-time copy read zeros")
    print(f"{backend}: draw UAV buffer stores reach title memory (aligned, in-place re-read, uavbase; misaligned "
          f"control stores nothing); texture UAV bound, two refusals skip the draw; {heap}")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
