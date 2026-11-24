"""Check the DirectStorage stand-in's zlib decoder (src/startup_stubs/inflate.h) against CPython's zlib.

The stand-in inflates zlib requests with its own RFC 1950/1951 decoder. The stub consumer only
feeds it stored blocks, a fixed block that repeats one byte at distance 1, and single-byte runs.
This check decodes streams that CPython's zlib encodes from seeded synthetic data with stored,
fixed and dynamic Huffman blocks (including a sparse alphabet whose block header needs all three
code-length repeat codes), back-references across the 32 KiB window and a sync flush between
blocks. Each decoded result must equal the input. The controls are streams the decoder must reject
without writing past the destination: one byte too little capacity and a truncated stream.

The distance bound is checked on bare deflate data, so neither the zlib header nor the Adler-32
check can reject it: after four literals, a match at distance 4 decodes and the same block with
distance 5 is rejected before the match copies anything. tests/startup_inflate_probe.cpp runs the
decoder; no GPU and no game files are involved.
"""
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import zlib


def corpus(rng, size):
    # Random literals and copies of 3..258 bytes from 1..32000 bytes back, both log-uniform, so an
    # encoder emits most length and distance codes with their extra bits.
    data = bytearray(rng.randbytes(4096))
    while len(data) < size:
        if rng.random() < 0.3:
            data += rng.randbytes(rng.randint(1, 40))
            continue
        length = round(3 * 86 ** rng.random())
        start = len(data) - min(len(data), round(32000 ** rng.random()))
        for i in range(length):
            data.append(data[start + i])
    return bytes(data[:size])


def compress(data, level=9, strategy=zlib.Z_DEFAULT_STRATEGY):
    encoder = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    return encoder.compress(data) + encoder.flush()


def first_block_type(stream, mode="zlib"):
    return (stream[2 if mode == "zlib" else 0] >> 1) & 3


class Bits:
    """Deflate bit order: fields least significant bit first, Huffman codes most significant bit first."""

    def __init__(self):
        self.value = 0
        self.count = 0

    def put(self, value, count):
        self.value |= value << self.count
        self.count += count

    def code(self, code, count):
        self.put(int(format(code, "0%db" % count)[::-1], 2), count)

    def to_bytes(self):
        return self.value.to_bytes((self.count + 7) // 8, "little")


def words(rng, size):
    # Lowercase words and spaces: most literal codes are unused, so the dynamic block header needs
    # the zero-run codes 17 and 18 as well as code 16.
    out = bytearray()
    while len(out) < size:
        out += bytes(rng.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rng.randint(1, 9))) + b" "
    return bytes(out[:size])


def fixed_match_block(literals, distance_code, extra_bits, extra_value):
    # Bare deflate data, one final fixed block: the literals, a length-3 match, end of block.
    bits = Bits()
    bits.put(1, 1)
    bits.put(1, 2)
    for literal in literals:
        bits.code(0x30 + literal, 8)
    bits.code(257 - 256, 7)
    bits.code(distance_code, 5)
    bits.put(extra_value, extra_bits)
    bits.code(256 - 256, 7)
    return bits.to_bytes()


def reference(stream, mode):
    return zlib.decompress(stream, 15 if mode == "zlib" else -15)


def reference_rejects(stream, mode="zlib"):
    try:
        reference(stream, mode)
    except zlib.error:
        return True
    return False


def run(probe, work, name, stream, capacity, mode):
    source, output = work / (name + "." + mode), work / (name + ".out")
    source.write_bytes(stream)
    result = subprocess.run([str(probe), mode, str(source), str(capacity), str(output)],
                            capture_output=True, text=True, timeout=60)
    return result, output


def expect_decoded(probe, work, name, stream, data, capacity=None, mode="zlib"):
    assert reference(stream, mode) == data, name + ": the reference decoder disagrees with the input"
    capacity = len(data) if capacity is None else capacity
    result, output = run(probe, work, name, stream, capacity, mode)
    assert result.returncode == 0, "%s: exit %d: %s%s" % (name, result.returncode, result.stdout, result.stderr)
    assert result.stdout.strip() == "inflated %d" % len(data), "%s: %s" % (name, result.stdout.strip())
    decoded = output.read_bytes()
    if decoded != data:
        first = next((i for i, (a, b) in enumerate(zip(decoded, data)) if a != b), min(len(decoded), len(data)))
        raise AssertionError("%s: first mismatch at byte %d of %d" % (name, first, len(data)))
    print("%-22s block %d, %6d -> %6d bytes, decoded" % (name, first_block_type(stream, mode), len(stream), len(data)))


def expect_rejected(probe, work, name, stream, capacity, mode="zlib", decoded_before=None, invalid=True):
    # invalid: the stream itself is damaged, so the reference decoder must reject it too.
    assert not invalid or reference_rejects(stream, mode), name + ": the reference decoder accepted the stream"
    result, _ = run(probe, work, name, stream, capacity, mode)
    want = "rejected" if decoded_before is None else "rejected after %d bytes" % decoded_before
    assert result.returncode == 1 and result.stdout.strip() == want, \
        "%s: exit %d: %s%s" % (name, result.returncode, result.stdout, result.stderr)
    print("%-22s %6d bytes, capacity %6d, %s" % (name, len(stream), capacity, want))


def main(probe):
    probe = Path(probe).resolve()
    rng = random.Random(1149)
    text = corpus(rng, 96 * 1024)
    noise = rng.randbytes(70000)
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)

        stored = zlib.compress(noise, 0)
        assert first_block_type(stored) == 0
        expect_decoded(probe, work, "stored", stored, noise)

        fixed = compress(text, strategy=zlib.Z_FIXED)
        assert first_block_type(fixed) == 1
        expect_decoded(probe, work, "fixed", fixed, text)

        dynamic = compress(text)
        assert first_block_type(dynamic) == 2
        expect_decoded(probe, work, "dynamic", dynamic, text)
        expect_decoded(probe, work, "dynamic_spare_capacity", dynamic, text, len(text) + 4096)
        sparse = words(rng, 32 * 1024)
        sparse_stream = compress(sparse)
        assert first_block_type(sparse_stream) == 2
        expect_decoded(probe, work, "dynamic_sparse", sparse_stream, sparse)

        # A sync flush ends the first part with an empty stored block; the second part copies from the
        # first across it, so the decoder must keep its window and bit position over the boundary.
        head, tail = noise[:20000], noise[5000:9000] + noise[100:3000] + text[:8192]
        encoder = zlib.compressobj(6)
        flushed = encoder.compress(head) + encoder.flush(zlib.Z_SYNC_FLUSH)
        assert flushed.endswith(b"\x00\x00\xff\xff")
        flushed += encoder.compress(tail) + encoder.flush()
        expect_decoded(probe, work, "sync_flush", flushed, head + tail)

        expect_decoded(probe, work, "empty", zlib.compress(b""), b"")

        # Controls: streams that do not fit or cannot be decoded are rejected.
        expect_rejected(probe, work, "dynamic_short_capacity", dynamic, len(text) - 1, invalid=False)
        expect_rejected(probe, work, "dynamic_truncated", dynamic[:len(dynamic) // 2], len(text))

        # The distance bound, on bare deflate data with room to spare: after "ABCD", distance 4 (code 3)
        # starts the match at the first output byte; distance 5 (code 4, extra bit 0) starts it one byte
        # before the output and must be rejected with only the four literals decoded.
        expect_decoded(probe, work, "raw_distance_4", fixed_match_block(b"ABCD", 3, 0, 0), b"ABCDABC", 64, "raw")
        expect_rejected(probe, work, "raw_distance_5", fixed_match_block(b"ABCD", 4, 1, 0), 64, "raw", 4)
    print("inflate.h matches zlib %s in 8 cases and rejects 3 controls" % zlib.ZLIB_RUNTIME_VERSION)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: startup_inflate_check.py <startup_inflate_probe.exe>")
    main(sys.argv[1])
