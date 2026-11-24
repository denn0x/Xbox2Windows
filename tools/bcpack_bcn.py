"""Reference BCPack block reconstruction for BC3, BC4 and BC5, kept in step with bcpack_decoder.h.

The lane layouts below were checked against the entropy layer: with them, every BC3, BC4 and BC5
interval of the local test archives decodes to exactly its logical size, consumes exactly its
compressed bytes and ends every lane in a legal rANS state. The same layouts are published by
bcpack-rs (https://github.com/Ahmet-Dedeler/bcpack-rs, revision c55c5fad, MIT OR Apache-2.0).

Block reconstruction follows the bcpack-rs reference rules as adapted into
src/startup_stubs/bcpack_decoder.h: a block's endpoint reference is x + unzigzag(d) and its index
reference is x (lanes 0 and 1), references beyond the decoded history read a 255-entry dictionary,
residuals are unzigzagged modulo the field width, pair and quadrant symbols are ranks into the
tables of src/startup_stubs/bcpack_tables_bcn.inl (read here, not duplicated), and an alpha
endpoint is stored as its state XOR 0x80.

The rank tables and dictionaries are fitted by bcpack-rs and are not independently verified; see
docs in the C++ header. The BC6H entry is a lane layout only (no block reconstruction).
"""
import ast
import re
from pathlib import Path

from _bcpack_rans import BCPackError, decode_lanes

ROOT = Path(__file__).resolve().parents[1]
TABLES = ROOT / "src" / "startup_stubs" / "bcpack_tables_bcn.inl"

# format: (alphabet bits per lane = symbol width - 1, block bytes, symbols per lane per block)
LAYOUTS = {
    3: ((3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7), 16, 2),
    4: ((7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5), 8, 1),
    5: ((3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5), 16, 2),
    6: ((3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4), 16, 2),
}

ALPHA_CODE = (0, 2, 3, 4, 5, 6, 7, 1)    # ramp position -> BC4 index code
COLOUR_CODE = (0, 2, 3, 1)               # ramp position -> BC1 index code
ALPHA_POSITION = tuple(ALPHA_CODE.index(c) for c in range(8))
COLOUR_POSITION = tuple(COLOUR_CODE.index(c) for c in range(4))

_TABLE_CACHE = {}


def _array(text, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*(?:\[[^\]]*\])*\s*=\s*\{", text)
    if not m:
        raise ValueError("table %s not found in %s" % (name, TABLES.name))
    depth = 0
    for i in range(m.end() - 1, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                body = text[m.end() - 1:i + 1]
                break
    else:
        raise ValueError("table %s: unbalanced braces" % name)
    return ast.literal_eval(body.replace("{", "[").replace("}", "]"))


def tables():
    """Rank tables and dictionaries parsed from bcpack_tables_bcn.inl (cached)."""
    if not _TABLE_CACHE:
        text = re.sub(r"//[^\n]*", "", TABLES.read_text(encoding="utf-8"))
        for name in ("RANK_PAIR_BC4", "RANK_PAIR_BC5", "RANK_PAIR_BC3_ALPHA", "RANK_QUAD_BC3_COLOUR",
                     "BC4_DICTIONARY", "BC5_DICTIONARY", "BC3_DICTIONARY"):
            _TABLE_CACHE[name] = _array(text, name)
    return _TABLE_CACHE


def zigzag(v):
    return 2 * v if v >= 0 else -2 * v - 1


def unzigzag(v):
    return -((v + 1) >> 1) if v & 1 else v >> 1


def residual(sym, bits):
    return unzigzag(sym) & ((1 << bits) - 1)


def interleave(even, odd):
    out = 0
    for k in range(4):
        out |= ((even >> k) & 1) << (2 * k) | ((odd >> k) & 1) << (2 * k + 1)
    return out


def deinterleave(byte):
    even = odd = 0
    for k in range(4):
        even |= ((byte >> (2 * k)) & 1) << k
        odd |= ((byte >> (2 * k + 1)) & 1) << k
    return even, odd


def pack_alpha(e0, e1, positions):
    v = (e0 ^ 0x80) | (e1 ^ 0x80) << 8
    for p in range(16):
        v |= ALPHA_CODE[positions[p] & 7] << (16 + 3 * p)
    return v.to_bytes(8, "little")


def pack_colour(ep, positions):
    c0 = ep[0] << 11 | ep[1] << 5 | ep[2]
    c1 = ep[3] << 11 | ep[4] << 5 | ep[5]
    w = 0
    for p in range(16):
        w |= COLOUR_CODE[positions[p] & 3] << (2 * p)
    return c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + w.to_bytes(4, "little")


def _source(hist, dictionary, i, r, n_ep, n_ix):
    if r == 0:
        return [0] * n_ep, [0] * n_ix
    if r <= i:
        return hist[i - r]
    k = r - i - 1
    if k < len(dictionary):
        ep, ix = dictionary[k]
        return list(ep), list(ix)
    return [0] * n_ep, [0] * n_ix


def _references(syms, i, per):
    if per == 1:
        d, x = syms[0][i] & 0xFF, syms[1][i] & 0xFF
    else:
        d = interleave(syms[0][2 * i], syms[0][2 * i + 1])
        x = interleave(syms[1][2 * i], syms[1][2 * i + 1])
    return (x + unzigzag(d)) & 0xFF, x


def blocks_bc4(syms, n):
    t = tables()
    pairs, dic = t["RANK_PAIR_BC4"], t["BC4_DICTIONARY"]
    hist, out = [], bytearray()
    for i in range(n):
        er, xr = _references(syms, i, 1)
        ep_ref, _ = _source(hist, dic, i, er, 2, 16)
        _, ix_ref = _source(hist, dic, i, xr, 2, 16)
        ep = [(ep_ref[e] + residual(syms[2 + e][i], 8)) & 0xFF for e in range(2)]
        ix = [0] * 16
        for k in range(8):
            a, b = pairs[syms[4 + k][i] & 63]
            ix[2 * k] = (ix_ref[2 * k] + a) & 7
            ix[2 * k + 1] = (ix_ref[2 * k + 1] + b) & 7
        hist.append((ep, ix))
        out += pack_alpha(ep[0], ep[1], ix)
    return bytes(out)


def blocks_bc5(syms, n):
    t = tables()
    pairs, dic = t["RANK_PAIR_BC5"], t["BC5_DICTIONARY"]
    hist, out = [], bytearray()
    for i in range(n):
        er, xr = _references(syms, i, 2)
        ep_ref, _ = _source(hist, dic, i, er, 4, 32)
        _, ix_ref = _source(hist, dic, i, xr, 4, 32)
        ep, ix = [0] * 4, [0] * 32
        for c in range(2):
            for e in range(2):
                ep[2 * c + e] = (ep_ref[2 * c + e] + residual(syms[2 + c][2 * i + e], 8)) & 0xFF
            for l in range(4):
                for h in range(2):
                    a, b = pairs[syms[4 + 4 * c + l][2 * i + h] & 63]
                    p = 16 * c + 4 * l + 2 * h
                    ix[p] = (ix_ref[p] + a) & 7
                    ix[p + 1] = (ix_ref[p + 1] + b) & 7
        hist.append((ep, ix))
        out += pack_alpha(ep[0], ep[1], ix[:16]) + pack_alpha(ep[2], ep[3], ix[16:])
    return bytes(out)


QUAD_ORIGIN = ((0, 0), (2, 0), (0, 2), (2, 2))   # TL, TR, BL, BR
QUAD_STEP = ((0, 0), (1, 0), (0, 1), (1, 1))


def blocks_bc3(syms, n):
    t = tables()
    pairs, quads, dic = t["RANK_PAIR_BC3_ALPHA"], t["RANK_QUAD_BC3_COLOUR"], t["BC3_DICTIONARY"]
    widths = (5, 6, 5)
    hist, out = [], bytearray()
    for i in range(n):
        er, xr = _references(syms, i, 2)
        ep_ref, _ = _source(hist, dic, i, er, 8, 32)
        _, ix_ref = _source(hist, dic, i, xr, 8, 32)
        ep, ix = [0] * 8, [0] * 32
        for e in range(2):
            ep[e] = (ep_ref[e] + residual(syms[2][2 * i + e], 8)) & 0xFF
        for l in range(4):
            for h in range(2):
                a, b = pairs[syms[3 + l][2 * i + h] & 63]
                p = 4 * l + 2 * h
                ix[p] = (ix_ref[p] + a) & 7
                ix[p + 1] = (ix_ref[p + 1] + b) & 7
        for c in range(3):
            mask = (1 << widths[c]) - 1
            ep[2 + c] = (ep_ref[2 + c] + residual(syms[7 + c][2 * i], widths[c])) & mask
            ep[5 + c] = (ep_ref[5 + c] + residual(syms[7 + c][2 * i + 1], widths[c])) & mask
        for q in range(4):
            sym = syms[10 + (q >> 1)][2 * i + (q & 1)] & 0xFF
            ox, oy = QUAD_ORIGIN[q]
            for k in range(4):
                dx, dy = QUAD_STEP[k]
                p = 16 + 4 * (oy + dy) + ox + dx
                ix[p] = (ix_ref[p] + quads[sym][k]) & 3
        hist.append((ep, ix))
        out += pack_alpha(ep[0], ep[1], ix[:16]) + pack_colour(ep[2:8], ix[16:])
    return bytes(out)


RECONSTRUCT = {3: blocks_bc3, 4: blocks_bc4, 5: blocks_bc5}


def lane_symbols(data, fmt, output_size):
    """Entropy-decode one interval into per-lane symbols for `output_size` bytes of blocks."""
    ab, block, per = LAYOUTS[fmt]
    if output_size <= 0 or output_size % block:
        raise ValueError("output size is not a whole number of %d-byte blocks" % block)
    n = output_size // block
    result = decode_lanes(data, list(ab), [n * per] * len(ab))
    if any(data[result["consumed"]:]):
        raise BCPackError("non-zero trailing data after byte %d" % result["consumed"])
    return result["syms"], n


def decode(data, fmt, output_size):
    """Decode a BCPack BC3/BC4/BC5 stream into `output_size` bytes of BCn blocks."""
    if fmt not in RECONSTRUCT:
        raise ValueError("BCPack format %d has no block reconstruction here" % fmt)
    syms, n = lane_symbols(data, fmt, output_size)
    return RECONSTRUCT[fmt](syms, n)
