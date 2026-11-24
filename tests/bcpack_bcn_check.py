"""Synthetic BCPack BC3/BC4/BC5 streams: encode chosen blocks, decode, compare (no title data).

The test owns its encoder side: it picks block contents (endpoints, ramp positions and the
references each block predicts from), derives the lane symbols from the reference rules, rANS
encodes every lane with the single-lane test encoder from bcpack_symbols_check.py, and interleaves
the lanes in the order the cycle-level decoder requests bytes. Expected blocks are packed here
directly from the chosen contents, not by the decoder under test.

PINNED holds one stream per format produced by this encoder (seed 7); the C++ decoder in
src/startup_stubs/bcpack_decoder.h is checked against the same bytes by
tests/bcpack_bcn_decode_test.cpp, which keeps both implementations in step.
"""
import random
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import bcpack_bcn  # noqa: E402
from bcpack_bcn import BCPackError, LAYOUTS, tables, zigzag  # noqa: E402
from _bcpack_rans import decode_lanes  # noqa: E402
from bcpack_symbols_check import encode_fragment  # noqa: E402

ALPHA_CODE = (0, 2, 3, 4, 5, 6, 7, 1)
COLOUR_CODE = (0, 2, 3, 1)


def signed(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def nibbles(byte):
    even = odd = 0
    for k in range(4):
        even |= ((byte >> (2 * k)) & 1) << k
        odd |= ((byte >> (2 * k + 1)) & 1) << k
    return even, odd


def interleave_lanes(lane_values, alphabet_bits, rng):
    """rANS-encode each lane (one flushed fragment) and interleave in decoder request order."""
    streams = []
    for values, ab in zip(lane_values, alphabet_bits):
        q = rng.randrange(16)
        streams.append(encode_fragment(ab + 1, q, values))
    cursor = [0] * len(streams)

    def source(lane):
        byte = streams[lane][cursor[lane]]
        cursor[lane] += 1
        return byte

    n = len(lane_values[0])
    order = decode_lanes(b"", list(alphabet_bits), [n] * len(alphabet_bits), source=source)["order"]
    if cursor != [len(s) for s in streams]:
        raise AssertionError("encoder schedule did not use every lane byte")
    cursor = [0] * len(streams)
    out = bytearray()
    for lane in order:
        out.append(streams[lane][cursor[lane]])
        cursor[lane] += 1
    return bytes(out)


class Encoder:
    """Build lane symbols for chosen blocks of one format."""

    def __init__(self, fmt, rng):
        self.fmt = fmt
        self.rng = rng
        t = tables()
        name = {3: "BC3", 4: "BC4", 5: "BC5"}[fmt]
        self.dictionary = t[name + "_DICTIONARY"]
        pairs = t["RANK_PAIR_BC3_ALPHA" if fmt == 3 else "RANK_PAIR_" + name]
        self.pair_symbol = {tuple(ab): s for s, ab in enumerate(pairs)}
        self.quad_symbol = {tuple(v): s for s, v in enumerate(t["RANK_QUAD_BC3_COLOUR"])}
        self.ab, self.block, self.per = LAYOUTS[fmt]
        self.lanes = [[] for _ in self.ab]
        self.hist = []
        self.expected = bytearray()
        self.n_ep = {3: 8, 4: 2, 5: 4}[fmt]
        self.n_ix = 16 if fmt == 4 else 32

    def source(self, i, r):
        if r == 0:
            return [0] * self.n_ep, [0] * self.n_ix
        if r <= i:
            return self.hist[i - r]
        ep, ix = self.dictionary[r - i - 1]
        return list(ep), list(ix)

    def put_ref(self, er, xr):
        d = zigzag(signed(er - xr, 8))
        if self.per == 1:
            self.lanes[0].append(d)
            self.lanes[1].append(xr)
        else:
            self.lanes[0].extend(nibbles(d))
            self.lanes[1].extend(nibbles(xr))

    def add(self, er, xr, ep, ix):
        """Append one block with endpoint states `ep`, ramp positions `ix`, references er/xr."""
        i = len(self.hist)
        ep_ref, _ = self.source(i, er)
        _, ix_ref = self.source(i, xr)
        self.put_ref(er, xr)
        widths = {3: (8, 8, 5, 6, 5, 5, 6, 5), 4: (8, 8), 5: (8, 8, 8, 8)}[self.fmt]
        res = [zigzag(signed(ep[k] - ep_ref[k], widths[k])) for k in range(self.n_ep)]
        if self.fmt == 4:
            self.lanes[2].append(res[0])
            self.lanes[3].append(res[1])
        elif self.fmt == 5:
            self.lanes[2].extend(res[0:2])
            self.lanes[3].extend(res[2:4])
        else:
            self.lanes[2].extend(res[0:2])
            for c in range(3):
                self.lanes[7 + c].extend((res[2 + c], res[5 + c]))
        alpha_channels = {3: 1, 4: 1, 5: 2}[self.fmt]
        for c in range(alpha_channels):
            for k in range(8):
                p = 16 * c + 2 * k
                sym = self.pair_symbol[((ix[p] - ix_ref[p]) & 7, (ix[p + 1] - ix_ref[p + 1]) & 7)]
                if self.fmt == 4:
                    self.lanes[4 + k].append(sym)
                else:
                    base = 3 if self.fmt == 3 else 4 + 4 * c
                    self.lanes[base + k // 2].append(sym)
        if self.fmt == 3:
            quad_pixels = []
            for ox, oy in ((0, 0), (2, 0), (0, 2), (2, 2)):
                quad_pixels.append([16 + 4 * (oy + dy) + ox + dx for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1))])
            for q, pix in enumerate(quad_pixels):
                key = tuple((ix[p] - ix_ref[p]) & 3 for p in pix)
                self.lanes[10 + (q >> 1)].append(self.quad_symbol[key])
        self.hist.append((list(ep), list(ix)))
        self.expected += self.pack(ep, ix)

    def pack(self, ep, ix):
        def alpha(e0, e1, pos):
            v = (e0 ^ 0x80) | (e1 ^ 0x80) << 8
            for p in range(16):
                v |= ALPHA_CODE[pos[p]] << (16 + 3 * p)
            return v.to_bytes(8, "little")

        if self.fmt == 4:
            return alpha(ep[0], ep[1], ix)
        if self.fmt == 5:
            return alpha(ep[0], ep[1], ix[:16]) + alpha(ep[2], ep[3], ix[16:])
        c0 = ep[2] << 11 | ep[3] << 5 | ep[4]
        c1 = ep[5] << 11 | ep[6] << 5 | ep[7]
        w = sum(COLOUR_CODE[ix[16 + p]] << (2 * p) for p in range(16))
        return (alpha(ep[0], ep[1], ix[:16]) + c0.to_bytes(2, "little") + c1.to_bytes(2, "little")
                + w.to_bytes(4, "little"))

    def random_block(self, er, xr):
        rng = self.rng
        i = len(self.hist)
        ep_ref, ix_ref = self.source(i, er)
        widths = {3: (8, 8, 5, 6, 5, 5, 6, 5), 4: (8, 8), 5: (8, 8, 8, 8)}[self.fmt]
        # Small residuals mostly (like real data), sometimes a full-range value.
        ep = [(ep_ref[k] + (rng.randrange(-3, 4) if rng.random() < 0.7 else rng.randrange(1 << widths[k])))
              & ((1 << widths[k]) - 1) for k in range(self.n_ep)]
        ix = []
        for p in range(self.n_ix):
            mask = 3 if self.fmt == 3 and p >= 16 else 7
            ix.append(rng.randrange(mask + 1))
        self.add(er, xr, ep, ix)

    def stream(self):
        return interleave_lanes(self.lanes, self.ab, self.rng), bytes(self.expected)


def build(fmt, seed, blocks=24):
    """Blocks cover: zero references, history references at several distances, dictionary
    references before the first block, and non-zero index references."""
    rng = random.Random(seed)
    enc = Encoder(fmt, rng)
    for i in range(blocks):
        choice = i % 6
        if i == 0:
            er, xr = 3, 0              # dictionary entry 2 (before the chunk)
        elif i == 1:
            er, xr = 0, 2              # zero endpoint state, index state from dictionary entry 0
        elif choice == 2:
            er, xr = 1, 1
        elif choice == 3:
            er, xr = min(i, 5), 0
        elif choice == 4:
            er, xr = 0, min(i, 3)
        else:
            er, xr = rng.randrange(1, i + 1), rng.randrange(0, i + 1)
        enc.random_block(er, xr)
    return enc.stream()


# Seed-7 streams of 4 blocks, as decoded by tests/bcpack_bcn_decode_test.cpp.
PINNED_SEED = 7
PINNED_BLOCKS = 4


class BcnChecks(unittest.TestCase):
    def test_roundtrip_all_formats(self):
        for fmt in (3, 4, 5):
            for seed in (1, 2, 3):
                with self.subTest(fmt=fmt, seed=seed):
                    stream, expected = build(fmt, seed)
                    self.assertEqual(bcpack_bcn.decode(stream, fmt, len(expected)), expected)

    def test_lane_shape_and_trailing_zeros(self):
        stream, expected = build(5, 4)
        syms, n = bcpack_bcn.lane_symbols(stream + bytes(16), 5, len(expected))
        self.assertEqual(n, len(expected) // 16)
        self.assertTrue(all(len(lane) == 2 * n for lane in syms))
        with self.assertRaises(BCPackError):
            bcpack_bcn.decode(stream + b"\x01", 5, len(expected))

    def test_negative_controls(self):
        for fmt in (3, 4, 5):
            stream, expected = build(fmt, 5)
            block = LAYOUTS[fmt][1]
            with self.subTest(fmt=fmt, case="truncated"), self.assertRaises(BCPackError):
                bcpack_bcn.decode(stream[:-1], fmt, len(expected))
            with self.subTest(fmt=fmt, case="one block too many"), self.assertRaises(BCPackError):
                bcpack_bcn.decode(stream, fmt, len(expected) + block)
            with self.subTest(fmt=fmt, case="not a block multiple"), self.assertRaises(ValueError):
                bcpack_bcn.decode(stream, fmt, len(expected) - 1)
            # A flipped payload byte must not reproduce the expected blocks.
            bad = bytearray(stream)
            bad[len(bad) // 2] ^= 0x40
            with self.subTest(fmt=fmt, case="flipped byte"):
                try:
                    self.assertNotEqual(bcpack_bcn.decode(bytes(bad), fmt, len(expected)), expected)
                except BCPackError:
                    pass

    def test_wrong_format_layout_rejected(self):
        # A BC5 stream read with the BC4 layout must not decode cleanly to the same bytes.
        stream, expected = build(5, 6)
        try:
            self.assertNotEqual(bcpack_bcn.decode(stream, 4, len(expected)), expected)
        except BCPackError:
            pass

    def test_cpp_vectors_match(self):
        source = (ROOT / "tests" / "bcpack_bcn_decode_test.cpp").read_text(encoding="utf-8")
        for fmt in (3, 4, 5):
            stream, expected = build(fmt, PINNED_SEED, PINNED_BLOCKS)
            with self.subTest(fmt=fmt):
                self.assertIn('"%s"' % stream.hex(), source)
                self.assertIn('"%s"' % expected.hex(), source)


def print_pinned():
    for fmt in (3, 4, 5):
        stream, expected = build(fmt, PINNED_SEED, PINNED_BLOCKS)
        print(fmt, stream.hex(), expected.hex())


if __name__ == "__main__":
    if sys.argv[1:] == ["--print-pinned"]:
        print_pinned()
    else:
        unittest.main()
