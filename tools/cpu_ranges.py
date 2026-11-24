"""File-backed x64 function intervals and chained unwind ownership."""

import struct


def image_bytes(pe, rva, size, executable=False):
    section = pe.get_section_by_rva(rva)
    if (not section or size <= 0 or (executable and not section.Characteristics & 0x20000000)
            or rva + size - section.VirtualAddress > min(section.Misc_VirtualSize, section.SizeOfRawData)):
        raise ValueError(f"invalid file-backed range at RVA {rva:#x}")
    data = pe.get_data(rva, size)
    if len(data) != size:
        raise ValueError(f"truncated range at RVA {rva:#x}")
    return data


def function_ranges(pe, entry):
    directories = pe.OPTIONAL_HEADER.DATA_DIRECTORY
    if len(directories) < 4:
        raise ValueError("image has no runtime-function directory")
    table = directories[3]
    if table.Size % 12:
        raise ValueError("incomplete runtime-function record")
    records = list(struct.iter_unpack("<III", image_bytes(pe, table.VirtualAddress, table.Size)))
    parents = {}
    previous = 0
    for record in records:
        begin, end, info = record
        if begin < previous or end <= begin or info % 4:
            raise ValueError(f"invalid runtime-function interval at {begin:#x}")
        previous = end
        header = image_bytes(pe, info, 4)
        version = header[0] & 7
        flags = header[0] >> 3
        parent = None
        if flags & 4:
            if flags != 4 or version not in (1, 2):
                raise ValueError(f"unsupported chained unwind header at {info:#x}")
            trailer = info + 4 + ((header[2] + 1) & ~1) * 2
            parent = struct.unpack("<III", image_bytes(pe, trailer, 12))
        parents[record] = parent
    owners = {}
    for record in records:
        current = record
        path = set()
        while current not in owners:
            if current not in parents or current in path:
                raise ValueError(f"invalid unwind chain from {record[0]:#x}")
            path.add(current)
            if parents[current] is None:
                break
            current = parents[current]
        root = owners.get(current, current)
        owners.update((part, root) for part in path)
    ranges = [(begin, end) for (begin, end, _), root in owners.items() if root[0] == entry]
    if not ranges:
        raise ValueError(f"no unique function interval at {entry:#x}")
    return sorted(ranges)
