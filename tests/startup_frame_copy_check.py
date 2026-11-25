"""A constant buffer the title rewrites between two draws of one frame reaches the second draw with its new contents.

Upload ring (wrap mode, a 16 KiB test ring): 64 more copies than the ring holds, all in one list, must not overwrite the
first draw's copy (chain, the default); the legacy ring (X2W_UPLOAD_RING=legacy, the negative control) does overwrite it;
a chain capped at 2 chunks refuses the copies past the cap and does not issue the draw that needed one.
"""
from pathlib import Path
import sys
import tempfile

from startup_address_srv_check import draw_texture

HLSL = (
    "ByteAddressBuffer constants : register(t0);\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "  return asfloat(constants.Load4(0));\n"
    "}\n"
)
RED = bytes((255, 0, 0, 255))
BLUE = bytes((0, 0, 255, 255))
GREEN = bytes((0, 255, 0, 255))


def run(directory, consumer, backend, mode, name=None, env=None):
    folder = directory / (name or mode)
    folder.mkdir()
    log = folder / "trap.log"
    pixels = draw_texture(folder, consumer, backend, HLSL,
                          {"X2W_TEST_FRAME_COPY": mode, "X2W_TRAP_LOG": str(log), **(env or {})},
                          binding="srv 0 buf table 0 0")
    lines = log.read_text(errors="replace").splitlines() if log.exists() else []
    return pixels, lines


def check(directory, consumer, backend):
    # The left half of the 8x4 target is drawn first, the right half second.
    cases = (
        ("changed", (RED * 4 + BLUE * 4) * 4, 1),  # rewritten in place: the second draw must see blue
        ("same", RED * 8 * 4, 0),                  # untouched: both draws red, no new copy
    )
    for mode, expected, refreshed_lines in cases:
        pixels, lines = run(directory, consumer, backend, mode)
        if pixels is None:
            return 77
        refreshed = [line for line in lines if "frame copy of" in line]
        if pixels != expected:
            raise AssertionError(f"{mode}: actual {pixels.hex()} expected {expected.hex()}")
        if len(refreshed) != refreshed_lines:
            raise AssertionError(f"{mode}: {len(refreshed)} refresh log lines, expected {refreshed_lines}:\n" + "\n".join(refreshed))
        print(f"{backend} {mode}: 32 pixels exact, {refreshed_lines} refresh log lines")

    ring = {"X2W_UPLOAD_RING_TEST_KIB": "16"}
    wanted = (RED * 4 + GREEN * 4) * 4

    def has(lines, text):
        return any(text in line for line in lines)

    # Chain (default): the first draw keeps its own bytes although 64 later copies (32 KiB) pass the 16 KiB ring size.
    pixels, lines = run(directory, consumer, backend, "wrap", "wrap_chain", ring)
    if pixels is None:
        return 77
    if pixels != wanted:
        raise AssertionError(f"wrap chain: actual {pixels.hex()} expected {wanted.hex()}")
    if not has(lines, "x2w-upload ring: chain") or not has(lines, "x2w-upload ring (chain): first copy past the old ring's size"):
        raise AssertionError("wrap chain: missing the chain / would-have-wrapped log lines:\n" + "\n".join(l for l in lines if "upload ring" in l))
    print(f"{backend} wrap chain: 32 pixels exact (left red after 64 more copies than the test ring holds); would-have-wrapped logged")

    # Negative control: the legacy ring reuses the first draw's slot before the list has executed.
    pixels, lines = run(directory, consumer, backend, "wrap", "wrap_legacy", {**ring, "X2W_UPLOAD_RING": "legacy"})
    if pixels is None:
        return 77
    left = b"".join(pixels[row * 32: row * 32 + 16] for row in range(4))
    if left == RED * 16:
        raise AssertionError("wrap legacy (negative control): the left half is still red; the control did not trip")
    if not has(lines, "x2w-upload ring (legacy): first copy written over ring data the GPU had not read yet"):
        raise AssertionError("wrap legacy: no overwrite log line")
    print(f"{backend} wrap legacy (negative control): left half {left[:4].hex()} instead of red; overwrite logged")

    # Cap: 2 chunks of 16 KiB hold 64 copies; the rest are refused, and the green draw that needed one is not issued.
    pixels, lines = run(directory, consumer, backend, "wrap", "wrap_cap", {**ring, "X2W_UPLOAD_RING_TEST_CAP_CHUNKS": "2"})
    if pixels is None:
        return 77
    capped = (RED * 4 + BLUE * 4) * 4
    if pixels != capped:
        raise AssertionError(f"wrap cap: actual {pixels.hex()} expected {capped.hex()} (left red, right not drawn)")
    if not has(lines, "is free yet; the copy fails") or not has(lines, "not issued at frame"):
        raise AssertionError("wrap cap: missing the refusal / draw-not-issued log lines")
    print(f"{backend} wrap cap: left red, right not drawn (clear colour); refusal and draw-not-issued logged")

    # Reuse across two submits: list B binds list A's cached red copy (chunk 0, fence passed) again, then 64 fresh
    # copies recycle A's chunks. Chunk 0 must be pinned again by that cache hit, so B's red draw reads its own bytes.
    def counters(folder):
        path = directory / folder / "ring.txt"
        if not path.exists():
            raise AssertionError(f"{folder}: no ring counters written")
        return {k: int(v) for k, v in (line.split("=") for line in path.read_text().split())}

    def reuse(folder, env):
        target = directory / folder / "ring.txt"
        return run(directory, consumer, backend, "reuse", folder, {**ring, "X2W_TEST_RING_COUNTERS": str(target), **env})

    pixels, lines = reuse("reuse_chain", {})
    if pixels is None:
        return 77
    c = counters("reuse_chain")
    chunk = 16 << 10
    if pixels != RED * 8 * 4:
        raise AssertionError(f"reuse chain: actual {pixels.hex()} expected all red; counters {c}")
    if c["repinned"] < 1 or not has(lines, "first cached copy bound again from a chunk waiting for its fences"):
        raise AssertionError(f"reuse chain: the cache hit did not re-pin a fenced chunk; counters {c}")
    if c["reserved"] <= c["made"] * chunk:
        raise AssertionError(f"reuse chain: no chunk was recycled (reserved {c['reserved']} bytes in {c['made']} chunks)")
    if c["refused"]:
        raise AssertionError(f"reuse chain: copies refused; counters {c}")
    print(f"{backend} reuse chain: 32 pixels red (the second list's cache hit kept its chunk); {c['reserved'] >> 10} KiB "
          f"reserved in {c['made']} chunks of 16 KiB (recycled), re-pinned {c['repinned']}")

    # Negative control: without the re-pin the recycled chunk 0 is overwritten by fillers before list B executes.
    pixels, lines = reuse("reuse_norepin", {"X2W_UPLOAD_RING_TEST_NO_REPIN": "1"})
    if pixels is None:
        return 77
    c = counters("reuse_norepin")
    right = b"".join(pixels[row * 32 + 16: row * 32 + 32] for row in range(4))
    if right == RED * 16:
        raise AssertionError(f"reuse no-repin (negative control): the right half is still red; the control did not trip; counters {c}")
    if c["repinned"]:
        raise AssertionError(f"reuse no-repin: re-pinned {c['repinned']} although the switch is set")
    print(f"{backend} reuse no-repin (negative control): right half {right[:4].hex()} instead of red; "
          f"{c['reserved'] >> 10} KiB in {c['made']} chunks")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
