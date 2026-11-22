"""Offline C++ lowering for the null-context FidelityFX version-query path."""
import argparse
import hashlib
from pathlib import Path

import pefile
from iced_x86 import Decoder, MemorySizeExt, Mnemonic as M, OpKind as K, Register as R, RegisterExt

from cpu_leaf_aot import export_rva
from cpu_ranges import function_ranges, image_bytes
from x87_lower import X87_OPS, statement as x87_statement


LOADER = "431bdd2e26c0c8214fd7601140eb9fe6d8a249f5000539453a9a9b6a0b7c0ee8"
UPSCALER = "faff02f294a7e9149ce079dffe3dedbcabe99b1f180ab14222aa8689e00a75fa"
REGS = {getattr(R, n.upper()): n for n in "rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15".split()}
HIGH_BYTE = {R.AH, R.BH, R.CH, R.DH}
UINT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}

INTEGER_HEADER = """#include <array>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
using std::memcpy;
using std::memset;
using word = uint64_t;
static_assert(sizeof(void*) == sizeof(word));
template<class T> T load(word address) {
    T v; memcpy(&v, reinterpret_cast<const void*>(address), sizeof v); return v;
}
template<class T> void store(word address, T v) {
    memcpy(reinterpret_cast<void*>(address), &v, sizeof v);
}
"""
HEADER = INTEGER_HEADER + "struct Query { word type, next, effect, device, count, ids, names; };\n"


def reg(number):
    return REGS[RegisterExt.full_register(number)]


def region(pe, start, end):
    ins = list(Decoder(64, image_bytes(pe, start, end-start, executable=True), ip=start))
    if not ins or ins[-1].next_ip != end or any(i.is_invalid for i in ins):
        raise ValueError("query interval does not end on an instruction boundary")
    return ins


def lower(body, initial, globals_, calls, exits, *, host_ops=None):
    """Emit only query integer operations. All external edges and calls need named bindings."""
    lines = ["word frame[16]{};", "word " + ", ".join(r + "=0" for r in REGS.values()) + ";",
             "bool zero=false, carry=false;", "rsp = reinterpret_cast<word>(frame);", *initial]
    allowed = {i.ip for i in body} | set(exits)
    xmm = {R.XMM0: "xmm0", R.XMM1: "xmm1", R.XMM2: "xmm2"}
    has_xmm_math = any(i.mnemonic in (M.VMOVDQU, M.VPSRLDQ, M.VMOVD)
                       or (i.mnemonic == M.VPADDD and i.op2_kind != K.MEMORY) for i in body)
    if has_xmm_math:
        lines.append("__m128i xmm0{}, xmm1{}, xmm2{};")
    # Unaligned YMM copies and VEX zeroing in the descriptor builder need
    # only byte storage on the desktop host.
    wide = {R.YMM0: "wide0", R.YMM1: "wide1", R.YMM2: "wide2"}
    has_wide = any(i.mnemonic == M.VMOVUPS for i in body)
    if has_wide:
        if has_xmm_math:
            raise ValueError("mixed vector arithmetic and wide descriptor copies are unsupported")
        lines.append("std::array<uint8_t,32> wide0{}, wide1{}, wide2{};")
    for index, i in enumerate(body):
        op = i.mnemonic

        def address():
            if i.is_ip_rel_memory_operand:
                if i.memory_base == R.EIP:
                    # The hardware adds the low 32 bits of the runtime RIP; an RVA binding cannot model that.
                    raise ValueError(f"unsupported EIP-relative operand at {i.ip:#x}: {i}")
                return globals_[i.ip_rel_memory_address]
            terms = []
            addr32 = False
            if i.memory_base != R.NONE:
                addr32 |= RegisterExt.size(i.memory_base) == 4
                terms.append(reg(i.memory_base))
            if i.memory_index != R.NONE:
                addr32 |= RegisterExt.size(i.memory_index) == 4
                terms.append(f"{reg(i.memory_index)} * {i.memory_index_scale}ull")
            terms.append(f"{i.memory_displacement}ull")
            expr = " + ".join(terms)
            return f"static_cast<uint32_t>({expr})" if addr32 else expr

        def operand(n):
            kind = i.op_kind(n)
            if kind == K.REGISTER:
                register = i.op_register(n)
                if register in HIGH_BYTE:
                    return f"static_cast<uint8_t>({reg(register)} >> 8)"
                return f"static_cast<{UINT[RegisterExt.size(register)]}>({reg(register)})"
            if kind == K.MEMORY:
                return f"load<{UINT[MemorySizeExt.size(i.memory_size)]}>({address()})"
            return f"{i.immediate(n)}ull"

        def write(expr):
            if i.op0_kind == K.MEMORY:
                return f"store<{UINT[MemorySizeExt.size(i.memory_size)]}>({address()}, {expr});"
            dest = i.op0_register
            if dest in HIGH_BYTE:
                return f"{reg(dest)} = ({reg(dest)} & ~0xff00ull) | (static_cast<word>(static_cast<uint8_t>({expr})) << 8);"
            size = RegisterExt.size(dest)
            value = f"static_cast<{UINT[size]}>({expr})"
            if size < 4:
                value = f"({reg(dest)} & ~{(1 << (8*size))-1}ull) | {value}"
            return f"{reg(dest)} = {value};"

        if host_ops and i.ip in host_ops:
            stmt = host_ops[i.ip]
        elif i.segment_prefix in (R.FS, R.GS) or i.has_lock_prefix:
            # FS/GS select a thread block base and LOCK makes the access atomic;
            # the flat loads and stores below would silently drop both.
            raise ValueError(f"unsupported instruction prefix at {i.ip:#x}: {i}")
        elif op in (M.JE, M.JNE, M.JAE, M.JA, M.JB, M.JBE, M.JMP):
            target = i.near_branch_target
            if target not in allowed:
                raise ValueError(f"unbound branch at {i.ip:#x}")
            cond = {M.JE: "if (zero) ", M.JNE: "if (!zero) ", M.JAE: "if (!carry) ",
                    M.JA: "if (!carry && !zero) ", M.JB: "if (carry) ", M.JBE: "if (carry || zero) ", M.JMP: ""}[op]
            stmt = f"{cond}goto block_{target:x};"
        elif op in (M.MOV, M.MOVZX):
            stmt = write(operand(1))
        elif op == M.MOVSXD:
            stmt = write(f"int64_t(int32_t({operand(1)}))")
        elif op == M.CMOVNE:
            stmt = write(f"zero ? {operand(0)} : {operand(1)}")
        elif op == M.CMOVE:
            stmt = write(f"zero ? {operand(1)} : {operand(0)}")
        elif op == M.CMOVBE:
            stmt = write(f"(carry || zero) ? {operand(1)} : {operand(0)}")
        elif op == M.LEA:
            stmt = write(address())
        elif op == M.CMP:
            # Sign-extended immediates arrive as 64-bit values; compare at the destination width.
            width = UINT[MemorySizeExt.size(i.memory_size) if i.op0_kind == K.MEMORY else RegisterExt.size(i.op0_register)]
            stmt = (f"{{ const {width} a = {operand(0)}, b = static_cast<{width}>({operand(1)}); "
                    f"zero = a == b; carry = a < b; }}")
        elif op == M.TEST:
            stmt = f"zero = (({operand(0)} & {operand(1)}) == 0); carry = false;"
        elif op == M.IMUL and i.op_count in (2, 3) and RegisterExt.size(i.op0_register) == 4:
            lhs, rhs = operand(i.op_count - 2), operand(i.op_count - 1)
            stmt = (f"{{ const int64_t product = int64_t(int32_t({lhs})) * int32_t({rhs}); "
                    f"carry = product != int32_t(product); {write('product')} }}")
        elif op == M.CDQ:
            stmt = "rdx = uint32_t(int32_t(rax) >> 31);"
        elif op in (M.SHL, M.SHR, M.SAR) and i.op1_kind == K.IMMEDIATE8 and RegisterExt.size(i.op0_register) in (4, 8):
            size = RegisterExt.size(i.op0_register)
            bits = 8 * size
            count = i.immediate(1) & (bits - 1)
            shift, last = ("<<", bits-count) if op == M.SHL else (">>", count-1)
            lhs = f"int{bits}_t(a)" if op == M.SAR else "a"
            stmt = (f"{{ const {UINT[size]} a = {operand(0)}, v = {lhs} {shift} {count}; "
                    f"carry = ((a >> {last}) & 1) != 0; zero = v == 0; {write('v')} }}") if count else \
                write(operand(0))  # A masked zero count keeps the flags but still zero-extends a 32-bit destination.
        elif op in (M.ADD, M.AND, M.OR, M.XOR, M.INC, M.DEC, M.SUB, M.SBB):
            operator = {M.ADD: "+", M.INC: "+", M.DEC: "-", M.AND: "&", M.OR: "|", M.XOR: "^", M.SUB: "-", M.SBB: "-"}[op]
            rhs = "1" if op in (M.INC, M.DEC) else operand(1)
            width = UINT[MemorySizeExt.size(i.memory_size) if i.op0_kind == K.MEMORY else RegisterExt.size(i.op0_register)]
            expr = f"a {operator} b" + (" - word(carry)" if op == M.SBB else "")
            update = {M.ADD: "carry = v < a;", M.SUB: "carry = a < b;", M.SBB: "carry = a < b || (carry && a == b);",
                      M.INC: "", M.DEC: "", M.XOR: "carry = false;", M.AND: "carry = false;", M.OR: "carry = false;"}[op]
            stmt = (f"{{ const {width} a = {operand(0)}, b = static_cast<{width}>({rhs}); "
                    f"const {width} v = static_cast<{width}>({expr}); {update} zero = v == 0; {write('v')} }}")
        elif op in (M.ANDN, M.BEXTR, M.BLSI, M.BLSMSK, M.BLSR, M.TZCNT) and RegisterExt.size(i.op0_register) in (4, 8):
            size = RegisterExt.size(i.op0_register)
            width, bits = UINT[size], 8 * size
            if op == M.ANDN:
                stmt = (f"{{ const {width} v = static_cast<{width}>(~static_cast<{width}>({operand(1)}) "
                        f"& static_cast<{width}>({operand(2)})); carry = false; zero = v == 0; {write('v')} }}")
            elif op == M.BEXTR:
                stmt = (f"{{ const {width} a = {operand(1)}, ctrl = {operand(2)}; "
                        f"const unsigned start = static_cast<unsigned>(ctrl & 0xffu), "
                        f"len = static_cast<unsigned>((ctrl >> 8) & 0xffu); "
                        f"{width} v = 0; if (start < {bits}u && len > 0u) {{ "
                        f"const {width} s = a >> start; "
                        f"v = len >= {bits}u ? s : static_cast<{width}>(s & ((static_cast<{width}>(1) << len) - 1)); }} "
                        f"carry = false; zero = v == 0; {write('v')} }}")
            elif op == M.BLSI:
                stmt = (f"{{ const {width} a = {operand(1)}, v = static_cast<{width}>(a & (static_cast<{width}>(0) - a)); "
                        f"carry = a != 0; zero = v == 0; {write('v')} }}")
            elif op == M.BLSMSK:
                stmt = (f"{{ const {width} a = {operand(1)}, v = static_cast<{width}>(a ^ (a - 1)); "
                        f"carry = a == 0; zero = false; {write('v')} }}")
            elif op == M.BLSR:
                stmt = (f"{{ const {width} a = {operand(1)}, v = static_cast<{width}>(a & (a - 1)); "
                        f"carry = a == 0; zero = v == 0; {write('v')} }}")
            else:  # M.TZCNT
                stmt = (f"{{ const {width} a = {operand(1)}; {width} v = 0; "
                        f"if (a == 0) {{ v = {bits}; }} else {{ while (((a >> v) & 1) == 0) ++v; }} "
                        f"carry = a == 0; zero = v == 0; {write('v')} }}")
        elif op in (M.VMOVQ, M.VMOVSD, M.VMOVHPD) and has_wide:
            lanes = {R.XMM0: "wide0", R.XMM1: "wide1", R.XMM2: "wide2"}
            if i.op1_kind == K.MEMORY and op != M.VMOVHPD:
                dst = lanes[i.op0_register]
                stmt = f"{dst} = {{}}; memcpy({dst}.data(), reinterpret_cast<const void*>({address()}), 8);"
            elif i.op0_kind == K.MEMORY:
                start = 8 if op == M.VMOVHPD else 0
                stmt = f"memcpy(reinterpret_cast<void*>({address()}), {lanes[i.op1_register]}.data()+{start}, 8);"
            else:
                raise ValueError(f"unsupported vector move at {i.ip:#x}")
        elif has_wide and op in (M.VPADDD, M.VPSRLD):
            lanes = {R.XMM0: "wide0", R.XMM1: "wide1", R.XMM2: "wide2"}
            src, dst = lanes[i.op1_register], lanes[i.op0_register]
            operation = (f"+ load<uint32_t>(({address()}) + n)" if op == M.VPADDD
                         else f">> {i.immediate(2)}")
            if op == M.VPSRLD and i.immediate(2) >= 32:
                raise ValueError("unsupported packed shift count")
            stmt = (f"{{ auto input = {src}; {dst} = {{}}; for (size_t n=0; n<16; n+=4) {{ "
                    f"auto v = load<uint32_t>(reinterpret_cast<word>(input.data()+n)) {operation}; "
                    f"memcpy({dst}.data()+n, &v, 4); }} }}")
        elif op == M.VMOVUPS and i.op1_kind == K.MEMORY:
            stmt = f"{wide[i.op0_register]} = load<std::array<uint8_t,32>>({address()});"
        elif op == M.VMOVUPS and i.op0_kind == K.MEMORY:
            if i.op1_register in wide:
                stmt = f"store({address()}, {wide[i.op1_register]});"
            else:
                source = {R.XMM0: "wide0", R.XMM1: "wide1", R.XMM2: "wide2"}[i.op1_register]
                stmt = f"memcpy(reinterpret_cast<void*>({address()}), {source}.data(), 16);"
        elif op == M.VPXOR and i.op0_register == i.op1_register == i.op2_register and has_wide:
            stmt = {R.XMM0: "wide0", R.XMM1: "wide1", R.XMM2: "wide2"}[i.op0_register] + " = {};"
        elif op == M.VZEROUPPER:
            stmt = " ".join(f"memset({v}.data()+16, 0, 16);" for v in wide.values()) if has_wide else ";"
        elif op == M.VMOVDQU and i.op1_kind == K.MEMORY:
            stmt = f"{xmm[i.op0_register]} = load<__m128i>({address()});"
        elif op == M.VPSRLDQ:
            stmt = f"{xmm[i.op0_register]} = _mm_srli_si128({xmm[i.op1_register]}, {i.immediate(2)});"
        elif op == M.VPADDD:
            stmt = f"{xmm[i.op0_register]} = _mm_add_epi32({xmm[i.op1_register]}, {xmm[i.op2_register]});"
        elif op == M.VMOVD and i.op1_register in xmm:
            stmt = write(f"_mm_cvtsi128_si32({xmm[i.op1_register]})")
        elif op == M.NOP or (op == M.XCHG and i.op0_register == i.op1_register == R.AX):
            stmt = ";"
        elif op == M.SETE:
            stmt = write("zero")
        elif op == M.CALL:
            stmt = calls[i.ip]
        elif op == M.RET:
            stmt = "return rax;"
        elif op in X87_OPS:
            stmt = x87_statement(i, address)
        else:
            raise ValueError(f"unsupported query operation at {i.ip:#x}: {i}")
        lines.append(f"block_{i.ip:x}: {stmt}")
        if (op not in (M.RET, M.JMP) and
                (index + 1 == len(body) or body[index + 1].ip != i.next_ip)):
            if i.next_ip not in allowed:
                raise ValueError(f"unbound fallthrough at {i.ip:#x}")
            lines.append(f"goto block_{i.next_ip:x};")
    if any(i.mnemonic in X87_OPS for i in body):
        lines.insert(3, "X87 fpu;")  # x87_lower.X87_HEADER must precede the generated function
    lines.extend(f"block_{target:x}: {statement}" for target, statement in exits.items())
    return "\n".join("    " + line for line in lines)


def leaf(pe, address):
    for i in Decoder(64, image_bytes(pe, address, 32, executable=True), ip=address):
        if i.mnemonic == M.RET:
            return region(pe, address, i.next_ip)
    raise ValueError("unsupported provider predicate")


def checked_image(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"unrecognized {path.name} version-query profile")
    return pefile.PE(data=data, fast_load=True)


def export_body(pe, name, slices):
    owned = function_ranges(pe, export_rva(pe, name))
    if not all(any(lo <= a < b <= hi for lo, hi in owned) for a, b in slices):
        raise ValueError(f"slice outside exported function {name}")
    return [ins for a, b in slices for ins in region(pe, a, b)]


def generate(loader, upscaler):
    # These two build-specific profiles specialize the entry guards to (nullptr, type 4).
    # Recover the record from initializer stores; the on-disk registry is zero-filled.
    with checked_image(upscaler, UPSCALER) as pe:
        init = {i.ip: i for i in region(pe, 0x10bb, 0x1144)}
        record = init[0x1130].ip_rel_memory_address
        fields = {init[addr].ip_rel_memory_address-record: value for addr, value in [
            (0x10d0, init[0x10bb].immediate(1)), (0x10e5, init[0x10e5].immediate(1)),
            (0x10f0, init[0x10de].ip_rel_memory_address)]}
        name = pe.get_string_at_rva(fields[32], 128)
        if not name or not all(32 <= c < 127 for c in name):
            raise ValueError("invalid provider version name")
        vtable = init[0x1105].ip_rel_memory_address
        predicates = []
        for slot, symbol in ((1, "supports_effect"), (2, "supports_device")):
            addr = pe.get_qword_at_rva(vtable + 8*slot) - pe.OPTIONAL_HEADER.ImageBase
            code = lower(leaf(pe, addr), ["rcx = self; rdx = input;"], {}, {}, {})
            predicates.append(f"static word {symbol}(word self, word input) {{\n{code}\n}}")
        globals_ = {init[0x1137].ip_rel_memory_address: "reinterpret_cast<word>(registry)",
                    record: "reinterpret_cast<word>(registry + 1)"}
        # The actual registry occupies one pointer between the decoded start and end.
        if record - init[0x1137].ip_rel_memory_address != 8:
            raise ValueError("unexpected provider registry size")
        calls = {site: f"rax = {fn}(rcx, rdx);" for site, fn in [
            (0x1595, "supports_effect"), (0x15a5, "supports_device"),
            (0x161a, "supports_effect"), (0x162a, "supports_device")]}
        provider = lower(export_body(pe, "ffxQuery", [(0x152b, 0x164a)]),
                         ["r15 = reinterpret_cast<word>(query); rdi = 0;"], globals_, calls,
                         {0x1999: "return static_cast<uint32_t>(rax);", 0x164a: "return 6;"})
    with checked_image(loader, LOADER) as pe:
        # The desktop binding supplies the translated upscaler; frame generation is absent.
        slots = {0x4088: "reinterpret_cast<word>(&present)", 0x40c0: "reinterpret_cast<word>(&absent)",
                 0x40a8: "reinterpret_cast<word>(&present)", 0x40e0: "reinterpret_cast<word>(&absent)"}
        caller = lower(export_body(pe, "ffxQuery", [(0x144b, 0x14b2), (0x14c2, 0x14d5)]),
                       ["rbx = reinterpret_cast<word>(query); rdi = reinterpret_cast<word>(&query->effect);"],
                       slots, {0x1456: "/* selected modules are bound offline */",
                               0x14ac: "rax = provider_versions(reinterpret_cast<Query*>(rdx));"},
                       {0x14b2: "return static_cast<uint32_t>(rax);", 0x150f: "return static_cast<uint32_t>(rax);",
                        0x14e5: "return 6;", 0x14d5: "return static_cast<uint32_t>(rax);"})
    chars = ", ".join(str(b) for b in name + b"\0")
    return HEADER + "\n".join(predicates) + f"""
static const char version_name[] = {{{chars}}};
static const word present = 1, absent = 0;
static const word entry[] = {{0, 0, {fields[16]}ull, {fields[24]}ull, reinterpret_cast<word>(version_name)}};
static const word registry[] = {{reinterpret_cast<word>(entry)}};
static uint32_t provider_versions(Query* query) {{
{provider}
}}
extern "C" __declspec(dllexport) uint32_t query_versions(Query* query) {{
    if (!query || query->type != 4) return 6;
{caller}
}}
"""


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("loader", "upscaler", "output"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    if args.output.suffix.lower() != ".cpp" or args.output.resolve() in (args.loader.resolve(), args.upscaler.resolve()):
        parser.error("choose a distinct .cpp output")
    args.output.write_text(generate(args.loader, args.upscaler), encoding="ascii")
