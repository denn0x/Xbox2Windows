"""Count bounded zlib streams containing a complete table of tagged sections."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import struct
import zlib


def sections(data):
    """Return four-character tags when big-endian section lengths exactly cover the buffer."""
    tags = []
    offset = 0
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from(">4sI", data, offset)
        if not re.fullmatch(b"[A-Z0-9_]{4}", tag) or size < 8 or offset + size > len(data):
            return ()
        tags.append(tag.decode("ascii"))
        offset += size
    return tuple(tags) if offset == len(data) else ()


def scan_window(data, base, first, last):
    valid = 0
    layouts = Counter()
    # RFC 1950 CMF bytes for DEFLATE with a window exponent from 8 through 15.
    first_aligned = -base % 16
    window = memoryview(data)
    aligned_methods = bytes(data[first_aligned:-1:16])
    for match in re.finditer(rb"[\x08\x18\x28\x38\x48\x58\x68\x78]", aligned_methods):
        pos = first_aligned + match.start() * 16
        flags = data[pos + 1]
        if flags & 0x20 or (data[pos] * 256 + flags) % 31:
            continue
        decoder = zlib.decompressobj()
        try:
            decoded = decoder.decompress(window[pos:pos + (256 << 10)], (256 << 10) + 1)
        except zlib.error:
            continue
        if not decoder.eof or len(decoded) > 256 << 10:
            continue
        valid += 1
        if decoded.startswith(first.encode("ascii")):
            tags = sections(decoded)
            if tags and tags[0] == first and tags[-1] == last:
                layouts[" ".join(tags)] += 1
    return valid, layouts


def inspect(path, first, last):
    size = path.stat().st_size
    window = 8 << 20
    # Merge overlapping windows on small files so a stream is counted only once.
    spans = []
    for start in (0, max(0, (size-window)//2), max(0, size-window)):
        end = min(size, start + window)
        if spans and start <= spans[-1][1]:
            spans[-1][1] = end
        else:
            spans.append([start, end])
    valid = 0
    layouts = Counter()
    with path.open("rb") as stream:
        for start, end in spans:
            stream.seek(start)
            count, found = scan_window(stream.read(end-start), start, first, last)
            valid += count
            layouts.update(found)
    return {"file":path.name, "bytes_sampled":sum(b-a for a,b in spans),
            "valid_zlib":valid, "section_records":sum(layouts.values()), "layouts":layouts}


def section_tag(value):
    if not re.fullmatch("[A-Z0-9_]{4}", value):
        raise argparse.ArgumentTypeError(f"{value!a} is not a section tag: use four characters from A-Z, 0-9 and _")
    return value


def main(argv=None):
    """Run the section probe CLI and return the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--first", required=True, type=section_tag)
    parser.add_argument("--last", required=True, type=section_tag)
    args = parser.parse_args(argv)
    print(json.dumps([inspect(p, args.first, args.last) for p in args.archives], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
