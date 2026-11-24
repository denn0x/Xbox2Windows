"""Inventory program candidates from an explicitly located SGD5 effect record."""
import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
from typing import BinaryIO, Sequence
import zlib

from paged_resource import PagedResource, Segment
from resource_inventory import parts


GROUPS = ("VS", "PS", "GS", "DS", "HS", "CS")


class Cursor:
    def __init__(self, view: PagedResource, start: int, end: int):
        self.view = view
        self.pos = start
        self.end = end

    def skip(self, size: int) -> int:
        start = self.pos
        if size > self.end - start:
            raise ValueError("SGD5 field exceeds supplied range")
        self.pos += size
        return start

    def read(self, size: int) -> bytes:
        return self.view.read(self.skip(size), size)

    def word(self) -> int:
        return int.from_bytes(self.read(4), "little")

    def name(self) -> str:
        length = self.read(1)[0]
        raw = self.read(length)
        if len(raw) < 2 or raw[-1] or not all(32 <= c < 127 for c in raw[:-1]):
            raise ValueError("Invalid length-prefixed SGD5 name")
        return raw[:-1].decode("ascii")


@dataclass(frozen=True)
class Program:
    name: str
    offset: int
    stored: int
    decoded: int


def read_program(cursor: Cursor) -> Program:
    name = cursor.name()
    cursor.skip(1)
    size = cursor.word()
    magic, repeated, decoded = struct.unpack("<3I", cursor.read(12))
    if magic != 0xddce7 or size != repeated or not 12 < size <= 8 << 20 or not 32 <= decoded <= 16 << 20:
        raise ValueError("Invalid bounded SGD5 compressed container")
    offset = cursor.skip(size - 12)
    cursor.skip(152)  # Hash and root signature.
    reflection = cursor.word()
    registers, buffers, _, repeated = struct.unpack("<4H", cursor.read(8))
    if reflection != repeated or reflection != 9 + 24 * (registers + buffers):
        raise ValueError("Invalid SGD5 reflection block length")
    cursor.skip(reflection - 8)
    return Program(name, offset, size - 12, decoded)


def validate_name_block(tail: bytes) -> None:
    # The size word of the final NUL-delimited name block must consume the
    # supplied record exactly. Other state/technique bytes are opaque.
    if len(tail) < 5 or tail[-1]:
        raise ValueError("Missing trailing SGD5 name block")
    suffix = len(tail)
    while suffix and (tail[suffix - 1] == 0 or 32 <= tail[suffix - 1] < 127):
        suffix -= 1
    starts = sum(tail[i + 4] == 0 and struct.unpack_from("<I", tail, i)[0] == len(tail) - i - 4
                 for i in range(max(0, suffix - 4), len(tail) - 4))
    if starts != 1:
        raise ValueError("Expected one sized name block ending at the supplied record boundary")


@dataclass(frozen=True)
class Candidate:
    group: str
    index: int
    name: str
    dxil_kind: int
    container_sha256: str


def inspect_container(view: PagedResource, program: Program, group: str, index: int) -> Candidate:
    decoder = zlib.decompressobj(-15)
    try:
        blob = decoder.decompress(view.read(program.offset, program.stored), program.decoded + 1)
    except zlib.error as exc:
        raise ValueError("Invalid compressed SGD5 candidate") from exc
    if not decoder.eof or decoder.unused_data or len(blob) != program.decoded:
        raise ValueError("Candidate container disagrees with its declared extents")
    dxil = parts(blob).get(b"DXIL", b"")
    if len(dxil) < 24:
        raise ValueError("Truncated candidate DXIL program header")
    version_type, words, magic, _, payload_offset, payload_size = struct.unpack_from("<II4sIII", dxil)
    # Console payloads may have a different signature from desktop LLVM bitcode.
    if (magic != b"DXIL" or payload_offset < 16 or payload_size < 4
            or not 8 + payload_offset + payload_size <= 4 * words <= len(dxil)):
        raise ValueError("Candidate DXIL program exceeds its declared extents or has an invalid header")
    return Candidate(group, index, program.name, version_type >> 16, hashlib.sha256(blob).hexdigest())


def candidates(stream: BinaryIO, name: str, group_start: int, group_end: int, arrays: int,
               record_start: int, record_end: int) -> list[Candidate]:
    if group_start < 0 or not group_start + 32 <= arrays < record_start < record_end <= group_end:
        raise ValueError("Program table and effect record must lie in the supplied SGD5 group")
    if record_end - record_start > 1 << 16:
        raise ValueError("Effect record exceeds 64 KiB budget")
    size = group_end - group_start
    view = PagedResource(stream, [Segment(group_start, size, size, "raw")])
    if view.read(0, 4) != b"SGD5":
        raise ValueError("Missing SGD5 group header")
    cursor = Cursor(view, arrays - group_start, record_start - group_start)
    groups: list[list[Program]] = []
    remaining = 65536
    for _ in GROUPS:
        count = cursor.word()
        if count > remaining:
            raise ValueError("SGD5 program tables exceed 65536-record budget")
        remaining -= count
        groups.append([read_program(cursor) for _ in range(count)])
    effect_count = cursor.word()
    before = record_start - group_start - cursor.pos
    after = group_end - record_end
    # Other effects each need a name, one word, six counts and a sized name block.
    if not 1 <= effect_count <= 1 + before // 36 + after // 36:
        raise ValueError("SGD5 effect count does not fit outside the selected record")
    if before and (before < 36 or effect_count == 1):
        raise ValueError("Selected effect disagrees with the SGD5 effect count")

    effect = Cursor(view, record_start - group_start, record_end - group_start)
    if effect.name() != name:
        raise ValueError("Selected effect name differs from the requested name")
    effect.skip(4)
    requested: list[tuple[str, int, Program]] = []
    for label, programs in zip(GROUPS, groups):
        count = effect.word()
        if count > 4096 - len(requested):
            raise ValueError("Effect exceeds 4096 candidate references")
        for index, in struct.iter_unpack("<I", effect.read(count * 4)):
            if index >= len(programs):
                raise ValueError(f"Candidate index outside {label} program table")
            requested.append((label, index, programs[index]))
    validate_name_block(effect.read(effect.end - effect.pos))
    if sum(p.stored + p.decoded for _, _, p in requested) > 64 << 20:
        raise ValueError("Candidate reads exceed 64 MiB combined stored and decoded budget")
    return [inspect_container(view, p, label, index) for label, index, p in requested]


def offset(value: str) -> int:
    return int(value, 0)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("effect", help="Expected exact name of the supplied effect record")
    parser.add_argument("--group", nargs=2, required=True, type=offset, metavar=("START", "END"),
                        help="Physical SGD5 group bounds, end exclusive")
    parser.add_argument("--arrays", required=True, type=offset, help="Physical start of the six program arrays")
    parser.add_argument("--record", nargs=2, required=True, type=offset, metavar=("START", "END"),
                        help="Physical bounds of the selected effect, including its final name block")
    args = parser.parse_args(argv)
    try:
        with args.archive.open("rb") as stream:
            found = candidates(stream, args.effect, *args.group, args.arrays, *args.record)
        print(json.dumps({"effect": args.effect, "candidates": [asdict(item) for item in found]}, indent=2))
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"{args.archive}: {exc}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
