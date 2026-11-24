"""Differential checks, fault-suppression checks, mutation controls and rejection controls for AVX move/shuffle/blend/logical AOT."""

import ctypes
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory

import pefile
from iced_x86 import Code, Encoder, Instruction, MemoryOperand, Register as R

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from cpu_avx_move_aot import lift_code, translate
from cpu_leaf_aot import export_rva

MASK32 = 0xffffffff
MASK64 = 0xffffffffffffffff
GUARD_PRE = bytes(0xA0 + (i % 16) for i in range(32))
GUARD_POST = bytes(0xC0 + (i % 16) for i in range(32))
FILL = b"\x5A" * 32
OUT_FILL_64 = b"\x5A" * 64
STORE_INIT = bytes(0xD0 + i for i in range(32))

# Standard 2-input (rcx=32B, rdx=32B, r8=32B) exports
STD_EXPORTS = (
    # Logical (16)
    "op_andpd_xmm", "op_andpd_ymm",
    "op_andps_xmm", "op_andps_ymm",
    "op_andnpd_xmm", "op_andnpd_ymm",
    "op_andnps_xmm", "op_andnps_ymm",
    "op_orpd_xmm", "op_orpd_ymm",
    "op_orps_xmm", "op_orps_ymm",
    "op_xorpd_xmm", "op_xorpd_ymm",
    "op_xorps_xmm", "op_xorps_ymm",
    # Immediate blends (8)
    "op_blendpd_xmm", "op_blendpd_ymm", "op_blendpd_ymm_all", "op_blendpd_ymm_none",
    "op_blendps_xmm", "op_blendps_ymm", "op_blendps_ymm_all", "op_blendps_ymm_none",
    # Broadcasts (16)
    "op_broadcastf128",
    "op_broadcastsd_mem0", "op_broadcastsd_mem1", "op_broadcastsd_mem2", "op_broadcastsd_mem3",
    "op_broadcastsd_reg",
    "op_broadcastss_xmm_mem0", "op_broadcastss_xmm_mem1",
    "op_broadcastss_xmm_mem2", "op_broadcastss_xmm_mem3",
    "op_broadcastss_ymm_mem4", "op_broadcastss_ymm_mem5",
    "op_broadcastss_ymm_mem6", "op_broadcastss_ymm_mem7",
    "op_broadcastss_xmm_reg", "op_broadcastss_ymm_reg",
    # Lane ops (13)
    "op_extractf128_lo", "op_extractf128_hi",
    "op_insertf128_lo", "op_insertf128_hi",
    "op_extractps_lane0", "op_extractps_lane1", "op_extractps_lane2", "op_extractps_lane3",
    "op_extractps_w1_lane2",
    "op_insertps_s0d1_z0", "op_insertps_s3d2_zmask", "op_insertps_zero_all",
    "op_insertps_s2d3_z1",
    # Permutes (18)
    "op_perm2f128_mix", "op_perm2f128_swap",
    "op_perm2f128_zero_lo", "op_perm2f128_zero_hi", "op_perm2f128_zero_both", "op_perm2f128_ignored",
    "op_permilpd_imm_xmm", "op_permilpd_imm_ymm", "op_permilpd_imm_ymm_lo", "op_permilpd_imm_ymm_hi",
    "op_permilpd_var_xmm", "op_permilpd_var_ymm",
    "op_permilps_imm_xmm", "op_permilps_imm_ymm", "op_permilps_imm_ymm_lo", "op_permilps_imm_ymm_hi",
    "op_permilps_var_xmm", "op_permilps_var_ymm",
    # Shuffles (10)
    "op_shufpd_xmm", "op_shufpd_ymm", "op_shufpd_ymm_lo", "op_shufpd_ymm_hi",
    "op_shufps_xmm", "op_shufps_ymm", "op_shufps_ymm_lo", "op_shufps_ymm_hi",
    "op_shufpd_xmm_sel1", "op_shufps_ymm_sel2",
    # Unpacks (8)
    "op_unpcklpd_xmm", "op_unpcklpd_ymm",
    "op_unpckhpd_xmm", "op_unpckhpd_ymm",
    "op_unpcklps_xmm", "op_unpcklps_ymm",
    "op_unpckhps_xmm", "op_unpckhps_ymm",
    # Full-width moves (aligned + unaligned + vlddqu + NT) (17)
    "op_movapd_xmm", "op_movapd_ymm", "op_movapd_store128", "op_movapd_xmm_dirty",
    "op_movupd_xmm", "op_movupd_ymm", "op_movupd_store128",
    "op_lddqu_xmm", "op_lddqu_ymm",
    "op_movntdq_xmm", "op_movntdq_ymm",
    "op_movntdqa_xmm", "op_movntdqa_ymm",
    "op_movntpd_xmm", "op_movntpd_ymm",
    "op_movntps_xmm", "op_movntps_ymm",
    # Dup moves (6)
    "op_movddup_xmm", "op_movddup_ymm",
    "op_movshdup_xmm", "op_movshdup_ymm",
    "op_movsldup_xmm", "op_movsldup_ymm",
    # Half-XMM register and memory moves (10)
    "op_movhlps", "op_movlhps",
    "op_movhpd_load", "op_movhpd_store",
    "op_movhps_load", "op_movhps_store",
    "op_movlpd_load", "op_movlpd_store",
    "op_movlps_load", "op_movlps_store",
    # Scalar moves (6)
    "op_movsd_load", "op_movsd_store", "op_movsd_reg",
    "op_movss_load", "op_movss_store", "op_movss_reg",
    # Sign-bit mask to GPR (4)
    "op_movmskpd_xmm", "op_movmskpd_ymm",
    "op_movmskps_xmm", "op_movmskps_ymm",
    # VZEROUPPER, chained pipeline and GPR bridge (5)
    "op_zeroupper", "op_pipeline", "op_mov_load32",
    "op_zeroupper_ymm1", "op_mov_store32",
)

# Variable blend exports take rcx=src1(32B), rdx=src2(32B)+mask(32B), r8=out(32B)
BLENDV_EXPORTS = (
    "op_blendvpd_xmm",
    "op_blendvpd_ymm",
    "op_blendvps_xmm",
    "op_blendvps_ymm",
)

# Exports that explicitly allow unaligned addresses in native x86 (tested at +1, +3, +7 byte offsets)
UNALIGNED_SAFE_EXPORTS = (
    "op_movupd_xmm", "op_movupd_ymm", "op_movupd_store128",
    "op_lddqu_xmm", "op_lddqu_ymm",
    "op_broadcastf128", "op_broadcastsd_mem1", "op_broadcastss_ymm_mem4",
    "op_movhpd_load", "op_movhpd_store", "op_movlps_load", "op_movlps_store",
    "op_movsd_load", "op_movsd_store", "op_movss_load", "op_movss_store",
)

# (export_name, elem_bits, wide, alt_disp)
MASKMOV_LOAD_SPECS = (
    ("op_maskmovps_load_xmm", 32, False, None),
    ("op_maskmovps_load_ymm", 32, True, 16),
    ("op_maskmovpd_load_xmm", 64, False, None),
    ("op_maskmovpd_load_ymm", 64, True, None),
)

# (export_name, elem_bits, wide)
MASKMOV_STORE_SPECS = (
    ("op_maskmovps_store_xmm", 32, False),
    ("op_maskmovps_store_ymm", 32, True),
    ("op_maskmovpd_store_xmm", 64, False),
    ("op_maskmovpd_store_ymm", 64, True),
)

MASKMOV_EXPORTS = tuple(s[0] for s in MASKMOV_LOAD_SPECS) + tuple(s[0] for s in MASKMOV_STORE_SPECS)
# Negative-displacement exports take rcx=input_a+32, rdx=input_b+32, r8=out(32B)
NEG_DISP_EXPORTS = ("op_negdisp_load",)
ALL_EXPORTS = STD_EXPORTS + BLENDV_EXPORTS + MASKMOV_EXPORTS + NEG_DISP_EXPORTS


# --- Independent Python ISA reference model ---

def u32s(buf):
    return struct.unpack("<8I", buf)


def pack_u32s(vals):
    return struct.pack("<8I", *(v & MASK32 for v in vals))


def u64s(buf):
    return struct.unpack("<4Q", buf)


def pack_u64s(vals):
    return struct.pack("<4Q", *(v & MASK64 for v in vals))


def ref_bitwise(a, b, op, wide):
    qa, qb = u64s(a), u64s(b)
    n = 4 if wide else 2
    out = []
    for i in range(n):
        if op == "and":
            out.append(qa[i] & qb[i])
        elif op == "andn":
            out.append((~qa[i]) & qb[i] & MASK64)
        elif op == "or":
            out.append(qa[i] | qb[i])
        elif op == "xor":
            out.append(qa[i] ^ qb[i])
    if not wide:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vblendpd(a, b, imm8, wide):
    qa, qb = u64s(a), u64s(b)
    n = 4 if wide else 2
    out = [qb[i] if ((imm8 >> i) & 1) else qa[i] for i in range(n)]
    if not wide:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vblendps(a, b, imm8, wide):
    da, db = u32s(a), u32s(b)
    n = 8 if wide else 4
    out = [db[i] if ((imm8 >> i) & 1) else da[i] for i in range(n)]
    if not wide:
        out.extend([0] * 4)
    return pack_u32s(out)


def ref_vblendvpd(a, b, mask, wide):
    qa, qb, qm = u64s(a), u64s(b), u64s(mask)
    n = 4 if wide else 2
    out = [qb[i] if (qm[i] & (1 << 63)) else qa[i] for i in range(n)]
    if not wide:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vblendvps(a, b, mask, wide):
    da, db, dm = u32s(a), u32s(b), u32s(mask)
    n = 8 if wide else 4
    out = [db[i] if (dm[i] & 0x80000000) else da[i] for i in range(n)]
    if not wide:
        out.extend([0] * 4)
    return pack_u32s(out)


def ref_vbroadcastf128(a, offset=0):
    lane = a[offset:offset + 16]
    return lane + lane


def ref_vbroadcastsd(a, lane_idx=0):
    elem = a[lane_idx * 8:(lane_idx + 1) * 8]
    return elem * 4


def ref_vbroadcastss(a, lane_idx, wide):
    elem = a[lane_idx * 4:(lane_idx + 1) * 4]
    return elem * 8 if wide else elem * 4 + b"\x00" * 16


def ref_vextractf128(a, imm8):
    lane = a[16:32] if (imm8 & 1) else a[:16]
    return lane + b"\x00" * 16


def ref_vinsertf128(a, b, imm8):
    if imm8 & 1:
        return a[:16] + b[:16]
    return b[:16] + a[16:32]


def ref_vextractps(a, imm8):
    val = u32s(a)[imm8 & 3]
    return struct.pack("<Q", val) + FILL[8:]


def ref_vinsertps(a, b, imm8):
    da, db = list(u32s(a)[:4]), u32s(b)[:4]
    count_s = (imm8 >> 6) & 3
    count_d = (imm8 >> 4) & 3
    da[count_d] = db[count_s]
    for i in range(4):
        if (imm8 >> i) & 1:
            da[i] = 0
    return pack_u32s(da + [0, 0, 0, 0])


def ref_vperm2f128(a, b, imm8):
    lanes = (a[:16], a[16:32], b[:16], b[16:32])
    def pick(nibble):
        return b"\x00" * 16 if (nibble & 8) else lanes[nibble & 3]
    return pick(imm8 & 0xf) + pick((imm8 >> 4) & 0xf)


def ref_vpermilpd_imm(a, imm8, wide):
    qa = u64s(a)
    out = [
        qa[1 if ((imm8 >> 0) & 1) else 0],
        qa[1 if ((imm8 >> 1) & 1) else 0],
    ]
    if wide:
        out.extend([
            qa[3 if ((imm8 >> 2) & 1) else 2],
            qa[3 if ((imm8 >> 3) & 1) else 2],
        ])
    else:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vpermilpd_var(a, ctrl, wide):
    qa, qc = u64s(a), u64s(ctrl)
    out = [
        qa[1 if ((qc[0] >> 1) & 1) else 0],
        qa[1 if ((qc[1] >> 1) & 1) else 0],
    ]
    if wide:
        out.extend([
            qa[3 if ((qc[2] >> 1) & 1) else 2],
            qa[3 if ((qc[3] >> 1) & 1) else 2],
        ])
    else:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vpermilps_imm(a, imm8, wide):
    da = u32s(a)
    out = [da[(imm8 >> (2 * i)) & 3] for i in range(4)]
    if wide:
        out.extend(da[4 + ((imm8 >> (2 * i)) & 3)] for i in range(4))
    else:
        out.extend([0] * 4)
    return pack_u32s(out)


def ref_vpermilps_var(a, ctrl, wide):
    da, dc = u32s(a), u32s(ctrl)
    out = [da[dc[i] & 3] for i in range(4)]
    if wide:
        out.extend(da[4 + (dc[4 + i] & 3)] for i in range(4))
    else:
        out.extend([0] * 4)
    return pack_u32s(out)


def ref_vshufpd(a, b, imm8, wide):
    qa, qb = u64s(a), u64s(b)
    out = [
        qa[1 if ((imm8 >> 0) & 1) else 0],
        qb[1 if ((imm8 >> 1) & 1) else 0],
    ]
    if wide:
        out.extend([
            qa[3 if ((imm8 >> 2) & 1) else 2],
            qb[3 if ((imm8 >> 3) & 1) else 2],
        ])
    else:
        out.extend([0, 0])
    return pack_u64s(out)


def ref_vshufps(a, b, imm8, wide):
    da, db = u32s(a), u32s(b)
    out = [
        da[(imm8 >> 0) & 3],
        da[(imm8 >> 2) & 3],
        db[(imm8 >> 4) & 3],
        db[(imm8 >> 6) & 3],
    ]
    if wide:
        out.extend([
            da[4 + ((imm8 >> 0) & 3)],
            da[4 + ((imm8 >> 2) & 3)],
            db[4 + ((imm8 >> 4) & 3)],
            db[4 + ((imm8 >> 6) & 3)],
        ])
    else:
        out.extend([0] * 4)
    return pack_u32s(out)


def ref_vunpcklpd(a, b, wide):
    qa, qb = u64s(a), u64s(b)
    return pack_u64s((qa[0], qb[0], qa[2], qb[2]) if wide else (qa[0], qb[0], 0, 0))


def ref_vunpckhpd(a, b, wide):
    qa, qb = u64s(a), u64s(b)
    return pack_u64s((qa[1], qb[1], qa[3], qb[3]) if wide else (qa[1], qb[1], 0, 0))


def ref_vunpcklps(a, b, wide):
    da, db = u32s(a), u32s(b)
    out = [da[0], db[0], da[1], db[1]]
    out.extend([da[4], db[4], da[5], db[5]] if wide else [0, 0, 0, 0])
    return pack_u32s(out)


def ref_vunpckhps(a, b, wide):
    da, db = u32s(a), u32s(b)
    out = [da[2], db[2], da[3], db[3]]
    out.extend([da[6], db[6], da[7], db[7]] if wide else [0, 0, 0, 0])
    return pack_u32s(out)


def ref_vmovddup(a, wide):
    qa = u64s(a)
    return pack_u64s((qa[0], qa[0], qa[2], qa[2]) if wide else (qa[0], qa[0], 0, 0))


def ref_vmovshdup(a, wide):
    da = u32s(a)
    out = [da[1], da[1], da[3], da[3]]
    out.extend([da[5], da[5], da[7], da[7]] if wide else [0, 0, 0, 0])
    return pack_u32s(out)


def ref_vmovsldup(a, wide):
    da = u32s(a)
    out = [da[0], da[0], da[2], da[2]]
    out.extend([da[4], da[4], da[6], da[6]] if wide else [0, 0, 0, 0])
    return pack_u32s(out)


def ref_vmovmskpd(a, wide):
    qa = u64s(a)
    n = 4 if wide else 2
    mask = 0
    for i in range(n):
        if qa[i] & (1 << 63):
            mask |= 1 << i
    return struct.pack("<Q", mask) + FILL[8:]


def ref_vmovmskps(a, wide):
    da = u32s(a)
    n = 8 if wide else 4
    mask = 0
    for i in range(n):
        if da[i] & 0x80000000:
            mask |= 1 << i
    return struct.pack("<Q", mask) + FILL[8:]


def ref_pipeline(a, b):
    ymm0 = ref_vbroadcastf128(a)
    ymm1 = b
    ymm2 = ref_bitwise(ymm0, ymm1, "xor", True)
    ymm3 = ref_bitwise(ymm1, ymm2, "andn", True)
    ymm4 = ref_bitwise(ymm3, ymm0, "or", True)
    ymm5 = ref_vperm2f128(ymm4, ymm1, 0x21)
    ymm5 = ref_vshufps(ymm5, ymm2, 0xD8, True)
    ymm4 = ref_vblendvps(ymm5, ymm3, ymm1, True)
    ymm4 = ref_vpermilps_var(ymm4, ymm0, True)
    ymm3 = ref_vunpcklps(ymm4, ymm5, True)
    ymm2 = ref_vmovshdup(ymm3, True)
    xmm0 = ref_vextractf128(ymm2, 1)
    msk = struct.unpack("<Q", ref_vmovmskps(ymm2, True)[:8])[0]
    xmm1 = struct.pack("<I", msk) + b"\x00" * 28
    xmm0 = ref_vinsertps(xmm0, xmm1, 0x30)
    ymm0 = ref_vinsertf128(ymm2, xmm0, 0)
    return ymm0


def reference_std(name, a, b, altered=False):
    # Logical
    for stem, op in (("andpd", "and"), ("andps", "and"), ("andnpd", "andn"), ("andnps", "andn"),
                     ("orpd", "or"), ("orps", "or"), ("xorpd", "xor"), ("xorps", "xor")):
        if name == f"op_{stem}_xmm":
            return ref_bitwise(a, b, op, False)
        if name == f"op_{stem}_ymm":
            return ref_bitwise(a, b, op, True)
    # Blends
    if name == "op_blendpd_xmm":
        return ref_vblendpd(a, b, 0xFD, False)
    if name == "op_blendpd_ymm":
        return ref_vblendpd(a, b, 0xF5 if altered else 0xFA, True)
    if name == "op_blendpd_ymm_all":
        return ref_vblendpd(a, b, 0xFF, True)
    if name == "op_blendpd_ymm_none":
        return ref_vblendpd(a, b, 0x00, True)
    if name == "op_blendps_xmm":
        return ref_vblendps(a, b, 0xFA, False)
    if name == "op_blendps_ymm":
        return ref_vblendps(a, b, 0x55 if altered else 0xAA, True)
    if name == "op_blendps_ymm_all":
        return ref_vblendps(a, b, 0xFF, True)
    if name == "op_blendps_ymm_none":
        return ref_vblendps(a, b, 0x00, True)
    # Broadcasts
    if name == "op_broadcastf128":
        return ref_vbroadcastf128(a, 16 if altered else 0)
    if name == "op_broadcastsd_mem0":
        return ref_vbroadcastsd(a, 0)
    if name == "op_broadcastsd_mem1":
        return ref_vbroadcastsd(a, 1)
    if name == "op_broadcastsd_mem2":
        return ref_vbroadcastsd(a, 2)
    if name == "op_broadcastsd_mem3":
        return ref_vbroadcastsd(a, 3)
    if name == "op_broadcastsd_reg":
        return ref_vbroadcastsd(a, 0)
    if name == "op_broadcastss_xmm_mem0":
        return ref_vbroadcastss(a, 0, False)
    if name == "op_broadcastss_xmm_mem1":
        return ref_vbroadcastss(a, 1, False)
    if name == "op_broadcastss_xmm_mem2":
        return ref_vbroadcastss(a, 2, False)
    if name == "op_broadcastss_xmm_mem3":
        return ref_vbroadcastss(a, 3, False)
    if name == "op_broadcastss_ymm_mem4":
        return ref_vbroadcastss(a, 4, True)
    if name == "op_broadcastss_ymm_mem5":
        return ref_vbroadcastss(a, 5, True)
    if name == "op_broadcastss_ymm_mem6":
        return ref_vbroadcastss(a, 6, True)
    if name == "op_broadcastss_ymm_mem7":
        return ref_vbroadcastss(a, 7, True)
    if name == "op_broadcastss_xmm_reg":
        return ref_vbroadcastss(a, 0, False)
    if name == "op_broadcastss_ymm_reg":
        return ref_vbroadcastss(a, 0, True)
    # Lane ops
    if name == "op_extractf128_lo":
        return ref_vextractf128(a, 0xFE)
    if name == "op_extractf128_hi":
        return ref_vextractf128(a, 0xFF)
    if name == "op_insertf128_lo":
        return ref_vinsertf128(a, b, 0xFE)
    if name == "op_insertf128_hi":
        return ref_vinsertf128(a, b, 0xFF)
    if name == "op_extractps_lane0":
        return ref_vextractps(a, 0xFC)
    if name == "op_extractps_lane1":
        return ref_vextractps(a, 0xFD)
    if name == "op_extractps_lane2":
        return ref_vextractps(a, 0xFE)
    if name == "op_extractps_lane3":
        return ref_vextractps(a, 0xFF)
    if name == "op_insertps_s0d1_z0":
        return ref_vinsertps(a, b, 0x10)
    if name == "op_insertps_s3d2_zmask":
        return ref_vinsertps(a, b, 0xED if altered else 0xE5)
    if name == "op_insertps_zero_all":
        return ref_vinsertps(a, b, 0xBF)
    if name == "op_insertps_s2d3_z1":
        return ref_vinsertps(a, b, 0xB2)
    if name == "op_extractps_w1_lane2":
        return ref_vextractps(a, 0xFE)
    # Permutes
    if name == "op_perm2f128_mix":
        return ref_vperm2f128(a, b, 0x31 if altered else 0x20)
    if name == "op_perm2f128_swap":
        return ref_vperm2f128(a, b, 0x03)
    if name == "op_perm2f128_zero_lo":
        return ref_vperm2f128(a, b, 0x28)
    if name == "op_perm2f128_zero_hi":
        return ref_vperm2f128(a, b, 0x81)
    if name == "op_perm2f128_zero_both":
        return ref_vperm2f128(a, b, 0x88)
    if name == "op_perm2f128_ignored":
        return ref_vperm2f128(a, b, 0x45)
    if name == "op_permilpd_imm_xmm":
        return ref_vpermilpd_imm(a, 0xFD, False)
    if name == "op_permilpd_imm_ymm":
        return ref_vpermilpd_imm(a, 0xF5 if altered else 0xFA, True)
    if name == "op_permilpd_imm_ymm_lo":
        return ref_vpermilpd_imm(a, 0x00, True)
    if name == "op_permilpd_imm_ymm_hi":
        return ref_vpermilpd_imm(a, 0xFF, True)
    if name == "op_permilpd_var_xmm":
        return ref_vpermilpd_var(a, b, False)
    if name == "op_permilpd_var_ymm":
        return ref_vpermilpd_var(a, b, True)
    if name == "op_permilps_imm_xmm":
        return ref_vpermilps_imm(a, 0x1B, False)
    if name == "op_permilps_imm_ymm":
        return ref_vpermilps_imm(a, 0x4E if altered else 0xB1, True)
    if name == "op_permilps_imm_ymm_lo":
        return ref_vpermilps_imm(a, 0x00, True)
    if name == "op_permilps_imm_ymm_hi":
        return ref_vpermilps_imm(a, 0xFF, True)
    if name == "op_permilps_var_xmm":
        return ref_vpermilps_var(a, b, False)
    if name == "op_permilps_var_ymm":
        return ref_vpermilps_var(a, b, True)
    # Shuffles
    if name == "op_shufpd_xmm":
        return ref_vshufpd(a, b, 0xFE, False)
    if name == "op_shufpd_ymm":
        return ref_vshufpd(a, b, 0xF5 if altered else 0xFA, True)
    if name == "op_shufpd_ymm_lo":
        return ref_vshufpd(a, b, 0x00, True)
    if name == "op_shufpd_ymm_hi":
        return ref_vshufpd(a, b, 0xFF, True)
    if name == "op_shufps_xmm":
        return ref_vshufps(a, b, 0xE4, False)
    if name == "op_shufps_ymm":
        return ref_vshufps(a, b, 0x1B if altered else 0xB1, True)
    if name == "op_shufps_ymm_lo":
        return ref_vshufps(a, b, 0x00, True)
    if name == "op_shufps_ymm_hi":
        return ref_vshufps(a, b, 0xFF, True)
    if name == "op_shufpd_xmm_sel1":
        return ref_vshufpd(a, b, 0x01, False)
    if name == "op_shufps_ymm_sel2":
        return ref_vshufps(a, b, 0x4E, True)
    # Unpacks
    if name == "op_unpcklpd_xmm":
        return ref_vunpcklpd(a, b, False)
    if name == "op_unpcklpd_ymm":
        return ref_vunpcklpd(a, b, True)
    if name == "op_unpckhpd_xmm":
        return ref_vunpckhpd(a, b, False)
    if name == "op_unpckhpd_ymm":
        return ref_vunpckhpd(a, b, True)
    if name == "op_unpcklps_xmm":
        return ref_vunpcklps(a, b, False)
    if name == "op_unpcklps_ymm":
        return ref_vunpcklps(a, b, True)
    if name == "op_unpckhps_xmm":
        return ref_vunpckhps(a, b, False)
    if name == "op_unpckhps_ymm":
        return ref_vunpckhps(a, b, True)
    # Full-width moves, vlddqu, non-temporal
    if name in ("op_movapd_xmm", "op_movupd_xmm", "op_lddqu_xmm", "op_movntdqa_xmm"):
        return a[:16] + b"\x00" * 16
    if name in ("op_movapd_ymm", "op_movupd_ymm", "op_lddqu_ymm", "op_movntdqa_ymm",
                "op_movntdq_ymm", "op_movntpd_ymm", "op_movntps_ymm"):
        return a
    if name == "op_movapd_xmm_dirty":
        return a[:16] + b"\x00" * 16
    if name in ("op_movapd_store128", "op_movupd_store128",
                "op_movntdq_xmm", "op_movntpd_xmm", "op_movntps_xmm"):
        return a[:16] + FILL[16:]
    # Dup moves
    if name == "op_movddup_xmm":
        return ref_vmovddup(a, False)
    if name == "op_movddup_ymm":
        return ref_vmovddup(a, True)
    if name == "op_movshdup_xmm":
        return ref_vmovshdup(a, False)
    if name == "op_movshdup_ymm":
        return ref_vmovshdup(a, True)
    if name == "op_movsldup_xmm":
        return ref_vmovsldup(a, False)
    if name == "op_movsldup_ymm":
        return ref_vmovsldup(a, True)
    # Half-XMM register and memory moves
    if name == "op_movhlps":
        return b[8:16] + a[8:16] + b"\x00" * 16
    if name == "op_movlhps":
        return a[:8] + b[:8] + b"\x00" * 16
    if name in ("op_movhpd_load", "op_movhps_load"):
        return a[:8] + b[:8] + b"\x00" * 16
    if name in ("op_movhpd_store", "op_movhps_store"):
        return a[8:16] + FILL[8:]
    if name in ("op_movlpd_load", "op_movlps_load"):
        return b[:8] + a[8:16] + b"\x00" * 16
    if name in ("op_movlpd_store", "op_movlps_store"):
        return a[:8] + FILL[8:]
    # Scalar moves
    if name == "op_movsd_load":
        return a[:8] + b"\x00" * 24
    if name == "op_movsd_store":
        return a[:8] + FILL[8:]
    if name == "op_movsd_reg":
        return b[:8] + a[8:16] + b"\x00" * 16
    if name == "op_movss_load":
        return a[:4] + b"\x00" * 28
    if name == "op_movss_store":
        return a[:4] + FILL[4:]
    if name == "op_movss_reg":
        return b[:4] + a[4:16] + b"\x00" * 16
    # Sign-bit mask to GPR
    if name == "op_movmskpd_xmm":
        return ref_vmovmskpd(a, False)
    if name == "op_movmskpd_ymm":
        return ref_vmovmskpd(a, True)
    if name == "op_movmskps_xmm":
        return ref_vmovmskps(a, False)
    if name == "op_movmskps_ymm":
        return ref_vmovmskps(a, True)
    # VZEROUPPER and pipeline
    if name == "op_zeroupper":
        return a[:16] + b"\x00" * 16
    if name == "op_pipeline":
        return ref_pipeline(a, b)
    if name == "op_mov_load32":
        return b[:4] + b"\x00" * 4 + FILL[8:]
    if name == "op_zeroupper_ymm1":
        return b[:16] + b"\x00" * 16
    if name == "op_mov_store32":
        return b[:4] + FILL[4:]
    raise ValueError(f"unknown standard export {name}")


# --- Input vectors ---

def general_pairs():
    rng = random.Random(202611)
    distinct_a = bytes(range(0x01, 0x21))
    distinct_b = bytes(range(0x81, 0xA1))
    # IEEE-754 float32 special bit patterns: +0, -0, subnormal, +1, -1, +inf, sNaN, qNaN.
    float32_specials = pack_u32s((
        0x00000000, 0x80000000, 0x00000001, 0x3F800000,
        0xBF800000, 0x7F800000, 0x7F800001, 0x7FC00001,
    ))
    # IEEE-754 float64 special bit patterns: -0.0, sNaN, qNaN, -subnormal.
    float64_specials = pack_u64s((
        0x8000000000000000, 0x7FF0000000000001, 0x7FF8000000000001, 0x8000000000000001,
    ))
    # Sign-bit and bit-1 control boundary patterns (exercises vmovmsk*, vpermilpd var bit 1, etc.)
    sign_mix_32 = pack_u32s((
        0x7FFFFFFF, 0x80000000, 0x80000001, 0x00000000,
        0xFFFFFFFF, 0x7F800000, 0xFF800000, 0x00000002,
    ))
    ctrl_mix_64 = pack_u64s((
        0x0000000000000001,  # bit 0 set, bit 1 clear -> selects lane 0 in vpermilpd!
        0x0000000000000002,  # bit 1 set -> selects lane 1
        0xFFFFFFFFFFFFFFFD,  # all bits except bit 1 -> selects lane 0!
        0x8000000000000002,  # bit 63 and bit 1 set -> selects lane 1
    ))
    pairs = [
        (distinct_a, distinct_b),
        (b"\x00" * 32, b"\xFF" * 32),
        (b"\xFF" * 32, b"\x00" * 32),
        (float32_specials, distinct_b),
        (float64_specials, distinct_a),
        (sign_mix_32, ctrl_mix_64),
        (ctrl_mix_64, sign_mix_32),
    ]
    for _ in range(6):
        pairs.append((rng.randbytes(32), rng.randbytes(32)))
    return pairs


def permil_var_pairs(elem_bits):
    """Data/control pairs targeting bit-1 selection for vpermilpd and [1:0] + high-bit noise for vpermilps."""
    data = pack_u32s(0x11000000 + i * 0x1111111 for i in range(8))
    fp_data = pack_u64s((
        0x8000000000000000, 0x7FF0000000000001, 0x7FF8000000000001, 0xFFF0000000000000,
    ))
    if elem_bits == 64:
        ctrls = [
            pack_u64s((0, 2, 0, 2)),
            pack_u64s((2, 0, 2, 0)),
            pack_u64s((1, 1, 1, 1)),  # bit 0 only -> MUST select low lane!
            pack_u64s((0xFFFFFFFFFFFFFFFD, 0x0000000000000002, 0x8000000000000001, 0xFFFFFFFFFFFFFFFF)),
        ]
    else:
        ctrls = [
            pack_u32s((0, 1, 2, 3, 3, 2, 1, 0)),
            pack_u32s((3, 0, 2, 1, 1, 3, 0, 2)),
            pack_u32s((0xFFFFFFFC | i for i in (2, 3, 0, 1, 0, 0, 3, 3))),
            pack_u32s((0x80000000, 0x80000001, 0x1234567E, 0x1234567F, 4, 5, 6, 7)),
        ]
    return [(d, c) for d in (data, fp_data) for c in ctrls]


def mask_patterns(elem_bits, wide):
    """Full, partial, alternating, single-lane, zero and non-MSB-noise mask vectors."""
    lanes = (32 if wide else 16) // (elem_bits // 8)
    all_lanes = set(range(lanes))
    even_lanes = {i for i in range(lanes) if i % 2 == 0}
    odd_lanes = {i for i in range(lanes) if i % 2 == 1}
    upper_unused = set(range(lanes, 8 if elem_bits == 32 else 4))
    return [
        pack_mask_bits(elem_bits, all_lanes),
        pack_mask_bits(elem_bits, all_lanes | upper_unused),
        pack_mask_bits(elem_bits, even_lanes, non_msb_noise=True),
        pack_mask_bits(elem_bits, odd_lanes, non_msb_noise=True),
        pack_mask_bits(elem_bits, {0}, non_msb_noise=True),
        pack_mask_bits(elem_bits, {lanes - 1}, non_msb_noise=True),
        pack_mask_bits(elem_bits, set(), non_msb_noise=False),
        pack_mask_bits(elem_bits, upper_unused, non_msb_noise=True),
    ]


def pack_mask_bits(elem_bits, active_lanes, non_msb_noise=False):
    if elem_bits == 32:
        vals = []
        for i in range(8):
            if i in active_lanes:
                vals.append(0x80000000 if (i & 1) else 0xFFFFFFFF)
            else:
                vals.append(0x7FFFFFFF if non_msb_noise else 0x00000000)
        return pack_u32s(vals)
    vals = []
    for i in range(4):
        if i in active_lanes:
            vals.append(0x8000000000000000 if (i & 1) else 0xFFFFFFFFFFFFFFFF)
        else:
            vals.append(0x7FFFFFFFFFFFFFFF if non_msb_noise else 0x0000000000000000)
    return pack_u64s(vals)


def inputs_for(name):
    if name in ("op_permilpd_var_xmm", "op_permilpd_var_ymm"):
        return permil_var_pairs(64) + general_pairs()
    if name in ("op_permilps_var_xmm", "op_permilps_var_ymm"):
        return permil_var_pairs(32) + general_pairs()
    if name in ("op_movmskpd_xmm", "op_movmskpd_ymm"):
        wide = name.endswith("_ymm")
        return [(m, b"\xFF" * 32) for m in mask_patterns(64, wide)] + general_pairs()
    if name in ("op_movmskps_xmm", "op_movmskps_ymm"):
        wide = name.endswith("_ymm")
        return [(m, b"\xFF" * 32) for m in mask_patterns(32, wide)] + general_pairs()
    return general_pairs()


# --- Aligned and unaligned guarded buffer helpers ---

def alloc_aligned_buffer(payload, align=32, unalign_shift=0):
    """Allocate a guarded buffer whose payload starts at a 32-byte aligned address + unalign_shift."""
    total = len(GUARD_PRE) + len(payload) + len(GUARD_POST) + align + unalign_shift
    buf = ctypes.create_string_buffer(total)
    base_addr = ctypes.addressof(buf)
    # Place payload_addr at the first 32-byte aligned address after GUARD_PRE, then shift by unalign_shift.
    start_after_guard = base_addr + len(GUARD_PRE)
    rem = start_after_guard % align
    pad = (align - rem) % align
    payload_off = len(GUARD_PRE) + pad + unalign_shift
    pre_off = payload_off - len(GUARD_PRE)
    post_off = payload_off + len(payload)
    buf[pre_off:payload_off] = GUARD_PRE
    buf[payload_off:post_off] = payload
    buf[post_off:post_off + len(GUARD_POST)] = GUARD_POST
    return buf, base_addr + payload_off, pre_off, payload_off, post_off


def check_guards_and_read(buf, pre_off, payload_off, post_off, label):
    raw = buf.raw
    if raw[pre_off:payload_off] != GUARD_PRE or raw[post_off:post_off + len(GUARD_POST)] != GUARD_POST:
        raise AssertionError(f"guard bytes corrupted around {label}")
    return raw[payload_off:post_off]


def invoke_std(fn, a, b, unalign_shift=0):
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(a, 32, unalign_shift)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(b, 32, unalign_shift)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL, 32, unalign_shift)
    fn(ptr_a, ptr_b, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "input_a")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "input_b")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "output")


def invoke_blendv(fn, src1, src2, mask):
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(src1, 32)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(src2 + mask, 32)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL, 32)
    fn(ptr_a, ptr_b, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "blendv src1")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "blendv src2+mask")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "blendv output")


def invoke_neg_disp(fn, a, b):
    """Pass RCX/RDX one 32-byte vector past input_a/input_b so [reg - disp] reads the payloads."""
    buf_a, ptr_a, a_pre, a_off, a_post = alloc_aligned_buffer(a, 32)
    buf_b, ptr_b, b_pre, b_off, b_post = alloc_aligned_buffer(b, 32)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(FILL, 32)
    fn(ptr_a + 32, ptr_b + 32, ptr_out)
    check_guards_and_read(buf_a, a_pre, a_off, a_post, "neg-disp input_a")
    check_guards_and_read(buf_b, b_pre, b_off, b_post, "neg-disp input_b")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "neg-disp output")


def assert_equal_output(label, name, idx, got, want):
    if got != want:
        raise AssertionError(
            f"{label} {name} case {idx}: got {got.hex()} != expected {want.hex()}")


def bind_exports(dll_path, prefix="", names=ALL_EXPORTS):
    lib = ctypes.CDLL(str(Path(dll_path).resolve()))
    funcs = {}
    for name in names:
        fn = getattr(lib, prefix + name)
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = None
        funcs[name] = fn
    return funcs


# --- Masked load/store (vmaskmovps/vmaskmovpd) reference & fault-suppression harness ---

DATA_BUF_SIZE = 128
DATA_BASE_OFFSET = 32


def build_known_data_buffer():
    rng = random.Random(0xA7E0)
    buf = bytearray(rng.randbytes(DATA_BUF_SIZE))
    f32_specials = pack_u32s((
        0x00000000, 0x80000000, 0x00000001, 0x3F800000,
        0xBF800000, 0x7F800000, 0x7F800001, 0x7FC00001,
    ))
    f64_specials = pack_u64s((
        0x8000000000000000, 0x7FF0000000000001, 0x7FF8000000000001, 0x0000000000000001,
    ))
    buf[DATA_BASE_OFFSET:DATA_BASE_OFFSET + 32] = f32_specials
    buf[DATA_BASE_OFFSET + 32:DATA_BASE_OFFSET + 64] = f64_specials
    return bytes(buf)


def ref_vmaskmov_load(src_mem, src_offset, mask_bytes, elem_bits, wide):
    elem_bytes = elem_bits // 8
    lanes = (32 if wide else 16) // elem_bytes
    if elem_bits == 32:
        masks = struct.unpack("<8I", mask_bytes)
        sign_bit = 0x80000000
    else:
        masks = struct.unpack("<4Q", mask_bytes)
        sign_bit = 0x8000000000000000
    out = bytearray(32)
    for i in range(lanes):
        if masks[i] & sign_bit:
            off = src_offset + i * elem_bytes
            out[i * elem_bytes:(i + 1) * elem_bytes] = src_mem[off:off + elem_bytes]
    return bytes(out) + mask_bytes


def ref_vmaskmov_store(init_mem, src_bytes, mask_bytes, elem_bits, wide):
    elem_bytes = elem_bits // 8
    lanes = (32 if wide else 16) // elem_bytes
    if elem_bits == 32:
        masks = struct.unpack("<8I", mask_bytes)
        sign_bit = 0x80000000
    else:
        masks = struct.unpack("<4Q", mask_bytes)
        sign_bit = 0x8000000000000000
    out = bytearray(init_mem)
    for i in range(lanes):
        if masks[i] & sign_bit:
            out[i * elem_bytes:(i + 1) * elem_bytes] = src_bytes[i * elem_bytes:(i + 1) * elem_bytes]
    return bytes(out)


def maskmov_cases_for(elem_bits, wide):
    seed_a = bytes(range(0x21, 0x41))
    seed_b = bytes(range(0x81, 0xA1))
    f32_specials = pack_u32s((
        0x00000000, 0x80000000, 0x00000001, 0x3F800000,
        0xBF800000, 0x7F800000, 0x7F800001, 0x7FC00001,
    ))
    masks = mask_patterns(elem_bits, wide)
    cases = [
        (seed_a, masks[0], False),
        (f32_specials, masks[1], False),
        (seed_b, masks[2], False),
        (seed_a, masks[3], False),
        (seed_b, masks[4], False),
        (seed_a, masks[5], False),
        # Zero-mask cases with NULL pointer (fault suppression).
        (seed_a, masks[6], True),
        (seed_b, masks[7], True),
    ]
    rng = random.Random(0x66A0 + elem_bits * 13 + int(wide))
    lanes = (32 if wide else 16) // (elem_bits // 8)
    for _ in range(4):
        active = {i for i in range(lanes) if rng.getrandbits(1)}
        cases.append((rng.randbytes(32), pack_mask_bits(elem_bits, active, non_msb_noise=True), False))
    return cases


def invoke_maskmov_load(fn, src_ptr, init_dest, mask):
    buf_ctrl, ptr_ctrl, c_pre, c_off, c_post = alloc_aligned_buffer(init_dest + mask, 32)
    buf_out, ptr_out, o_pre, o_off, o_post = alloc_aligned_buffer(OUT_FILL_64, 32)
    fn(src_ptr, ptr_ctrl, ptr_out)
    check_guards_and_read(buf_ctrl, c_pre, c_off, c_post, "vmaskmov load ctrl")
    return check_guards_and_read(buf_out, o_pre, o_off, o_post, "vmaskmov load output")


def invoke_maskmov_store(fn, src_vec, mask, init_mem, dst_ptr_override=None, unalign_shift=0):
    buf_src, ptr_src, s_pre, s_off, s_post = alloc_aligned_buffer(src_vec, 32)
    buf_mask, ptr_mask, m_pre, m_off, m_post = alloc_aligned_buffer(mask, 32)
    if dst_ptr_override is not None:
        fn(ptr_src, ptr_mask, dst_ptr_override)
        check_guards_and_read(buf_src, s_pre, s_off, s_post, "vmaskmov store src")
        check_guards_and_read(buf_mask, m_pre, m_off, m_post, "vmaskmov store mask")
        return init_mem
    buf_dst, ptr_dst, d_pre, d_off, d_post = alloc_aligned_buffer(init_mem, 32, unalign_shift)
    fn(ptr_src, ptr_mask, ptr_dst)
    check_guards_and_read(buf_src, s_pre, s_off, s_post, "vmaskmov store src")
    check_guards_and_read(buf_mask, m_pre, m_off, m_post, "vmaskmov store mask")
    return check_guards_and_read(buf_dst, d_pre, d_off, d_post, "vmaskmov store dst")


def check_guard_page_fault_suppression(native_mem, lifted_mem):
    """Allocate a PAGE_READWRITE page followed by PAGE_NOACCESS and verify masked-off lanes never fault."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualProtect.argtypes = [
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    kernel32.VirtualProtect.restype = ctypes.c_int
    kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    kernel32.VirtualFree.restype = ctypes.c_int

    page_size = 4096
    base = kernel32.VirtualAlloc(None, page_size * 2, 0x3000, 0x04)
    if not base:
        raise OSError(f"VirtualAlloc failed with error {ctypes.get_last_error()}")
    checked = 0
    try:
        old_prot = ctypes.c_uint32(0)
        if not kernel32.VirtualProtect(base + page_size, page_size, 0x01, ctypes.byref(old_prot)):
            raise OSError(f"VirtualProtect(PAGE_NOACCESS) failed with error {ctypes.get_last_error()}")
        page0_data = bytes((i * 73 + 19) & 0xFF for i in range(page_size))
        ctypes.memmove(base, page0_data, page_size)
        page1 = base + page_size
        name = "op_broadcastss_ymm_mem7"
        src = page1 - 32
        native_out = ctypes.create_string_buffer(32)
        lifted_out = ctypes.create_string_buffer(32)
        native_mem[name](src, src, ctypes.addressof(native_out))
        lifted_mem[name](src, src, ctypes.addressof(lifted_out))
        # The broadcast reads only the last dword before the protected page.
        want = ref_vbroadcastss(page0_data[-32:], 7, True)
        assert_equal_output("native-guard-broadcast", name, 0, native_out.raw, want)
        assert_equal_output("lifted-guard-broadcast", name, 0, lifted_out.raw, want)

        init_dest = bytes(range(0xA1, 0xC1))

        for name, elem_bits, wide, _ in MASKMOV_LOAD_SPECS:
            elem_bytes = elem_bits // 8
            total_bytes = 32 if wide else 16
            half_bytes = total_bytes // 2
            lanes = total_bytes // elem_bytes
            active = set(range(lanes // 2))
            mask = pack_mask_bits(elem_bits, active, non_msb_noise=True)
            straddle_off = page_size - half_bytes
            want = ref_vmaskmov_load(page0_data, straddle_off, mask, elem_bits, wide)
            got_n = invoke_maskmov_load(native_mem[name], base + straddle_off, init_dest, mask)
            got_l = invoke_maskmov_load(lifted_mem[name], base + straddle_off, init_dest, mask)
            assert_equal_output("guard-native", name, 0, got_n, want)
            assert_equal_output("guard-lifted", name, 0, got_l, want)
            zero_mask = pack_mask_bits(elem_bits, set(), non_msb_noise=True)
            want_z = b"\x00" * 32 + zero_mask
            got_nz = invoke_maskmov_load(native_mem[name], page1, init_dest, zero_mask)
            got_lz = invoke_maskmov_load(lifted_mem[name], page1, init_dest, zero_mask)
            assert_equal_output("guard-zero-native", name, 1, got_nz, want_z)
            assert_equal_output("guard-zero-lifted", name, 1, got_lz, want_z)
            checked += 2

        src_vec = bytes(range(0x10, 0x30))
        for name, elem_bits, wide in MASKMOV_STORE_SPECS:
            elem_bytes = elem_bits // 8
            total_bytes = 32 if wide else 16
            half_bytes = total_bytes // 2
            lanes = total_bytes // elem_bytes
            active = set(range(lanes // 2))
            mask = pack_mask_bits(elem_bits, active, non_msb_noise=True)
            straddle_off = page_size - half_bytes
            init_tail = bytes(0xE0 + i for i in range(half_bytes))
            want_tail = ref_vmaskmov_store(
                init_tail + b"\x00" * (32 - half_bytes), src_vec, mask, elem_bits, wide)[:half_bytes]
            for label, fn in (("guard-native", native_mem[name]), ("guard-lifted", lifted_mem[name])):
                ctypes.memmove(base + straddle_off, init_tail, half_bytes)
                invoke_maskmov_store(fn, src_vec, mask, init_tail, dst_ptr_override=base + straddle_off)
                got_tail = ctypes.string_at(base + straddle_off, half_bytes)
                assert_equal_output(label, name, 0, got_tail, want_tail)
            zero_mask = pack_mask_bits(elem_bits, set(), non_msb_noise=True)
            invoke_maskmov_store(native_mem[name], src_vec, zero_mask, STORE_INIT, dst_ptr_override=page1)
            invoke_maskmov_store(lifted_mem[name], src_vec, zero_mask, STORE_INIT, dst_ptr_override=page1)
            checked += 2
    finally:
        kernel32.VirtualFree(base, 0, 0x8000)
    return checked


def check_differential(native_path, lifted_path, altered_path):
    native = bind_exports(native_path)
    lifted = bind_exports(lifted_path, "lifted_")
    altered = bind_exports(altered_path, "lifted_")

    std_total = 0
    for name in STD_EXPORTS:
        for idx, (a, b) in enumerate(inputs_for(name)):
            want = reference_std(name, a, b, altered=False)
            got_n = invoke_std(native[name], a, b)
            got_l = invoke_std(lifted[name], a, b)
            assert_equal_output("native", name, idx, got_n, want)
            assert_equal_output("lifted", name, idx, got_l, want)
            std_total += 1

    # Unaligned pointer paths (+1, +3, +7 byte offsets) for unaligned-safe forms.
    unalign_total = 0
    sample_pairs = general_pairs()[:4]
    for name in UNALIGNED_SAFE_EXPORTS:
        for shift in (1, 3, 7):
            for idx, (a, b) in enumerate(sample_pairs):
                want = reference_std(name, a, b, altered=False)
                got_n = invoke_std(native[name], a, b, unalign_shift=shift)
                got_l = invoke_std(lifted[name], a, b, unalign_shift=shift)
                assert_equal_output(f"native-unaligned+{shift}", name, idx, got_n, want)
                assert_equal_output(f"lifted-unaligned+{shift}", name, idx, got_l, want)
                unalign_total += 1

    # An XMM vmovapd needs only 16-byte alignment; this pins against a 32-byte requirement on the XMM form.
    name = "op_movapd_store128"
    a, b = bytes(range(32)), bytes(32)
    want = reference_std(name, a, b)
    got_n = invoke_std(native[name], a, b, unalign_shift=16)
    got_l = invoke_std(lifted[name], a, b, unalign_shift=16)
    assert_equal_output("native-align16", name, 0, got_n, want)
    assert_equal_output("lifted-align16", name, 0, got_l, want)

    # Negative displacements ([rcx - 32] vector load, [rdx - 8] GPR load).
    neg_total = 0
    for name in NEG_DISP_EXPORTS:
        for idx, (a, b) in enumerate(general_pairs()):
            want = a[:8] + b[24:32] + a[16:32]
            got_n = invoke_neg_disp(native[name], a, b)
            got_l = invoke_neg_disp(lifted[name], a, b)
            assert_equal_output("native", name, idx, got_n, want)
            assert_equal_output("lifted", name, idx, got_l, want)
            neg_total += 1

    # Variable blend (3 vector inputs: src1, src2, mask).
    blendv_total = 0
    for name in BLENDV_EXPORTS:
        elem_bits = 64 if "pd" in name else 32
        wide = name.endswith("_ymm")
        ref_fn = ref_vblendvpd if elem_bits == 64 else ref_vblendvps
        masks = mask_patterns(elem_bits, wide)
        for idx, (src1, src2) in enumerate(general_pairs()[:6]):
            for m_idx, mask in enumerate(masks):
                want = ref_fn(src1, src2, mask, wide)
                got_n = invoke_blendv(native[name], src1, src2, mask)
                got_l = invoke_blendv(lifted[name], src1, src2, mask)
                assert_equal_output("native", name, idx * 10 + m_idx, got_n, want)
                assert_equal_output("lifted", name, idx * 10 + m_idx, got_l, want)
                blendv_total += 1

    # Masked load/store differential checks (aligned and unaligned +3) + guard-page fault suppression.
    known_data = build_known_data_buffer()
    buf_data, ptr_data_base, d_pre, d_off, d_post = alloc_aligned_buffer(known_data, 32)
    base_ptr = ptr_data_base + DATA_BASE_OFFSET

    maskmov_total = 0
    for name, elem_bits, wide, _ in MASKMOV_LOAD_SPECS:
        for idx, (init_dest, mask, null_ptr) in enumerate(maskmov_cases_for(elem_bits, wide)):
            for shift in (0, 3):
                if null_ptr and shift != 0:
                    continue
                want = ref_vmaskmov_load(known_data, DATA_BASE_OFFSET + shift, mask, elem_bits, wide)
                ptr = 0 if null_ptr else base_ptr + shift
                got_n = invoke_maskmov_load(native[name], ptr, init_dest, mask)
                got_l = invoke_maskmov_load(lifted[name], ptr, init_dest, mask)
                assert_equal_output("native", name, idx * 10 + shift, got_n, want)
                assert_equal_output("lifted", name, idx * 10 + shift, got_l, want)
                maskmov_total += 1

    for name, elem_bits, wide in MASKMOV_STORE_SPECS:
        for idx, (src_vec, mask, null_ptr) in enumerate(maskmov_cases_for(elem_bits, wide)):
            for shift in (0, 3):
                if null_ptr and shift != 0:
                    continue
                want = ref_vmaskmov_store(STORE_INIT, src_vec, mask, elem_bits, wide)
                override = 0 if null_ptr else None
                got_n = invoke_maskmov_store(native[name], src_vec, mask, STORE_INIT,
                                             dst_ptr_override=override, unalign_shift=shift)
                got_l = invoke_maskmov_store(lifted[name], src_vec, mask, STORE_INIT,
                                             dst_ptr_override=override, unalign_shift=shift)
                assert_equal_output("native", name, idx * 10 + shift, got_n, want)
                assert_equal_output("lifted", name, idx * 10 + shift, got_l, want)
                maskmov_total += 1

    check_guards_and_read(buf_data, d_pre, d_off, d_post, "maskmov known_data")
    guard_total = check_guard_page_fault_suppression(native, lifted)

    # Altered build negative controls (8 exports across immediate blends, broadcasts, inserts, permutes, shuffles, maskmov).
    altered_std = (
        "op_blendpd_ymm", "op_blendps_ymm", "op_broadcastf128", "op_insertps_s3d2_zmask",
        "op_perm2f128_mix", "op_permilpd_imm_ymm", "op_permilps_imm_ymm",
        "op_shufpd_ymm", "op_shufps_ymm",
    )
    changed = {}
    for name in altered_std:
        diffs = 0
        for idx, (a, b) in enumerate(inputs_for(name)):
            want_alt = reference_std(name, a, b, altered=True)
            want_orig = reference_std(name, a, b, altered=False)
            got_alt = invoke_std(altered[name], a, b)
            assert_equal_output("altered", name, idx, got_alt, want_alt)
            if got_alt != want_orig:
                diffs += 1
        if not diffs:
            raise AssertionError(f"altered control {name} did not diverge from original")
        changed[name] = diffs

    m_diffs = 0
    for idx, (init_dest, mask, null_ptr) in enumerate(maskmov_cases_for(32, True)):
        want_alt = ref_vmaskmov_load(known_data, DATA_BASE_OFFSET + 16, mask, 32, True)
        want_orig = ref_vmaskmov_load(known_data, DATA_BASE_OFFSET, mask, 32, True)
        got_alt = invoke_maskmov_load(altered["op_maskmovps_load_ymm"],
                                      0 if null_ptr else base_ptr, init_dest, mask)
        assert_equal_output("altered", "op_maskmovps_load_ymm", idx, got_alt, want_alt)
        if got_alt != want_orig:
            m_diffs += 1
    if not m_diffs:
        raise AssertionError("altered control op_maskmovps_load_ymm did not diverge from original")
    changed["op_maskmovps_load_ymm"] = m_diffs

    # Output mutation negative control.
    sample_a, sample_b = general_pairs()[0]
    expected = reference_std("op_pipeline", sample_a, sample_b)
    mutated = bytearray(expected)
    mutated[19] ^= 0x01
    try:
        assert_equal_output("mutation", "op_pipeline", 0, bytes(mutated), expected)
    except AssertionError:
        pass
    else:
        raise AssertionError("mutated output was not caught by comparator")

    total = std_total + unalign_total + blendv_total + maskmov_total + neg_total
    print(f"{total} native/lifted/reference AVX-move cases ({std_total} standard across {len(STD_EXPORTS)} exports, "
          f"{unalign_total} unaligned across {len(UNALIGNED_SAFE_EXPORTS)} exports, "
          f"{blendv_total} variable-blend across {len(BLENDV_EXPORTS)} exports, "
          f"{maskmov_total} vmaskmov across {len(MASKMOV_EXPORTS)} exports) and {guard_total} guard-page "
          f"fault-suppression cases matched bit-for-bit; altered controls diverged on {changed}; "
          f"output mutation control caught")


# --- Alternative-encoding acceptance & translator rejection controls ---

def encode_seq(*instructions):
    enc = Encoder(64)
    ip = 0x1000
    for ins in instructions:
        ip += enc.encode(ins, ip)
    return enc.take_buffer()


def check_alt_encodings_and_rejections(original_path):
    """Verify alternate VEX encodings lift cleanly and all out-of-scope/illegal forms reject."""
    ld_ymm0 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))
    ld_ymm1 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM1, MemoryOperand(R.RCX))
    ld_ymm2 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_YMM_YMMM256, R.YMM2, MemoryOperand(R.RDX))
    ld_xmm1 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM1, MemoryOperand(R.RCX))
    ld_xmm2 = Instruction.create_reg_mem(Code.VEX_VMOVDQU_XMM_XMMM128, R.XMM2, MemoryOperand(R.RDX))
    st_r8_rax = Instruction.create_mem_reg(Code.MOV_RM64_R64, MemoryOperand(R.R8), R.RAX)
    ret_ins = Instruction.create(Code.RETNQ)

    # Alternate valid VEX encodings that MASM may not emit by default:
    # 0F11 reg-reg vmovsd/vmovss and W1 r64 vmovmskpd/vmovmskps/vextractps.
    alt_accept = [
        ("vmovsd_0f11", (ld_xmm1, ld_xmm2, Instruction.create_reg_reg_reg(
            Code.VEX_VMOVSD_XMM_XMM_XMM_0F11, R.XMM0, R.XMM1, R.XMM2), ret_ins)),
        ("vmovss_0f11", (ld_xmm1, ld_xmm2, Instruction.create_reg_reg_reg(
            Code.VEX_VMOVSS_XMM_XMM_XMM_0F11, R.XMM0, R.XMM1, R.XMM2), ret_ins)),
        ("vmovmskpd_r64_xmm", (ld_xmm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPD_R64_XMM, R.RAX, R.XMM1), st_r8_rax, ret_ins)),
        ("vmovmskpd_r64_ymm", (ld_ymm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPD_R64_YMM, R.RAX, R.YMM1), st_r8_rax, ret_ins)),
        ("vmovmskps_r64_xmm", (ld_xmm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPS_R64_XMM, R.RAX, R.XMM1), st_r8_rax, ret_ins)),
        ("vmovmskps_r64_ymm", (ld_ymm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPS_R64_YMM, R.RAX, R.YMM1), st_r8_rax, ret_ins)),
        ("vextractps_r64", (ld_xmm1, Instruction.create_reg_reg_u32(
            Code.VEX_VEXTRACTPS_R64M32_XMM_IMM8, R.RAX, R.XMM1, 2), st_r8_rax, ret_ins)),
    ]
    for label, seq in alt_accept:
        src, count = lift_code(label, 0x1000, encode_seq(*seq))
        assert f"lifted_{label}" in src and count == len(seq), (label, count)

    # 1) Memory-operand encodings of all 20 register-only AVX forms must be rejected explicitly.
    # (The eight logical ops take a memory source and are covered by avx_move_mem_check.py.)
    reg_only_mem_cases = [
        ("vblendpd_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VBLENDPD_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0xA)),
        ("vblendps_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VBLENDPS_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0xAA)),
        ("vblendvpd_mem", Instruction.create_reg_reg_mem_reg(Code.VEX_VBLENDVPD_YMM_YMM_YMMM256_YMM, R.YMM0, R.YMM1, MemoryOperand(R.RDX), R.YMM2)),
        ("vblendvps_mem", Instruction.create_reg_reg_mem_reg(Code.VEX_VBLENDVPS_YMM_YMM_YMMM256_YMM, R.YMM0, R.YMM1, MemoryOperand(R.RDX), R.YMM2)),
        ("vextractf128_mem", Instruction.create_mem_reg_u32(Code.VEX_VEXTRACTF128_XMMM128_YMM_IMM8, MemoryOperand(R.R8), R.YMM1, 1)),
        ("vinsertf128_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VINSERTF128_YMM_YMM_XMMM128_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 1)),
        ("vextractps_mem", Instruction.create_mem_reg_u32(Code.VEX_VEXTRACTPS_RM32_XMM_IMM8, MemoryOperand(R.R8), R.XMM1, 2)),
        ("vinsertps_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VINSERTPS_XMM_XMM_XMMM32_IMM8, R.XMM0, R.XMM1, MemoryOperand(R.RDX), 0x10)),
        ("vperm2f128_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VPERM2F128_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0x20)),
        ("vpermilpd_imm_mem", Instruction.create_reg_mem_u32(Code.VEX_VPERMILPD_YMM_YMMM256_IMM8, R.YMM0, MemoryOperand(R.RCX), 0xA)),
        ("vpermilpd_var_mem", Instruction.create_reg_reg_mem(Code.VEX_VPERMILPD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vpermilps_imm_mem", Instruction.create_reg_mem_u32(Code.VEX_VPERMILPS_YMM_YMMM256_IMM8, R.YMM0, MemoryOperand(R.RCX), 0x1B)),
        ("vpermilps_var_mem", Instruction.create_reg_reg_mem(Code.VEX_VPERMILPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vshufpd_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VSHUFPD_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0xA)),
        ("vshufps_mem", Instruction.create_reg_reg_mem_u32(Code.VEX_VSHUFPS_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, MemoryOperand(R.RDX), 0xE4)),
        ("vunpcklpd_mem", Instruction.create_reg_reg_mem(Code.VEX_VUNPCKLPD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vunpcklps_mem", Instruction.create_reg_reg_mem(Code.VEX_VUNPCKLPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vunpckhpd_mem", Instruction.create_reg_reg_mem(Code.VEX_VUNPCKHPD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vunpckhps_mem", Instruction.create_reg_reg_mem(Code.VEX_VUNPCKHPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, MemoryOperand(R.RDX))),
        ("vmovddup_mem", Instruction.create_reg_mem(Code.VEX_VMOVDDUP_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))),
        ("vmovshdup_mem", Instruction.create_reg_mem(Code.VEX_VMOVSHDUP_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))),
        ("vmovsldup_mem", Instruction.create_reg_mem(Code.VEX_VMOVSLDUP_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))),
    ]

    reg_only = dict(reg_only_mem_cases)

    # 2) Carved-out move forms owned by cpu_vex_aot.py (PR #325) (vmovdqa, vmovups, vmovaps, vmovq, vmovd must reject).
    ld_rax = Instruction.create_reg_mem(Code.MOV_R64_RM64, R.RAX, MemoryOperand(R.RCX))
    carved_out_cases = [
        ("carved_vmovdqa_load_xmm", Instruction.create_reg_mem(Code.VEX_VMOVDQA_XMM_XMMM128, R.XMM0, MemoryOperand(R.RCX))),
        ("carved_vmovdqa_store_ymm", Instruction.create_mem_reg(Code.VEX_VMOVDQA_YMMM256_YMM, MemoryOperand(R.R8), R.YMM1)),
        ("carved_vmovups_load_ymm", Instruction.create_reg_mem(Code.VEX_VMOVUPS_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))),
        ("carved_vmovups_reg_xmm", Instruction.create_reg_reg(Code.VEX_VMOVUPS_XMM_XMMM128, R.XMM0, R.XMM1)),
        ("carved_vmovaps_load_ymm", Instruction.create_reg_mem(Code.VEX_VMOVAPS_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX))),
        ("carved_vmovaps_store_xmm", Instruction.create_mem_reg(Code.VEX_VMOVAPS_XMMM128_XMM, MemoryOperand(R.R8), R.XMM1)),
        ("carved_vmovd_mem_load", Instruction.create_reg_mem(Code.VEX_VMOVD_XMM_RM32, R.XMM0, MemoryOperand(R.RCX))),
        ("carved_vmovd_from_gpr", Instruction.create_reg_reg(Code.VEX_VMOVD_XMM_RM32, R.XMM0, R.EAX)),
        ("carved_vmovd_to_gpr", Instruction.create_reg_reg(Code.VEX_VMOVD_RM32_XMM, R.EAX, R.XMM1)),
        ("carved_vmovq_mem_load", Instruction.create_reg_mem(Code.VEX_VMOVQ_XMM_XMMM64, R.XMM0, MemoryOperand(R.RCX))),
        ("carved_vmovq_from_gpr", Instruction.create_reg_reg(Code.VEX_VMOVQ_XMM_RM64, R.XMM0, R.RAX)),
        ("carved_vmovq_to_gpr", Instruction.create_reg_reg(Code.VEX_VMOVQ_RM64_XMM, R.RAX, R.XMM1)),
        ("carved_vmovq_d6_reg", Instruction.create_reg_reg(Code.VEX_VMOVQ_XMMM64_XMM, R.XMM0, R.XMM1)),
    ]

    # 3) Out-of-scope instructions (FP arithmetic, compares/tests/comis, AVX2, SSE, GPR arithmetic, branches).
    out_of_scope_cases = [
        ("vcmpps", Instruction.create_reg_reg_reg_u32(Code.VEX_VCMPPS_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, R.YMM2, 0)),
        ("vcmppd", Instruction.create_reg_reg_reg_u32(Code.VEX_VCMPPD_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, R.YMM2, 0)),
        ("vtestps", Instruction.create_reg_reg(Code.VEX_VTESTPS_YMM_YMMM256, R.YMM1, R.YMM2)),
        ("vtestpd", Instruction.create_reg_reg(Code.VEX_VTESTPD_YMM_YMMM256, R.YMM1, R.YMM2)),
        ("vcomiss", Instruction.create_reg_reg(Code.VEX_VCOMISS_XMM_XMMM32, R.XMM1, R.XMM2)),
        ("vucomisd", Instruction.create_reg_reg(Code.VEX_VUCOMISD_XMM_XMMM64, R.XMM1, R.XMM2)),
        ("vaddps", Instruction.create_reg_reg_reg(Code.VEX_VADDPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2)),
        ("vpaddd", Instruction.create_reg_reg_reg(Code.VEX_VPADDD_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM2)),
        ("vperm2i128", Instruction.create_reg_reg_reg_u32(Code.VEX_VPERM2I128_YMM_YMM_YMMM256_IMM8, R.YMM0, R.YMM1, R.YMM2, 0x20)),
        ("movaps_sse", Instruction.create_reg_reg(Code.MOVAPS_XMM_XMMM128, R.XMM0, R.XMM1)),
        ("add_gpr", Instruction.create_reg_reg(Code.ADD_R32_RM32, R.EAX, R.ECX)),
        ("jmp_branch", Instruction.create_branch(Code.JMP_REL8_64, 0x1020)),
        ("ud2", Instruction.create(Code.UD2)),
    ]

    # 4) EVEX, prefixes, ABI, GPR/vector state and addressing violations.
    seg_bcast = Instruction.create_reg_mem(Code.VEX_VBROADCASTF128_YMM_M128, R.YMM0, MemoryOperand(R.RCX))
    seg_bcast.segment_prefix = R.GS
    seg_maskmov = Instruction.create_reg_reg_mem(Code.VEX_VMASKMOVPS_YMM_YMM_M256, R.YMM0, R.YMM1, MemoryOperand(R.RCX))
    seg_maskmov.segment_prefix = R.FS
    evex_andps = Instruction.create_reg_reg_reg(Code.EVEX_VANDPS_YMM_K1Z_YMM_YMMM256B32, R.YMM0, R.YMM1, R.YMM2)
    evex_andps.op_mask = R.K1
    evex_movupd = Instruction.create_reg_mem(Code.EVEX_VMOVUPD_YMM_K1Z_YMMM256, R.YMM0, MemoryOperand(R.RCX))
    evex_movupd.op_mask = R.K1

    other_cases = [
        ("evex_vandps", (ld_ymm1, ld_ymm2, evex_andps, ret_ins), "unsupported EVEX instruction"),
        ("evex_vmovupd", (evex_movupd, ret_ins), "unsupported EVEX instruction"),
        ("nonvolatile_ymm6", (Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM6, MemoryOperand(R.RCX)), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_xmm6", (ld_ymm1, Instruction.create_reg_reg_u32(
            Code.VEX_VEXTRACTF128_XMMM128_YMM_IMM8, R.XMM6, R.YMM1, 0), ret_ins), "nonvolatile vector register"),
        ("nonvolatile_gpr_rbx", (ld_ymm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPS_R64_YMM, R.RBX, R.YMM1), ret_ins), "nonvolatile GPR"),
        ("arg_gpr_clobber_rcx", (ld_ymm1, Instruction.create_reg_reg(
            Code.VEX_VMOVMSKPS_R64_YMM, R.RCX, R.YMM1), ret_ins), "unsupported GPR"),
        ("mov_reg_to_reg", (Instruction.create_reg_reg(
            Code.MOV_R64_RM64, R.RAX, R.RCX), ret_ins), "unsupported mov instruction"),
        ("undefined_ymm3", (ld_ymm1, Instruction.create_reg_reg_reg(
            Code.VEX_VXORPS_YMM_YMM_YMMM256, R.YMM0, R.YMM1, R.YMM3), ret_ins), "undefined vector register ymm3"),
        ("undefined_gpr_rax", (Instruction.create_mem_reg(
            Code.MOV_RM64_R64, MemoryOperand(R.R8), R.RAX), ret_ins), "undefined GPR rax"),
        ("maskmov_undef_mask", (Instruction.create_reg_reg_mem(
            Code.VEX_VMASKMOVPS_YMM_YMM_M256, R.YMM0, R.YMM3, MemoryOperand(R.RCX)), ret_ins),
         "undefined vector register ymm3"),
        ("maskmov_store_undef_src", (ld_ymm1, Instruction.create_mem_reg_reg(
            Code.VEX_VMASKMOVPD_M256_YMM_YMM, MemoryOperand(R.R8), R.YMM1, R.YMM3), ret_ins),
         "undefined vector register ymm3"),
        ("rip_rel_mem", (Instruction.create_reg_mem(
            Code.VEX_VBROADCASTF128_YMM_M128, R.YMM0, MemoryOperand(R.RIP, displ=0x1010, displ_size=1)), ret_ins),
         "unsupported memory operand"),
        ("rsp_mem", (Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.RSP)), ret_ins),
         "unsupported memory operand"),
        ("addr32_mem", (Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.ECX)), ret_ins),
         "unsupported memory operand"),
        ("indexed_mem", (Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.RCX, R.RDX, 4)), ret_ins),
         "unsupported memory operand"),
        ("non_arg_base_rax", (Instruction.create_reg_mem(
            Code.VEX_VMOVUPD_YMM_YMMM256, R.YMM0, MemoryOperand(R.RAX)), ret_ins),
         "unsupported memory operand"),
        ("maskmov_rip_rel", (ld_ymm1, Instruction.create_reg_reg_mem(
            Code.VEX_VMASKMOVPD_YMM_YMM_M256, R.YMM0, R.YMM1, MemoryOperand(R.RIP, displ=0x1010, displ_size=1)), ret_ins),
         "unsupported memory operand"),
        ("maskmov_indexed", (ld_ymm1, Instruction.create_reg_reg_mem(
            Code.VEX_VMASKMOVPS_YMM_YMM_M256, R.YMM0, R.YMM1, MemoryOperand(R.RCX, R.RDX, 4)), ret_ins),
         "unsupported memory operand"),
        ("segment_prefix_bcast", (seg_bcast, ret_ins), "unsupported instruction prefix"),
        ("segment_prefix_maskmov", (ld_ymm1, seg_maskmov, ret_ins), "unsupported instruction prefix"),
        ("missing_ret", (ld_ymm0,), "missing terminal ret"),
        ("after_ret", (ret_ins, ld_ymm0), "unreachable instruction after ret"),
        ("rep_ret", b"\xf3\xc3", "unsupported instruction prefix"),
    ]

    all_unit = [
        *((lbl, (ld_ymm0, ld_ymm1, ld_ymm2, ins, ret_ins),
           "unsupported memory operand for register-domain AVX instruction")
          for lbl, ins in reg_only_mem_cases),
        *((lbl, (ld_ymm0, ld_ymm1, ld_ymm2, ld_rax, ins, ret_ins), "unsupported instruction")
          for lbl, ins in carved_out_cases),
        *((lbl, (ld_ymm0, ld_ymm1, ld_ymm2, ins, ret_ins), "unsupported instruction")
          for lbl, ins in out_of_scope_cases),
        *other_cases,
    ]

    for label, seq, expected_reason in all_unit:
        raw = seq if isinstance(seq, bytes) else encode_seq(*seq)
        try:
            lift_code(label, 0x1000, raw)
        except ValueError as exc:
            if expected_reason not in str(exc):
                raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
        else:
            raise AssertionError(f"{label}: translator unexpectedly accepted rejected control")

    # End-to-end PE patching controls on a copy of the built native DLL.
    pe_bytes = Path(original_path).read_bytes()
    with pefile.PE(data=pe_bytes, fast_load=True) as pe:
        entry = export_rva(pe, "op_pipeline")
        file_off = pe.get_offset_from_rva(entry)
        pdata = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        records = pe.get_data(pdata.VirtualAddress, pdata.Size)
        rec_idx = next(i for i in range(0, len(records), 12)
                       if struct.unpack_from("<I", records, i)[0] == entry)
        pdata_end_off = pe.get_offset_from_rva(pdata.VirtualAddress + rec_idx + 4)
        orig_end = struct.unpack_from("<I", records, rec_idx + 4)[0]
        max_len = orig_end - entry

    pe_patches = [
        ("pe_vshufps_mem", encode_seq(ld_ymm1, reg_only["vshufps_mem"], ret_ins),
         "unsupported memory operand for register-domain AVX instruction"),
        ("pe_vperm2f128_mem", encode_seq(ld_ymm1, reg_only["vperm2f128_mem"], ret_ins),
         "unsupported memory operand for register-domain AVX instruction"),
        ("pe_carved_vmovups", encode_seq(carved_out_cases[2][1], ret_ins),
         "unsupported instruction"),
        ("pe_vcmpps", encode_seq(ld_ymm1, ld_ymm2, out_of_scope_cases[0][1], ret_ins),
         "unsupported instruction"),
        ("pe_evex_vandps", encode_seq(*other_cases[0][1]), "unsupported EVEX instruction"),
        ("pe_nonvolatile_ymm6", encode_seq(*other_cases[2][1]), "nonvolatile vector register"),
        ("pe_nonvolatile_rbx", encode_seq(*other_cases[4][1]), "nonvolatile GPR"),
        ("pe_undef_ymm3", encode_seq(*other_cases[7][1]), "undefined vector register ymm3"),
        ("pe_maskmov_indexed", encode_seq(*other_cases[17][1]), "unsupported memory operand"),
        ("pe_missing_ret", encode_seq(ld_ymm0), "missing terminal ret"),
        ("pe_after_ret", encode_seq(ret_ins, ld_ymm0), "unreachable instruction after ret"),
    ]
    with TemporaryDirectory() as folder:
        patched_path = Path(folder) / "patched.dll"
        for label, patch, expected_reason in pe_patches:
            if len(patch) > max_len:
                raise AssertionError(f"{label}: patch ({len(patch)} bytes) exceeds op_pipeline ({max_len})")
            damaged = bytearray(pe_bytes)
            damaged[file_off:file_off + len(patch)] = patch
            struct.pack_into("<I", damaged, pdata_end_off, entry + len(patch))
            patched_path.write_bytes(damaged)
            try:
                translate(patched_path, ["op_pipeline"])
            except ValueError as exc:
                if expected_reason not in str(exc):
                    raise AssertionError(f"{label}: expected {expected_reason!r}, got {exc!r}") from exc
            else:
                raise AssertionError(f"{label}: PE translate accepted rejected patch")

    print(f"{len(alt_accept)} alternate-encoding lift checks, {len(all_unit)} unit rejection controls "
          f"({len(reg_only_mem_cases)} register-domain memory, {len(carved_out_cases)} carved-out, "
          f"{len(out_of_scope_cases)} out-of-scope, {len(other_cases)} EVEX/ABI/state/addressing/prefix),"
          f" and {len(pe_patches)} PE-patched rejection controls passed")


if __name__ == "__main__":
    check_differential(*sys.argv[1:4])
    check_alt_encodings_and_rejections(sys.argv[1])
