"""Check XG slot 10 for bounded BC5/BC7 chains, including untouched packed slots."""
import ctypes as ct
import struct
import sys

from startup_xg_bc7_layout_check import DESC, HRESULT, PTR, RELEASE, UINT

TILING = ct.WINFUNCTYPE(HRESULT, PTR, PTR, PTR, PTR, PTR, UINT, PTR)
ENTRY = struct.Struct("<IHHI")
SENTINEL = b"\xaa" * ENTRY.size
# Standard mip dimensions in tiles and their first tile within a slice.
CASES = {
    512: (12, [(4, 2, 1, 4), (2, 1, 1, 2), (1, 1, 1, 1)]),
    1024: (22, [(4, 4, 1, 6), (2, 2, 1, 2), (1, 1, 1, 1)]),
}


def query(fn, instance, height, slices, first, asked):
    stride, standard = CASES[height]
    expected = bytearray(SENTINEL * 20)
    expected_count = 0
    for i in range(min(asked, max(0, 9 * slices - first))):
        array_slice, mip = divmod(first + i, 9)
        if mip < len(standard):
            w, h, depth, start = standard[mip]
            ENTRY.pack_into(expected, i * ENTRY.size, w, h, depth, start + array_slice * stride)
            expected_count += 1

    output = ct.create_string_buffer(SENTINEL * 20, 20 * ENTRY.size)
    count = UINT(asked)
    total = UINT(0)
    packed = ct.create_string_buffer(12)
    shape = (UINT * 3)()
    assert fn(instance, ct.byref(total), packed, shape, ct.byref(count), first, output) == 0
    assert total.value == stride * slices
    assert struct.unpack("<BB2xII", packed.raw) == (3, 6, 1, 0)
    assert tuple(shape) == (256, 256, 1)
    assert (count.value, output.raw) == (expected_count, expected), (height, slices, first, asked)
    count.value = asked
    assert fn(instance, None, None, None, ct.byref(count), first, None) == 0
    assert count.value == expected_count
    assert fn(instance, None, None, None, None, first, output) == 0
    assert output.raw == expected


def main(path):
    create = ct.WinDLL(path).XGCreateTextureComputer
    create.argtypes = (PTR, ct.POINTER(PTR), ct.c_uint64, ct.c_uint64)
    create.restype = HRESULT
    spans = [(0, 9), (0, 2), (2, 4), (3, 4), (8, 4), (17, 8), (18, 8),
             (0xffffffff, 4), (0, 0), (0, 0xffffffff)]
    for fmt in (83, 98):
        for height in CASES:
            for slices in (1, 2):
                desc = ct.create_string_buffer(DESC.pack(3, 0, 1024, height, slices, 9, fmt, 1, 0, 0x109, 0))
                computer = PTR()
                assert create(desc, ct.byref(computer), 0, 0) == 0 and computer
                slots = ct.cast(computer, ct.POINTER(ct.POINTER(PTR)))[0]
                try:
                    for first, asked in spans:
                        query(TILING(slots[10]), computer, height, slices, first, asked)
                finally:
                    RELEASE(slots[1])(computer)
    print("80 BC5/BC7 subresource queries matched counts, starts and sentinel bytes")


if __name__ == "__main__":
    main(sys.argv[1])
