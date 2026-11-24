"""Check XG slot 10 for 64 KiB standard layouts (0x10a) answered from the tile plan."""
import ctypes as ct
import struct
import sys

from startup_xg_bc7_layout_check import DESC, HRESULT, PTR, RELEASE, UINT

TILING = ct.WINFUNCTYPE(HRESULT, PTR, PTR, PTR, PTR, PTR, UINT, PTR)
ENTRY = struct.Struct("<IHHI")
SENTINEL = b"\xaa" * ENTRY.size
SLOTS = 40
# (format, width, height, slices, mips): (tile width, tile height, tiles per slice,
# [(tiles wide, tiles high, first tile in the slice) for each standard mip]).
# Written out by hand from the rule in tile64_plan.h; model() below recomputes them.
CASES = {
    (98, 2048, 2048, 1, 10): (256, 256, 86, [(8, 8, 22), (4, 4, 6), (2, 2, 2), (1, 1, 1)]),
    (98, 2048, 1024, 1, 9): (256, 256, 44, [(8, 4, 12), (4, 2, 4), (2, 1, 2), (1, 1, 1)]),
    (98, 1024, 2048, 1, 9): (256, 256, 43, [(4, 8, 11), (2, 4, 3), (1, 2, 1)]),
    (98, 2048, 512, 1, 8): (256, 256, 24, [(8, 2, 8), (4, 1, 4), (2, 1, 2), (1, 1, 1)]),
    (99, 512, 2048, 1, 8): (256, 256, 23, [(2, 8, 7), (1, 4, 3), (1, 2, 1)]),
    (98, 1024, 1024, 1, 3): (256, 256, 21, [(4, 4, 5), (2, 2, 1), (1, 1, 0)]),
    (71, 2048, 2048, 1, 10): (512, 256, 43, [(4, 8, 11), (2, 4, 3), (1, 2, 1)]),
    (72, 4096, 2048, 1, 10): (512, 256, 86, [(8, 8, 22), (4, 4, 6), (2, 2, 2), (1, 1, 1)]),
    (71, 256, 256, 64, 9): (512, 256, 1, []),
    (71, 512, 512, 3, 8): (512, 256, 3, [(1, 2, 1)]),
    (83, 256, 256, 8, 9): (256, 256, 2, [(1, 1, 1)]),
    (83, 512, 512, 3, 8): (256, 256, 6, [(2, 2, 2), (1, 1, 1)]),
    (77, 1024, 128, 1, 1): (256, 256, 4, [(4, 1, 0)]),
    (28, 1024, 1024, 1, 9): (128, 128, 86, [(8, 8, 22), (4, 4, 6), (2, 2, 2), (1, 1, 1)]),
    (28, 256, 256, 2, 7): (128, 128, 6, [(2, 2, 2), (1, 1, 1)]),
    (29, 128, 256, 8, 1): (128, 128, 2, [(1, 2, 0)]),
    (65, 256, 1024, 1, 1): (256, 256, 4, [(1, 4, 0)]),
    (54, 256, 256, 1, 1): (256, 128, 2, [(1, 2, 0)]),
}
SPANS = [(0, 16), (0, 2), (1, 3), (0, 0), (0, 0xffffffff), (0xffffffff, 4)]


def model(width, height, mips, tile_width, tile_height):
    """Tiles per slice and (wide, high, start) per standard mip, largest mip first."""
    grids = []
    for mip in range(mips):
        w, h = max(1, width >> mip), max(1, height >> mip)
        if w * 2 <= tile_width and h <= tile_height:
            break
        grids.append((-(-w // tile_width), -(-h // tile_height)))
    total = 1 if len(grids) < mips else 0
    starts = []
    for wide, high in reversed(grids):
        starts.append(total)
        total += wide * high
    return total, [(wide, high, start) for (wide, high), start in zip(grids, reversed(starts))]


def query(fn, instance, case, first, asked):
    fmt, width, height, slices, mips = case
    tile_width, tile_height, stride, standard = CASES[case]
    expected = bytearray(SENTINEL * SLOTS)
    expected_count = 0
    for i in range(min(asked, SLOTS, max(0, mips * slices - first))):
        array_slice, mip = divmod(first + i, mips)
        if mip < len(standard):
            wide, high, start = standard[mip]
            ENTRY.pack_into(expected, i * ENTRY.size, wide, high, 1, start + array_slice * stride)
            expected_count += 1

    output = ct.create_string_buffer(SENTINEL * SLOTS, SLOTS * ENTRY.size)
    count = UINT(min(asked, SLOTS))
    total = UINT(0)
    packed = ct.create_string_buffer(12)
    shape = (UINT * 3)()
    fn(instance, ct.byref(total), packed, shape, ct.byref(count), first, output)
    assert total.value == stride * slices, (case, total.value)
    assert struct.unpack("<BB2xII", packed.raw) == (len(standard), mips - len(standard), 1 if len(standard) < mips else 0, 0), case
    assert tuple(shape) == (tile_width, tile_height, 1), case
    assert (count.value, output.raw) == (expected_count, bytes(expected)), (case, first, asked)
    return output.raw


def main(path):
    for case, (tile_width, tile_height, stride, standard) in CASES.items():
        assert model(case[1], case[2], case[4], tile_width, tile_height) == (stride, standard), case
    # Control: the smallest standard mip comes first, so the starts fall as the mip gets larger.
    # A largest-mip-first table (the desktop reserved-resource order) must not pass.
    desktop_order = [(8, 8, 0), (4, 4, 64), (2, 2, 80), (1, 1, 84)]
    assert CASES[(98, 2048, 2048, 1, 10)][3] != desktop_order

    create = ct.WinDLL(path).XGCreateTextureComputer
    create.argtypes = (PTR, ct.POINTER(PTR), ct.c_uint64, ct.c_uint64)
    create.restype = HRESULT
    queries = 0
    for case in CASES:
        fmt, width, height, slices, mips = case
        desc = ct.create_string_buffer(DESC.pack(3, 0, width, height, slices, mips, fmt, 1, 0, 0x10a, 0x300000))
        computer = PTR()
        assert create(desc, ct.byref(computer), 0, 0) == 0 and computer
        slots = ct.cast(computer, ct.POINTER(ct.POINTER(PTR)))[0]
        try:
            fn = TILING(slots[10])
            spans = SPANS + ([(mips - 1, mips + 2), (mips * slices - 1, 4)] if slices > 1 else [])
            for first, asked in spans:
                raw = query(fn, computer, case, first, asked)
                queries += 1
            if case == (98, 2048, 2048, 1, 10):
                wrong = b"".join(ENTRY.pack(wide, high, 1, start) for wide, high, start in desktop_order)
                assert query(fn, computer, case, 0, 4)[:len(wrong)] != wrong
        finally:
            RELEASE(slots[1])(computer)
    print(f"startup_xg_tile64_tiling: {len(CASES)} shapes, {queries} queries")


if __name__ == "__main__":
    main(sys.argv[1])
