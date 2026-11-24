"""Lift bounded Windows x64 leaf functions with SSE4a instructions to C++.

Each export uses the leaf ABI void fn(word rcx, word rdx, word r8), where rcx,
rdx and r8 carry host pointers to caller-owned buffers (16-byte-aligned when
16-byte-aligned memory operands are used; rcx and rdx are inputs, r8 is the
output). XMM0..XMM5 are modeled as portable 128-bit value structs (Vec128, two
64-bit lanes). Only legacy encodings are accepted; VEX, EVEX and other
non-legacy forms reject.

Supported SSE4a instructions (docs/coverage/x64_zen2.tsv group SSE3-4):
  - extrq (immediate form, 66 0F 78 /0 ib ib): extract bit field from xmm[63:0],
    zero-extending into xmm[63:0]; rejects static offset + width > 64
  - extrq (register form, 66 0F 79 /r): extract bit field from xmm1[63:0] using
    length=xmm2[5:0] (0 -> 64) and index=xmm2[13:8]
  - insertq (immediate form, F2 0F 78 /r ib ib): insert bits [width-1:0] of
    xmm2[63:0] into xmm1[63:0] at offset; rejects static offset + width > 64
  - insertq (register form, F2 0F 79 /r): insert bits [width-1:0] of xmm2[63:0]
    into xmm1[63:0] using length=xmm2[69:64] (0 -> 64) and index=xmm2[77:72]
  - movntsd (F2 0F 2B /r): unaligned non-temporal 64-bit store to [r8 + disp]
  - movntss (F3 0F 2B /r): unaligned non-temporal 32-bit store to [r8 + disp]

Bits 127:64 of an extrq/insertq destination are architecturally undefined per
the AMD64 APM; this lifter leaves dst.q[1] unchanged and tests only observe the
defined low 64 bits.

Transport/ABI glue: movaps, movdqa (16-byte aligned 128-bit load/store/reg
copy), movdqu (unaligned 128-bit load/store/reg copy) and terminal ret. All
memory stores must target an r8-rooted flat address; loads may read from rcx,
rdx or r8.
"""

import argparse
from pathlib import Path

import pefile
from iced_x86 import Code, Decoder, EncodingKind, OpKind, Register as R, RegisterExt

from cpu_leaf_aot import bounded_code, export_rva


ARGUMENTS = {R.RCX: "rcx", R.RDX: "rdx", R.R8: "r8"}
VOLATILE_XMM = {getattr(R, f"XMM{i}"): i for i in range(6)}
NONVOLATILE_VEC = (
    {getattr(R, f"XMM{i}") for i in range(6, 16)}
    | {getattr(R, f"YMM{i}") for i in range(6, 16)}
    | {getattr(R, f"ZMM{i}") for i in range(6, 32)}
)
NONVOLATILE_GPR = {
    R.EBX, R.RBX, R.EBP, R.RBP, R.ESI, R.RSI, R.EDI, R.RDI, R.ESP, R.RSP,
    R.R12D, R.R12, R.R13D, R.R13, R.R14D, R.R14, R.R15D, R.R15,
}

LOAD_ALIGNED_VEC_CODES = {Code.MOVAPS_XMM_XMMM128, Code.MOVDQA_XMM_XMMM128}
STORE_ALIGNED_VEC_CODES = {Code.MOVAPS_XMMM128_XMM, Code.MOVDQA_XMMM128_XMM}

ALLOWED_CODES = {
    Code.RETNQ,
    Code.EXTRQ_XMM_IMM8_IMM8,
    Code.EXTRQ_XMM_XMM,
    Code.INSERTQ_XMM_XMM_IMM8_IMM8,
    Code.INSERTQ_XMM_XMM,
    Code.MOVNTSD_M64_XMM,
    Code.MOVNTSS_M32_XMM,
    Code.MOVAPS_XMM_XMMM128,
    Code.MOVAPS_XMMM128_XMM,
    Code.MOVDQA_XMM_XMMM128,
    Code.MOVDQA_XMMM128_XMM,
    Code.MOVDQU_XMM_XMMM128,
    Code.MOVDQU_XMMM128_XMM,
}

PRELUDE = """#include <atomic>
#include <cstdint>
#include <intrin.h>

extern "C" __declspec(dllexport) std::uint32_t host_has_sse4a() {
    int info[4]{};
    __cpuid(info, 0x80000000);
    if (static_cast<unsigned>(info[0]) < 0x80000001u) return 0u;
    __cpuid(info, 0x80000001);
    return (static_cast<unsigned>(info[2]) & (1u << 6)) != 0u ? 1u : 0u;
}

using word = std::uint64_t;

struct Vec128 {
    std::uint64_t q[2];
};

// Guest reads can fault even when the translated result is dead. Keep surrounding guest
// writes on their original side of the read when the compiler inlines these helpers.
static inline Vec128 load_always(word address) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto* src = reinterpret_cast<const volatile std::uint64_t __unaligned*>(address);
    const std::uint64_t q0 = src[0];
    const std::uint64_t q1 = src[1];
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return {{q0, q1}};
}

static inline void store32(word address, std::uint32_t val) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    *reinterpret_cast<volatile std::uint32_t __unaligned*>(address) = val;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

static inline void store64(word address, std::uint64_t val) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    *reinterpret_cast<volatile std::uint64_t __unaligned*>(address) = val;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

static inline void store128(word address, const Vec128& v) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    auto* dst = reinterpret_cast<volatile std::uint64_t __unaligned*>(address);
    dst[0] = v.q[0];
    dst[1] = v.q[1];
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

static inline std::uint64_t sse4a_extract(std::uint64_t dst, unsigned width, unsigned offset) {
    if (offset + width > 64u) {
        return 0ull;
    }
    const std::uint64_t mask = (width == 64u) ? ~0ull : ((1ull << width) - 1ull);
    return (dst >> offset) & mask;
}

static inline std::uint64_t sse4a_insert(
    std::uint64_t dst, std::uint64_t src, unsigned width, unsigned offset) {
    if (offset + width > 64u) {
        return 0ull;
    }
    const std::uint64_t mask = (width == 64u) ? ~0ull : ((1ull << width) - 1ull);
    const std::uint64_t field_mask = mask << offset;
    return (dst & ~field_mask) | ((src & mask) << offset);
}

static inline std::uint64_t op_extrq_imm(
    std::uint64_t dst, std::uint8_t len_imm, std::uint8_t idx_imm) {
    const unsigned raw_len = static_cast<unsigned>(len_imm & 0x3fu);
    const unsigned width = raw_len == 0u ? 64u : raw_len;
    const unsigned offset = static_cast<unsigned>(idx_imm & 0x3fu);
    return sse4a_extract(dst, width, offset);
}

static inline std::uint64_t op_extrq_reg(std::uint64_t dst, std::uint64_t ctrl) {
    const unsigned raw_len = static_cast<unsigned>(ctrl & 0x3full);
    const unsigned width = raw_len == 0u ? 64u : raw_len;
    const unsigned offset = static_cast<unsigned>((ctrl >> 8) & 0x3full);
    return sse4a_extract(dst, width, offset);
}

static inline std::uint64_t op_insertq_imm(
    std::uint64_t dst, std::uint64_t src, std::uint8_t len_imm, std::uint8_t idx_imm) {
    const unsigned raw_len = static_cast<unsigned>(len_imm & 0x3fu);
    const unsigned width = raw_len == 0u ? 64u : raw_len;
    const unsigned offset = static_cast<unsigned>(idx_imm & 0x3fu);
    return sse4a_insert(dst, src, width, offset);
}

static inline std::uint64_t op_insertq_reg(std::uint64_t dst, const Vec128& src) {
    const std::uint64_t ctrl = src.q[1];
    const unsigned raw_len = static_cast<unsigned>(ctrl & 0x3full);
    const unsigned width = raw_len == 0u ? 64u : raw_len;
    const unsigned offset = static_cast<unsigned>((ctrl >> 8) & 0x3full);
    return sse4a_insert(dst, src.q[0], width, offset);
}
"""


def vec_index(register, ins):
    """Map XMM0..XMM5 to an index, rejecting nonvolatile and unsupported registers."""
    if register in VOLATILE_XMM:
        return VOLATILE_XMM[register]
    if register in NONVOLATILE_VEC:
        raise ValueError(f"nonvolatile vector register {register} at RVA {ins.ip:#x}: {ins}")
    raise ValueError(f"unsupported vector register {register} at RVA {ins.ip:#x}: {ins}")


def flat_address(ins, *, alignment=1, write=False):
    """Lower a flat [base + disp] pointer operand rooted in an argument GPR64."""
    base = ins.memory_base
    if (ins.is_ip_rel_memory_operand or base in (R.NONE, R.RIP, R.EIP, R.RSP, R.ESP)
            or ins.memory_index != R.NONE or not RegisterExt.is_gpr64(base)):
        raise ValueError(f"unsupported memory operand at RVA {ins.ip:#x}: {ins}")
    if base in NONVOLATILE_GPR:
        raise ValueError(f"nonvolatile GPR base {base} in memory operand at RVA {ins.ip:#x}: {ins}")
    if base not in ARGUMENTS:
        raise ValueError(f"unsupported memory base {base} at RVA {ins.ip:#x}: {ins}")
    if write and base != R.R8:
        raise ValueError(f"non-r8 store memory operand at RVA {ins.ip:#x}: {ins}")
    disp = ins.memory_displacement & ((1 << 64) - 1)
    if disp % alignment:
        raise ValueError(f"unaligned {alignment}-byte memory operand at RVA {ins.ip:#x}: {ins}")
    name = ARGUMENTS[base]
    return f"({name} + {disp:#x}ull)" if disp else name


def validate_sse4a_imm(len_imm, idx_imm, ins):
    """Validate immediate SSE4a bitfield bounds (offset + width <= 64)."""
    width = (len_imm & 0x3F) or 64
    offset = idx_imm & 0x3F
    if offset + width > 64:
        raise ValueError(
            f"undefined SSE4a bitfield (width {width} + offset {offset} > 64) "
            f"at RVA {ins.ip:#x}: {ins}"
        )


def lift_code(name, start, raw):
    """Decode and lift one bounded straight-line legacy SSE4a leaf function."""
    instructions = list(Decoder(64, raw, ip=start))
    if not instructions or instructions[-1].next_ip != start + len(raw):
        raise ValueError(f"truncated instruction stream in {name} at RVA {start:#x}")
    if any(ins.is_invalid for ins in instructions):
        bad = next(ins for ins in instructions if ins.is_invalid)
        raise ValueError(f"invalid instruction at RVA {bad.ip:#x}")

    used = set()
    defined = set()
    statements = []
    returned = False

    def read_vec(op_index, ins):
        if ins.op_kind(op_index) != OpKind.REGISTER:
            raise ValueError(f"expected register operand {op_index} at RVA {ins.ip:#x}: {ins}")
        idx = vec_index(ins.op_register(op_index), ins)
        if idx not in defined:
            raise ValueError(f"undefined vector register xmm{idx} at RVA {ins.ip:#x}: {ins}")
        used.add(idx)
        return f"xmm{idx}"

    def write_vec(op_index, ins):
        if ins.op_kind(op_index) != OpKind.REGISTER:
            raise ValueError(f"expected register destination at RVA {ins.ip:#x}: {ins}")
        idx = vec_index(ins.op_register(op_index), ins)
        used.add(idx)
        return idx, f"xmm{idx}"

    def vec_src128(op_index, ins, *, alignment):
        if ins.op_kind(op_index) == OpKind.MEMORY:
            return f"load_always({flat_address(ins, alignment=alignment)})"
        return read_vec(op_index, ins)

    for ins in instructions:
        if returned:
            raise ValueError(f"unreachable instruction after ret at RVA {ins.ip:#x}: {ins}")
        if (ins.has_lock_prefix or ins.has_rep_prefix or ins.has_repne_prefix
                or ins.has_segment_prefix or ins.has_xacquire_prefix or ins.has_xrelease_prefix):
            raise ValueError(f"unsupported instruction prefix at RVA {ins.ip:#x}: {ins}")
        if ins.op_code().encoding != EncodingKind.LEGACY:
            raise ValueError(f"non-legacy encoding at RVA {ins.ip:#x}: {ins}")
        if ins.code not in ALLOWED_CODES:
            raise ValueError(f"unsupported instruction at RVA {ins.ip:#x}: {ins}")

        if ins.code == Code.RETNQ:
            statements.append("return;")
            returned = True
        elif ins.code == Code.EXTRQ_XMM_IMM8_IMM8:
            len_imm = ins.immediate8 & 0xFF
            idx_imm = ins.immediate8_2nd & 0xFF
            validate_sse4a_imm(len_imm, idx_imm, ins)
            dst = read_vec(0, ins)
            dst_idx, dst_name = write_vec(0, ins)
            statements.append(
                f"{dst_name}.q[0] = op_extrq_imm({dst}.q[0], {len_imm:#x}u, {idx_imm:#x}u);"
            )
            defined.add(dst_idx)
        elif ins.code == Code.EXTRQ_XMM_XMM:
            dst = read_vec(0, ins)
            src = read_vec(1, ins)
            dst_idx, dst_name = write_vec(0, ins)
            statements.append(f"{dst_name}.q[0] = op_extrq_reg({dst}.q[0], {src}.q[0]);")
            defined.add(dst_idx)
        elif ins.code == Code.INSERTQ_XMM_XMM_IMM8_IMM8:
            len_imm = ins.immediate8 & 0xFF
            idx_imm = ins.immediate8_2nd & 0xFF
            validate_sse4a_imm(len_imm, idx_imm, ins)
            dst = read_vec(0, ins)
            src = read_vec(1, ins)
            dst_idx, dst_name = write_vec(0, ins)
            statements.append(
                f"{dst_name}.q[0] = op_insertq_imm({dst}.q[0], {src}.q[0], {len_imm:#x}u, {idx_imm:#x}u);"
            )
            defined.add(dst_idx)
        elif ins.code == Code.INSERTQ_XMM_XMM:
            dst = read_vec(0, ins)
            src = read_vec(1, ins)
            dst_idx, dst_name = write_vec(0, ins)
            statements.append(f"{dst_name}.q[0] = op_insertq_reg({dst}.q[0], {src});")
            defined.add(dst_idx)
        elif ins.code == Code.MOVNTSD_M64_XMM:
            if ins.op0_kind != OpKind.MEMORY:
                raise ValueError(f"expected memory destination at RVA {ins.ip:#x}: {ins}")
            src = read_vec(1, ins)
            statements.append(f"store64({flat_address(ins, write=True)}, {src}.q[0]);")
        elif ins.code == Code.MOVNTSS_M32_XMM:
            if ins.op0_kind != OpKind.MEMORY:
                raise ValueError(f"expected memory destination at RVA {ins.ip:#x}: {ins}")
            src = read_vec(1, ins)
            statements.append(
                f"store32({flat_address(ins, write=True)}, static_cast<std::uint32_t>({src}.q[0]));"
            )
        elif ins.code in (*LOAD_ALIGNED_VEC_CODES, Code.MOVDQU_XMM_XMMM128):
            alignment = 16 if ins.code in LOAD_ALIGNED_VEC_CODES else 1
            src = vec_src128(1, ins, alignment=alignment)
            dst_idx, dst_name = write_vec(0, ins)
            statements.append(f"{dst_name} = {src};")
            defined.add(dst_idx)
        elif ins.code in (*STORE_ALIGNED_VEC_CODES, Code.MOVDQU_XMMM128_XMM):
            alignment = 16 if ins.code in STORE_ALIGNED_VEC_CODES else 1
            src = read_vec(1, ins)
            if ins.op0_kind == OpKind.MEMORY:
                statements.append(
                    f"store128({flat_address(ins, alignment=alignment, write=True)}, {src});"
                )
            else:
                dst_idx, dst_name = write_vec(0, ins)
                statements.append(f"{dst_name} = {src};")
                defined.add(dst_idx)
        else:
            raise ValueError(f"unsupported instruction at RVA {ins.ip:#x}: {ins}")

    if not returned:
        raise ValueError(f"missing terminal ret in {name} at RVA {start + len(raw):#x}")

    decls = [f"    Vec128 xmm{idx}{{}};" for idx in sorted(used)]
    body = [f"    {stmt}" for stmt in statements]
    lines = [
        f'extern "C" __declspec(dllexport) void lifted_{name}(word rcx, word rdx, word r8) {{',
        "    (void)rcx; (void)rdx; (void)r8;",
        *decls,
        *body,
        "}",
    ]
    return "\n".join(lines), len(instructions)


def lift(pe, name):
    entry = export_rva(pe, name)
    raw = bounded_code(pe, entry)
    source, count = lift_code(name, entry, raw)
    return source, entry, count


def translate_pe(pe, names):
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20B:
        raise ValueError("expected AMD64 PE32+")
    lifted = [lift(pe, name) for name in names]
    source = PRELUDE + "\n".join(f"\n{text}" for text, _, _ in lifted) + "\n"
    meta = [(name, entry, count) for name, (_, entry, count) in zip(names, lifted)]
    return source, meta


def translate(image, names):
    with pefile.PE(str(image), fast_load=True) as pe:
        return translate_pe(pe, names)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=Path)
    parser.add_argument("exports", help="comma-separated export names")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        source, meta = translate(args.image, args.exports.split(","))
        args.output.write_text(source, encoding="utf-8")
        for name, entry, count in meta:
            print(f"{name} at RVA {entry:#x}: {count} instructions")
    except (ValueError, OSError, pefile.PEFormatError) as error:
        parser.exit(1, f"{error}\n")
