"""Build a tiny AMD64 PE32+ DLL in memory: one exported leaf function with a runtime-function record."""

import struct

SECTION, FILE = 0x1000, 0x200
LEAF = bytes.fromhex("8bc1 83c001 8902 c3")  # mov eax, ecx; add eax, 1; mov [rdx], eax; ret


def build_leaf_dll(path, directories=16, code=LEAF, export="leaf"):
    """Write a DLL exporting `export`; `directories` is the optional header's NumberOfRvaAndSizes."""
    text, rdata = code, bytearray()
    unwind = 0x2000
    rdata += bytes([1, 0, 0, 0])  # version 1, no prolog, no unwind codes
    exports = 0x2000 + len(rdata)
    names = 0x2000 + len(rdata) + 40 + 4 + 4 + 2
    rdata += struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, names + len(export) + 1, 1, 1, 1,
                         exports + 40, exports + 44, exports + 48)
    rdata += struct.pack("<IIH", SECTION, names, 0) + export.encode() + b"\0" + b"x.dll\0"
    pdata = struct.pack("<III", SECTION, SECTION + len(code), unwind)
    sections = ((b".text", SECTION, text, 0x60000020), (b".rdata", 0x2000, bytes(rdata), 0x40000040),
                (b".pdata", 0x3000, pdata, 0x40000040))
    entries = [(0, 0)] * 16
    entries[0], entries[3] = (exports, len(rdata) - 4), (0x3000, len(pdata))
    optional = struct.pack("<HBBIIIIIQIIHHHHHHIIIIHHQQQQII", 0x20B, 14, 0, len(text), 0, 0, SECTION, SECTION,
                           0x140000000, SECTION, FILE, 6, 0, 0, 0, 6, 0, 0, 0x4000, FILE, 0, 3, 0x8160,
                           0x100000, SECTION, 0x100000, SECTION, 0, directories)
    optional += b"".join(struct.pack("<II", *entry) for entry in entries[:directories])
    headers, offset, body = b"", FILE, b""
    for name, address, data, flags in sections:
        raw = -(-len(data) // FILE) * FILE
        headers += name.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", len(data), address, raw, offset + len(body),
                                                      0, 0, 0, 0, flags)
        body += data.ljust(raw, b"\0")
    coff = struct.pack("<HHIIIHH", 0x8664, len(sections), 0, 0, 0, len(optional), 0x2022)
    image = b"MZ" + bytes(58) + struct.pack("<I", 0x40) + b"PE\0\0" + coff + optional + headers
    with open(path, "wb") as handle:
        handle.write(image.ljust(FILE, b"\0") + body)
