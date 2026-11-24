"""Trace a straight-line descriptor load and its first immediate buffer read.

The diagnostic shader exposes four loaded words as depth stripes. Its export
is supplied by the probe, so it is never a complete translated archive PS.
"""
import re
from typing import NamedTuple

from pixel_aot import scalar_u32


SPAN = r"s\[(\d+):(\d+)\]"
IMM = r"(null|0x[0-9a-f]+)"
DESCRIPTOR = re.compile(rf"s_load_dwordx4 {SPAN}, {SPAN}, {IMM}")
BUF_READ = re.compile(rf"s_buffer_load_dword(x[248]|x16)? (s\d+|s\[\d+:\d+\]), {SPAN}, (null|0x[0-9a-f]+|s\d+)")
SALU_B32 = ("s_lshl_b32", "s_lshr_b32", "s_and_b32", "s_or_b32", "s_xor_b32")
SALU_OPERAND = re.compile(r"s\d+|-?[0-9]{1,10}|0x[0-9a-f]{1,8}")
EXEC_NAMES = ("exec", "exec_lo")
WAIT_LIMITS = {"vmcnt": 63, "lgkmcnt": 63, "expcnt": 7}
FLAT_SCRATCH_WRITE = re.compile(r"s_setreg_imm32_b32 hwreg\(HW_REG_FLAT_SCR_(?:LO|HI)\), (\S+)")
VECTOR_WORK = {
    "v_cvt_u32_f32_e32", "v_lshlrev_b32_e32", "v_and_b32_e32", "v_and_or_b32",
    "v_cmp_eq_u32_e32", "v_interp_p1_f32_e32", "v_interp_p2_f32_e32",
    "v_interp_p1ll_f16", "v_interp_p2_f16", "v_interp_mov_f32_e32", "v_mov_b32_e32",
}


class Descriptor(NamedTuple):
    table: int
    offset: int
    first: int
    instruction: int


class Read(NamedTuple):
    width: int
    offset: int | str

    def mode(self):
        addressing = "register" if isinstance(self.offset, str) else "immediate"
        return f"{self.width} DWORD(s), {addressing}"


def immediate(token):
    return 0 if token == "null" else int(token, 16)


def written_registers(token):
    if re.fullmatch(r"s\d+", token):
        return range(int(token[1:]), int(token[1:]) + 1)
    if match := re.fullmatch(SPAN, token):
        return range(int(match[1]), int(match[2]) + 1)
    return ()


def wait_state(line):
    """None for a line that is not an s_waitcnt, "invalid" for a wait the lowering refuses, else whether
    the wait drains every outstanding scalar load (lgkmcnt(0), or the all-zero immediate)."""
    match = re.fullmatch(r"s_waitcnt\s+(.*)", line)
    if not match:
        return None
    payload = match[1].strip()
    if payload in ("0", "0x0"):
        return True
    seen = {}
    for token in payload.split():
        counter = re.fullmatch(r"(vmcnt|lgkmcnt|expcnt)\((\d+)\)", token)
        if not counter or counter[1] in seen or int(counter[2]) > WAIT_LIMITS[counter[1]]:
            return "invalid"
        seen[counter[1]] = int(counter[2])
    return "invalid" if not seen else seen.get("lgkmcnt") == 0


def flat_scratch_value(value):
    if re.fullmatch(r"0x[0-9a-f]{1,8}", value):
        return True
    return bool(re.fullmatch(r"-?[0-9]{1,10}", value)) and -0x80000000 <= int(value) <= 0xffffffff


def operand_b32(token):
    """A 32-bit scalar ALU source: a register, or any immediate pixel_aot.scalar_u32 lowers (including floats)."""
    if re.fullmatch(r"s\d+", token):
        return True
    try:
        scalar_u32(token, {}, set())
    except ValueError:
        return False
    return True


def salu_form(line):
    """Destination and source registers of a lowered scalar ALU line, or None."""
    op, _, args = line.partition(" ")
    operands = args.split(", ")
    if op in SALU_B32 and len(operands) == 3 and re.fullmatch(r"s\d+", operands[0]) \
            and all(operand_b32(arg) for arg in operands[1:]):
        dest = written_registers(operands[0])
        if dest.stop > 106:
            return None
        sources = [reg for arg in operands[1:] for reg in written_registers(arg)]
        return dest, sources
    if op == "s_and_b64" and len(operands) == 3:
        dest = written_registers(operands[0])
        if (re.fullmatch(SPAN, operands[0]) and len(dest) == 2 and dest.start % 2 == 0 and dest.stop <= 106
                and all(re.fullmatch(SPAN, arg) or SALU_OPERAND.fullmatch(arg) for arg in operands[1:])):
            return dest, [reg for arg in operands[1:] for reg in written_registers(arg)]
    return None


def exec_and(line):
    """The lowered execution-mask restore forms: exec = exec & saved mask, nothing else."""
    op, _, args = line.partition(" ")
    operands = args.split(", ")
    if len(operands) != 3 or operands[0] not in EXEC_NAMES or operands[1] not in EXEC_NAMES:
        return False
    if op == "s_and_b32":
        return bool(re.fullmatch(r"s\d+", operands[2]))
    return op == "s_and_b64" and bool(re.fullmatch(rf"s\d+|{SPAN}", operands[2]))


def scalar_writes(line):
    """Known straight-line instructions only. None means the trace stops."""
    op, _, args = line.partition(" ")
    dest = args.split(", ", 1)[0]
    if op in VECTOR_WORK and (re.fullmatch(r"v\d+", dest) or dest == "vcc_lo"):
        return ()
    if op in ("s_inst_prefetch", "s_nop", "s_clause"):
        return ()
    if re.fullmatch(r"s_setreg_imm32_b32 hwreg\(HW_REG_MODE, 2, 2\), [03]", line):
        return ()
    if line in ("s_wqm_b64 exec, exec", "s_wqm_b64 exec_lo, exec_lo",
                "s_wqm_b32 exec_lo, exec_lo", "s_wqm_b32 exec, exec"):
        return ()
    if exec_and(line):
        return ()
    if (match := FLAT_SCRATCH_WRITE.fullmatch(line)) and flat_scratch_value(match[1]):
        return ()
    if form := salu_form(line):
        return form[0]
    if op in ("s_mov_b32", "s_mov_b64", "s_movk_i32", "s_add_i32", "s_brev_b32",
              "s_load_dwordx4", "s_load_dwordx8"):
        if dest in ("m0", "exec", "exec_lo"):
            return ()
        if op == "s_mov_b64" and re.fullmatch(r"s\d+", dest):
            n = int(dest[1:])
            return range(n, n + 2)
        if regs := written_registers(dest):
            return regs
    return None


def first_descriptor(lines):
    origins = {i: i for i in range(106)}
    zero = set()
    pending = set()
    for i, line in enumerate(lines):
        if match := DESCRIPTOR.fullmatch(line):
            dst, end, lo, hi = map(int, match.groups()[:4])
            if (lo not in origins or hi not in zero or hi != lo + 1 or lo % 2
                    or pending.intersection((lo, hi, *range(dst, end + 1)))
                    or dst % 4 or end != dst + 3 or end > 105):
                return None
            return Descriptor(origins[lo], immediate(match[5]), dst, i)
        if (wait := wait_state(line)) is not None:
            if wait == "invalid":
                return None
            if wait:
                pending.clear()
            continue
        writes = scalar_writes(line)
        if writes is None or pending.intersection(writes):
            return None
        if (form := salu_form(line)) and pending.intersection(form[1]):
            return None
        move = re.fullmatch(r"s_mov_b32 s(\d+), (s\d+|0)", line)
        source = int(move[2][1:]) if move and move[2] != "0" else None
        origin = origins.get(source)
        is_zero = bool(move and (source is None or source in zero))
        for dest in writes:
            origins.pop(dest, None)
            zero.discard(dest)
        if move:
            dest = int(move[1])
            if origin is not None:
                origins[dest] = origin
            if is_zero:
                zero.add(dest)
        if line.startswith("s_load_"):
            pending.update(writes)
    return None


def first_read(lines, descriptor):
    """Require a completed descriptor, an intact four-register span, and a completed read."""
    watched = set(range(descriptor.first, descriptor.first + 4))
    ready = False
    result = None
    for line in lines[descriptor.instruction + 1:]:
        if (wait := wait_state(line)) is not None:
            if wait == "invalid":
                return None
            if wait:
                if result is not None:
                    return result
                ready = True
            continue
        match = BUF_READ.fullmatch(line)
        if match and result is None and int(match[3]) == descriptor.first:
            width = int(match[1][1:]) if match[1] else 1
            dest = written_registers(match[2])
            offset = match[5] if match[5].startswith("s") else immediate(match[5])
            if (not ready or int(match[4]) != descriptor.first + 3 or len(dest) != width
                    or dest.start % width or dest.stop > 106 or (isinstance(offset, int) and offset % 4)):
                return None
            result = Read(width, offset)
            watched = set(dest)
            continue
        operands = line.partition(" ")[2].split(", ")
        if not ready or result is not None:
            if any(watched.intersection(written_registers(arg)) for arg in operands[1:]):
                return None
        writes = written_registers(match[2]) if match else scalar_writes(line)
        if writes is None or watched.intersection(writes):
            return None
    return None


def depth_probe(descriptor, read):
    if (descriptor.table, descriptor.offset, read) != (9, 0, Read(4, 16)):
        raise ValueError("Expected entry s9 + 0 and four buffer words at byte 16")
    return (
        "cbuffer Observed : register(b0) { uint4 words[128]; };\n"
        "float main(float4 pos : SV_Position) : SV_Depth {\n"
        f"    return asfloat(words[{read.offset // 16}][(uint)pos.x & 3u]);\n"
        "}\n"
    )
