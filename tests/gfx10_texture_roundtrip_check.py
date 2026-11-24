"""Byte-exact synthetic check of the GFX10 4 KiB BC7 surface and mip-tail tools."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bc7_tiles import linearize_4k_s, linearize_4k_s_tail, pack_4k_s_tail, tile_4k_s

PADDING = b"\xff" * 16

# Pixel dimensions, tail slot. Mip 0 is a complete 4 KiB tile; mips 1-6 share one tail.
MIPS = ((64, 64, None), (32, 32, 0), (16, 16, 1), (8, 8, 2),
        (4, 4, 3), (2, 2, 6), (1, 1, 7))


def block_index(x: int, y: int) -> int:
    """Oracle written from the documented order, low to high: y0 y1 x0 x1 y2 x2 y3 x3."""
    order = (("y", 0), ("y", 1), ("x", 0), ("x", 1), ("y", 2), ("x", 2), ("y", 3), ("x", 3))
    value = {"x": x, "y": y}
    return sum(((value[axis] >> bit) & 1) << position for position, (axis, bit) in enumerate(order))


# Oracle slot origins in the 16-by-16 block tile, separate from the tool's table.
ORIGINS = ((8, 0), (4, 8), (0, 12), (0, 8), (4, 4), (4, 0), (0, 4), (0, 0))


def tag(mip: int, x: int, y: int) -> bytes:
    return struct.pack("<4sHHH6x", b"TIL0", mip, x, y)


def mip_linear(mip: int, width: int, height: int) -> bytes:
    columns, rows = -(-width // 4), -(-height // 4)
    return b"".join(tag(mip, x, y) for y in range(rows) for x in range(columns))


def oracle_surface(mip: int, width: int, height: int) -> tuple[bytes, bytes]:
    """Build the padded tile and linear rows from tags and the independent bit order."""
    columns, rows = -(-width // 4), -(-height // 4)
    tile_pitch = -(-columns // 16)
    payload = bytearray(PADDING * (tile_pitch * -(-rows // 16) * 256))
    linear = bytearray()
    for y in range(rows):
        for x in range(columns):
            block = tag(mip, x, y)
            tile = (y // 16) * tile_pitch + x // 16
            start = tile * 4096 + block_index(x % 16, y % 16) * 16
            payload[start:start + 16] = block
            linear += block
    return bytes(payload), bytes(linear)


def oracle_tail(entries) -> bytes:
    payload = bytearray(PADDING * 256)
    for mip, width, height, level in entries:
        columns, rows = -(-width // 4), -(-height // 4)
        x0, y0 = ORIGINS[level]
        for row in range(rows):
            for column in range(columns):
                start = block_index(x0 + column, y0 + row) * 16
                payload[start:start + 16] = tag(mip, column, row)
    return bytes(payload)


def first_mismatch(actual: bytes, expected: bytes, columns: int) -> tuple[int, int] | None:
    for offset in range(0, max(len(actual), len(expected)), 16):
        if actual[offset:offset + 16] != expected[offset:offset + 16]:
            return (offset // 16) % columns, (offset // 16) // columns
    return None


class RoundTripChecks(unittest.TestCase):
    def test_mip0_is_one_tile_and_matches_oracle(self) -> None:
        payload, linear = oracle_surface(0, 64, 64)
        self.assertEqual(len(payload), 4096)
        built = tile_4k_s(linear, 64, 64)
        self.assertEqual(len(built), 4096)
        self.assertEqual(built, payload)
        self.assertEqual(linearize_4k_s(payload, 64, 64), linear)
        self.assertEqual(tile_4k_s(linearize_4k_s(payload, 64, 64), 64, 64), payload)
        self.assertEqual(tile_4k_s(memoryview(linear), 64, 64), payload)

    def test_seven_mip_chain_is_eight_kib(self) -> None:
        mip0_payload, mip0_linear = oracle_surface(0, 64, 64)
        entries = [(mip, width, height, level) for mip, (width, height, level) in enumerate(MIPS[1:], 1)]
        tail = pack_4k_s_tail([(mip_linear(*entry[:3]), entry[1], entry[2], entry[3]) for entry in entries])
        self.assertEqual(len(tail), 4096)
        self.assertEqual(tail, oracle_tail(entries))

        for mip, width, height, level in entries:
            expected = mip_linear(mip, width, height)
            self.assertEqual(linearize_4k_s_tail(tail, width, height, level), expected)
        full = mip0_payload + tail
        self.assertEqual(len(full), 8192)
        self.assertEqual(full, mip0_payload + oracle_tail(entries))
        self.assertEqual(linearize_4k_s(full[:4096], 64, 64), mip0_linear)
        self.assertEqual(tail[255 * 16:256 * 16], PADDING)

    def test_tail_can_be_repacked_in_place(self) -> None:
        entries = [(mip, width, height, level) for mip, (width, height, level) in enumerate(MIPS[1:], 1)]
        levels = [(mip_linear(*entry[:3]), entry[1], entry[2], entry[3]) for entry in entries]
        tail = pack_4k_s_tail(levels)
        self.assertEqual(pack_4k_s_tail(levels, tile=tail), tail)
        self.assertEqual(pack_4k_s_tail(levels[3:], tile=tail), tail)


class ControlChecks(unittest.TestCase):
    def test_pitched_source_and_wrong_stride(self) -> None:
        rows = [b"".join(tag(0, x, y) for x in range(21)) + PADDING for y in range(18)]
        pitched = b"".join(rows)
        tight = b"".join(row[:21 * 16] for row in rows)
        built = tile_4k_s(pitched, 81, 69, pitch_blocks=22)
        self.assertEqual(linearize_4k_s(built, 81, 69), tight)
        self.assertEqual(tile_4k_s(linearize_4k_s(built, 81, 69), 81, 69), built)

        wrong = tile_4k_s(pitched, 81, 69, pitch_blocks=21)
        self.assertEqual(first_mismatch(linearize_4k_s(wrong, 81, 69), tight, 21), (0, 1))
        with self.assertRaises(ValueError):
            tile_4k_s(pitched, 81, 69, pitch_blocks=20)
        with self.assertRaises(ValueError):
            tile_4k_s(pitched, 81, 69)

    def test_tail_slot_controls(self) -> None:
        entries = [(mip, width, height, level) for mip, (width, height, level) in enumerate(MIPS[1:], 1)]
        tail = pack_4k_s_tail([(mip_linear(*entry[:3]), entry[1], entry[2], entry[3]) for entry in entries])
        with self.assertRaises(ValueError):
            linearize_4k_s_tail(tail, 32, 32, 1)
        with self.assertRaises(ValueError):
            pack_4k_s_tail([(mip_linear(1, 32, 32), 32, 32, 1)])

        wrong = pack_4k_s_tail([(mip_linear(1, 16, 16), 16, 16, 0)])
        self.assertEqual(first_mismatch(linearize_4k_s_tail(wrong, 16, 16, 1),
                                        mip_linear(1, 16, 16), 4), (0, 0))

    def test_tail_length_and_overlap_controls(self) -> None:
        for size in (4095, 4097):
            with self.assertRaises(ValueError):
                linearize_4k_s_tail(bytes(size), 1, 1, 7)
            with self.assertRaises(ValueError):
                pack_4k_s_tail([], tile=bytes(size))
        with self.assertRaises(ValueError):
            pack_4k_s_tail([(mip_linear(1, 32, 32), 32, 32, 0),
                            (mip_linear(2, 16, 16), 16, 16, 0)])


def main() -> int:
    scenario = sys.argv[1] if len(sys.argv) > 1 else "all"
    if scenario not in ("all", "roundtrip", "controls"):
        print("usage: gfx10_texture_roundtrip_check.py [all|roundtrip|controls]", file=sys.stderr)
        return 2
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    if scenario in ("all", "roundtrip"):
        suite.addTests(loader.loadTestsFromTestCase(RoundTripChecks))
    if scenario in ("all", "controls"):
        suite.addTests(loader.loadTestsFromTestCase(ControlChecks))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f"gfx10_texture_roundtrip {scenario}: {'PASS' if result.wasSuccessful() else 'FAIL'}")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
