"""Free-list buffer entries and slot-70 buffers without a resource: X2W_BUFFER_HEAP_INDEX=1 (LOCAL textures,
src/startup_stubs/d3d12_draw.inl buffer_heap_index_on, d3d12_memory_srv.inl).

tests/startup_draw_uav.cpp modes membuf*: the view heap's memory starts as the title's free list (slot M holds M + 1).
Slot 1 is a decoy raw buffer view (first dword 55): slot N of the link 1. Slot 0 is a memory SRV (device slot 70) of
private memory that no resource covers (first dword 99), or stays free (membuf-link). The title copies slot 0 to byte 128 of the heap
with its own memory copy, and a translated draw reads dword 0 of `srv 0 buf table 0 128` as its colour.
X2W_DRAW_NULL_IDENTITY=1 is set in every mode, as in run 109: a null buffer then reads 1.0f (0x3f800000).

- membuf (switch on): slot 0 gets the console V# of that memory (no stand-in wrapper: run 110), the copy carries it,
  and the draw reads 99.
- membuf-off (control, switch off): slot 70 refuses buffer_unavailable, slot 0 keeps its link 1, the entry is
  "00000001 0 0 0", and the draw reads the identity fill.
- membuf-link (switch on, no slot-70 call): the entry is a free link and binds null (identity fill), and the log has the
  srv-heapbuf line with slots N-1 and N.
- membuf-structured / membuf-typed (switch on): the same with a structured (stride 16) and a typed (R32_UINT) view, the
  view forms of run 109's refusals: 99.
- membuf-refused (switch on): the slot-70 call has a null address and is refused (null_address); its destination gets
  the refused-view marker, the copied entry carries it, binds null, and the srv-heapbuf line names the reason.
- membuf-unaligned (switch on): as run 112's remaining buffer_unavailable calls, a placed buffer covers the memory but
  the view starts 20 bytes in with 12-byte records (no FirstElement names it): the V# is written anyway, 99, and the
  counter line names the case. membuf-unaligned-off (control, switch off): refused buffer_unavailable, identity fill.
- In no mode does the draw read the decoy at slot N (55).
"""
from pathlib import Path
import re
import sys
import tempfile

from startup_draw_uav_check import run_mode

IDENTITY = bytes((0, 0, 0x80, 0x3F))
VALUE = bytes((99, 0, 0, 0))
DECOY = bytes((55, 0, 0, 0))
ON = {"X2W_BUFFER_HEAP_INDEX": "1"}
MODES = {"membuf": ON, "membuf-off": {}, "membuf-link": ON, "membuf-structured": ON, "membuf-typed": ON, "membuf-refused": ON,
         "membuf-unaligned": ON, "membuf-unaligned-off": {}}


def columns(image):
    """Pixels (x, y) for x < 2 of the 8x4 RGBA8 read-back: what the two strips drew."""
    return {image[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4] for y in range(4) for x in range(2)}


def check(directory, consumer, backend):
    results = {}
    for mode, env in MODES.items():
        got = run_mode(directory, consumer, backend, mode, dict(env, X2W_DRAW_NULL_IDENTITY="1"))
        if got is None:
            print("no existing-heap support or no adapter: skip")
            return 77
        results[mode] = got
    for mode, (_, image, log) in results.items():
        if DECOY in columns(image):
            raise AssertionError(f"{mode}: the draw read the decoy at slot N (55)\n{log[-3000:]}")
    _, image, log = results["membuf"]
    if columns(image) != {VALUE}:
        raise AssertionError(f"membuf: pixels {image[:32].hex()} (expected 99 = {VALUE.hex()})\n{log[-4000:]}")
    # The destination holds the console V# (base = the private memory, so word 0 is neither a link, a token nor the
    # marker); no stand-in wrapper is made (run 110).
    for mode, layout in (("membuf", "stride 0, records 256, word 3 0x"), ("membuf-structured", "stride 16, records 4, word 3 0x"),
                         ("membuf-typed", "stride 4, records 64, word 3 0x"),
                         ("membuf-unaligned", "stride 12, records 4, word 3 0x")):
        _, image, log = results[mode]
        words = re.search(r"^membuf entry words: (\w+) (\w+) (\w+) (\w+)$", log, re.M)
        if columns(image) != {VALUE} or not words or words.group(1) in ("00000001", "56573258", "52463258") or words.group(4) == "00000000":
            raise AssertionError(f"{mode}: pixels {image[:32].hex()} (expected 99), entry {words.group(0) if words else None}\n{log[-4000:]}")
        written = re.search(r"^x2w-memsrv buffer descriptor written at .*$", log, re.M)
        if "X2W_BUFFER_HEAP_INDEX=1" not in log or not written or layout not in written.group(0) or "stand-in buffer at" in log:
            raise AssertionError(f"{mode}: descriptor line {written.group(0) if written else None} (expected {layout!r}), or a stand-in\n{log[-4000:]}")
    _, image, log = results["membuf-unaligned"]
    if "(a buffer is there, at an offset the view cannot name)" not in log \
            or "buffer descriptors: written 1 (no buffer 0, buffer at an offset the view cannot name 1)" not in log:
        raise AssertionError(f"membuf-unaligned: the found-buffer case is not named\n{log[-4000:]}")
    _, image, log = results["membuf-unaligned-off"]
    if columns(image) != {IDENTITY} or "reason=buffer_unavailable" not in log or "buffer descriptor written" in log:
        raise AssertionError(f"membuf-unaligned-off: pixels {image[:32].hex()} (expected the identity fill)\n{log[-4000:]}")
    _, image, log = results["membuf-refused"]
    line = re.search(r"^x2w-frame srv-heapbuf .*$", log, re.M)
    if columns(image) != {IDENTITY} or "membuf entry words: 52463258" not in log or not line \
            or "(refused: null_address)" not in line.group(0) \
            or not re.search(r"refused view: fmt \d+ layout 0x[0-9a-f]+ dim 1 mips \d+ [1-9]\d*x1x1 address 0x0 unreadable, detile: -$",
                             line.group(0)):
        raise AssertionError(f"membuf-refused: pixels {image[:32].hex()}, line {line.group(0) if line else None}\n{log[-4000:]}")
    _, image, log = results["membuf-off"]
    if columns(image) != {IDENTITY} or "membuf entry words: 00000001 00000000 00000000 00000000" not in log \
            or "reason=buffer_unavailable" not in log or "X2W_BUFFER_HEAP_INDEX" in log:
        raise AssertionError(f"membuf-off: pixels {image[:32].hex()} (expected the identity fill {IDENTITY.hex()})\n{log[-4000:]}")
    _, image, log = results["membuf-link"]
    line = re.search(r"^x2w-frame srv-heapbuf .*$", log, re.M)
    if columns(image) != {IDENTITY} or not line or "(free link)" not in line.group(0) \
            or "slot N-1 0x0 words 00000001" not in line.group(0) or "slot N 0x1 token 56573258" not in line.group(0):
        raise AssertionError(f"membuf-link: pixels {image[:32].hex()}, line {line.group(0) if line else None}\n{log[-4000:]}")
    print(f"{backend}: slot-70 buffer without a resource read through a copied entry (99) with X2W_BUFFER_HEAP_INDEX; "
          f"switch off: link 1 copied, identity fill; free link: bound null and logged with slots N-1 / N; decoy at slot N never read; structured and typed views 99; a refused call's marker is carried, bound null and named")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
