"""gfx1013 NGG mesh-stage machine code -> HLSL mesh shader (SM 6.5) for a mesh + pixel pipeline (startup tools).

The console compiles a mesh shader to an NGG primitive shader (wave64, 128 threads, one thread per output vertex and
one per output primitive). This module translates that program with the pipeline translator's one-lane model
(gfx2hlsl.Stage: registers are uint variables, exec/vcc/masks hold the lane's bit) and wraps it in an HLSL mesh
shader:

  * one HLSL thread per console lane: SV_GroupIndex = the console's thread index in the group
    (wave id from merged_wave_info bits 27:24 times 64, plus v_mbcnt lane);
  * s_sendmsg 0x9 (GS_ALLOC_REQ) records m0 = vertices | primitives << 12 (Mesa ac_nir_prerast_utils.c
    ac_nir_ngg_alloc_vertices_and_primitives) in groupshared memory; after the program a barrier publishes it and
    SetMeshOutputCounts runs once, in uniform control flow;
  * `exp pos0` / `exp paramN` from thread t define output vertex t, `exp prim` (target 20) primitive t, with the
    gfx10 primitive export word: index i in bits [10*i+8 : 10*i], bit 31 = null primitive (Mesa ac_nir_lower_ngg.c
    "nir_ubfe_imm(..., 10 * v, 9)" and the "1u << 31" null export). A null or out-of-range primitive is emitted
    with SV_CullPrimitive;
  * the workgroup index: the console reads v5 (first ES input VGPR, the vertex id in a fast-launch NGG
    dispatch) of its first lane and shifts it right by 7. This translation sets v5 = SV_GroupID.x * 128 +
    SV_GroupIndex (INFERRED: consistent with every one of the 9282 archive shaders reading v5 >> 7 in each wave
    without LDS; a register dump of a mesh pipeline's GE/SPI state would settle it).

Cross-lane facts the one-lane model cannot see are checked and refused (Unsupported), never guessed:
  * v_readfirstlane_b32 needs a source that is the same in every lane (taint analysis over the emitted HLSL);
  * a per-lane branch (execz/execnz/vccz/vccnz) is taken per lane; that equals the wave-level branch only when
    no scalar register written in the skipped range is read after it, and, for vcc, when vcc is uniform over the
    active lanes (`exec & uniform`);
  * a branch on an SCC that depends on lane masks is refused (the hardware reads the whole mask).
LDS, DPP, permlane, readlane/writelane are not lowered (refused by the base translator).
"""
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from gfx2hlsl import Unsupported, reg_range, parse  # noqa: E402

GROUP = 128           # PSV0 numthreads (128, 1, 1) in all 9282 archive mesh records
MAX_VERTICES = 128    # PSV0 MaxOutputVertices
MAX_PRIMITIVES = 128  # PSV0 MaxOutputPrimitives
ALL_LANES = (1 << 64) - 1
FUSED = ('v_fma_f32', 'v_fmac_f32', 'v_fmamk_f32', 'v_fmaak_f32')
# X2W_MESH_EXACT_FMA=1: fused multiply-adds through binary64 (needs double-precision shader ops); default off,
# as the pipeline translator (`a * b + c`, which the compiler may or may not fuse)
EXACT_FMA = os.environ.get('X2W_MESH_EXACT_FMA') == '1'
LDS_DWORDS = 1024     # groupshared x2w_lds (4 KB of the 28 KB a D3D12 mesh shader may use); addresses are clamped
TOKEN = re.compile(r'\b(v\d+|s\d+|exec|vcc|m0|scc|a|w|t|q|tid|lane)\b')
SIMPLE = re.compile(r'^\s*(?:if \(exec & 1\) )?(v\d+|s\d+|exec|vcc|m0|scc|a|w|t|q)\s*=\s*(.*)$')

MESH_HELPERS = {
    # v_perm_b32 D, S0, S1, S2: byte i of D = selector byte i of S2 over the eight bytes {S0 (bytes 4-7), S1 (0-3)};
    # 8..11 replicate the sign bit of S1[15], S1[31], S0[15], S0[31]; 12 gives 0x00, 13..15 give 0xff
    # (RDNA 2 ISA, V_PERM_B32).
    'x2w_perm': ('uint x2w_perm_byte(uint s0, uint s1, uint sel) { if (sel < 4u) return (s1 >> (8u * sel)) & 255u; '
                 'if (sel < 8u) return (s0 >> (8u * (sel - 4u))) & 255u; if (sel == 8u) return ((s1 >> 15) & 1u) * 255u; '
                 'if (sel == 9u) return ((s1 >> 31) & 1u) * 255u; if (sel == 10u) return ((s0 >> 15) & 1u) * 255u; '
                 'if (sel == 11u) return ((s0 >> 31) & 1u) * 255u; if (sel == 12u) return 0u; return 255u; }\n'
                 'uint x2w_perm(uint s0, uint s1, uint s2) { uint r = 0; [unroll] for (uint i = 0; i < 4u; ++i) '
                 'r |= x2w_perm_byte(s0, s1, (s2 >> (8u * i)) & 255u) << (8u * i); return r; }'),
    # v_mbcnt_{lo,hi}_u32_b32 D, S0, S1 in a 64-lane wave: S1 + the number of set S0 bits below this lane in the
    # low (lanes 0-31) or high (lanes 32-63) half of the wave mask.
    'x2w_fma': 'float x2w_fma(float a, float b, float c) { return (float)fma((double)a, (double)b, (double)c); }',
    'x2w_mbcnt_lo':'uint x2w_mbcnt_lo(uint m, uint lane) { return countbits(m & (lane >= 32u ? 0xffffffffu : ((1u << lane) - 1u))); }',
    'x2w_mbcnt_hi': 'uint x2w_mbcnt_hi(uint m, uint lane) { return lane < 32u ? 0u : countbits(m & ((1u << (lane - 32u)) - 1u)); }',
}


def scalar_regs(text):
    """SGPR numbers named in an operand string (s7, s[4:7])."""
    out = set()
    for m in re.finditer(r'\bs\[(\d+):(\d+)\]|\bs(\d+)\b', text):
        if m.group(3):
            out.add(int(m.group(3)))
        else:
            out.update(range(int(m.group(1)), int(m.group(2)) + 1))
    return out


CARRY = re.compile(r'v_(add|sub|subrev)_co(_ci)?_u32')

NO_DEST = ('s_cmp', 's_bitcmp', 's_cbranch', 's_branch', 's_waitcnt', 's_sendmsg', 's_barrier', 's_nop', 's_setreg',
           's_inst_prefetch', 's_endpgm', 's_clause', 's_setprio', 's_code_end', 's_sopp')


def reads_writes(name, asm):
    """(scalar registers read, scalar registers written, writes vcc, reads vcc) of one instruction (approximate:
    writes are only what the destination operand names)."""
    _, ops, mods = parse(asm)
    base = re.sub(r'_e(32|64)$', '', name)
    writes, reads = set(), set()
    wvcc = rvcc = False
    if base.startswith('s_') and not base.startswith(NO_DEST) and ops:
        writes = scalar_regs(ops[0])
        wvcc = ops[0].startswith('vcc')
        rest = ops[1:]
        if base.startswith(('s_load', 's_buffer_load')):
            rest = ops[1:]
    elif (base.startswith('v_cmp') and len(ops) == 3) or base == 'v_readfirstlane_b32' or base == 'v_readlane_b32':
        writes = scalar_regs(ops[0])
        wvcc = ops[0].startswith('vcc')
        rest = ops[1:]
    elif CARRY.fullmatch(base) and len(ops) >= 4:
        # VOP2/VOP3b carry-out: the disassembler names one SGPR (or vcc_lo); in wave64 the mask is the pair
        writes = scalar_regs(ops[1])
        writes |= {n + 1 for n in writes}
        wvcc = ops[1].startswith('vcc')
        rest = ops[2:]
    else:
        rest = ops
        if base.startswith('v_cmp') and len(ops) == 2:
            wvcc = not base.startswith('v_cmpx')
    for o in rest:
        reads |= scalar_regs(o)
        rvcc |= o.startswith('vcc')
    if base in ('v_cndmask_b32', 'v_addc_co_u32') and len(ops) == 3:
        rvcc = True
    if base in ('s_cbranch_vccz', 's_cbranch_vccnz'):
        rvcc = True
    return reads, writes, wvcc, rvcc


class MeshStage(G.Stage):
    """One NGG mesh stage. kind 'ms'."""

    def __init__(self, pipe, code):
        self.tainted = {'tid', 'lane', 'v5'}
        self.au, self.epoch = {}, 0
        self.au_scalars = {}
        self.broadcasts = 0
        self.divergent_until = -1
        self.exported_vertex = False
        self.exported_prim = False
        self.alloc_seen = False
        self.wave = 64
        self.exec_static = ALL_LANES  # exec as a known 64-bit lane mask (the same in every wave), else None
        self.static_copies = {}      # SGPR -> the static exec mask it holds
        self._static_set = False
        self.shadow = {}             # VGPR -> (unguarded variable, exec mask at the write); straight-line code only
        self.shadow_vars = []
        self.pending = []            # statements that start the next block (after a broadcast barrier)
        self.lds_used = False
        self.lds_written_in_phase = False
        super().__init__(pipe, 'ms', code)
        if not self.alloc_seen:
            raise Unsupported('no GS_ALLOC_REQ (s_sendmsg 0x9): not an NGG mesh stage')
        self.barriers = self.barrier_count()

    def translate(self):
        # The program ends at the s_code_end padding; the words after it are data, not code.
        for i, x in enumerate(self.ins):
            if x[2] == 's_code_end':
                self.ins = self.ins[:i]
                break
        self.has_lds = any(x[2].startswith('ds_') for x in self.ins)
        return super().translate()

    def next_offset(self):
        for off, n, _, _ in self.ins:
            if off == self.pc:
                return off + n
        return None

    def branch_targets(self):
        if getattr(self, '_targets', None) is None:
            self._targets = {int(parse(asm)[1][0].split('_')[1], 16) for _, _, name, asm in self.ins
                             if name.startswith('s_cbranch') or name == 's_branch'}
        return self._targets

    def body(self):
        """With barriers: the program runs as barriers + 1 phases, written out one after the other with a group
        barrier between them in straight-line code (no barrier inside a loop). barrier_count() put every block in
        exactly one phase; within a phase a thread runs its blocks until it reaches a barrier (pc gets
        0x10000 | the block after it) or the end (0x20000). Each console barrier is thus the phase boundary every
        wave of the group reaches."""
        if not any(b[2] and b[2][0] == 'barrier' for b in self.blocks):
            return super().body()
        index = {b[0]: i for i, b in enumerate(self.blocks)}

        def goto(off):
            if off not in index:
                raise Unsupported('branch into the middle of a block (+0x%x)' % off)
            return index[off]
        out = ['    uint pc = 0;']
        for phase in range(self.barriers + 1):
            members = [i for i in range(len(self.blocks)) if self.phase_of.get(i) == phase]
            loop = any(t and t[0] in ('jump', 'branch') and goto(t[-1]) <= i
                       for i in members for t in [self.blocks[i][2]])
            out.append('    // phase %d' % phase)
            ind = '    '
            if loop:
                out.append('    [loop] for (uint guard = 0; guard < 65536 && pc < 0x10000u; ++guard) {')
                ind = '        '
            for k, i in enumerate(members):
                off, lines, term = self.blocks[i]
                out.append('%s%sif (pc == %d) {' % (ind, 'else ' if loop and k else '', i))
                out += [ind + '    ' + line for line in lines]
                last = i + 1 >= len(self.blocks)
                nxt = '0x20000u' if last else '%d' % (i + 1)
                if not term:
                    out.append('%s    pc = %s;' % (ind, nxt))
                elif term[0] == 'end':
                    out.append('%s    pc = 0x20000u;' % ind)
                elif term[0] == 'barrier':
                    out.append('%s    pc = %s;' % (ind, '0x20000u' if last else '0x10000u | %d' % (i + 1)))
                elif term[0] == 'jump':
                    out.append('%s    pc = %d;' % (ind, goto(term[1])))
                else:
                    out.append('%s    pc = (%s) ? %d : %s;' % (ind, term[1], goto(term[2]), nxt))
                out.append('%s}' % ind)
            if loop:
                out.append('    }')
            if phase < self.barriers:
                out += ['    GroupMemoryBarrierWithGroupSync();',
                        '    if (pc < 0x20000u) pc &= 0xffffu;']
        return out

    def barrier_count(self):
        """Every path from the entry to s_endpgm must cross the same number of barriers (the hardware needs every
        wave to arrive at each barrier). Returns that number; the body runs as that many + 1 phases."""
        blocks = self.blocks
        index = {b[0]: i for i, b in enumerate(blocks)}
        memo = {}

        def counts(i, depth=0):
            if i in memo:
                return memo[i]
            if depth > 10000:
                raise Unsupported('block graph too deep')
            off, lines, term = blocks[i]
            here = 1 if term and term[0] == 'barrier' else 0
            nxt = []
            if term is None or term[0] in ('barrier', 'branch'):
                if i + 1 < len(blocks):
                    nxt.append(i + 1)
            if term and term[0] in ('jump', 'branch'):
                if term[-1] <= off:
                    if any(blocks[k][2] and blocks[k][2][0] == 'barrier' for k in range(index[term[-1]], i + 1)):
                        raise Unsupported('barrier inside a loop')
                    nxt = [k for k in nxt if k != index[term[-1]]]   # loop back edge: counted once
                else:
                    nxt.append(index[term[-1]])
            result = set()
            for k in nxt:
                result |= {here + c for c in counts(k, depth + 1)}
            if not nxt:
                result = {here}
            memo[i] = result
            return result
        found = counts(0)
        if len(found) != 1:
            raise Unsupported('paths cross different numbers of barriers: %s' % sorted(found))
        total = min(found)
        # Every reachable block then has one count of barriers still ahead (two would give two totals), so it runs
        # in exactly one phase: total - that count.
        self.phase_of = {}
        for i, c in memo.items():
            if len(c) != 1:
                raise Unsupported('block %#x reaches the end across different numbers of barriers' % blocks[i][0])
            self.phase_of[i] = total - min(c)
        return total

    # ---- entry ---------------------------------------------------------------------------------
    def setup_entry(self):
        root = self.pipe.root
        reg = 8  # user SGPRs from s8, as the pixel and vertex stages (setup_entry in gfx2hlsl)
        for index, p in enumerate(root['params']):
            if p['type'] == 0:
                self.sym[reg] = ('ptr', ('table', index))
                reg += 1
            elif p['type'] == 1:
                for i in range(p['count']):
                    self.sym[reg] = ('ud', self.pipe.ud(('rootconst', index, i)))
                    reg += 1
            else:
                self.sym[reg] = ('ptr', ('root', index))
                self.sym[reg + 1] = ('ptrhi', ('root', index))
                reg += 2
        if root['flags'] & 0x400:
            self.sym[reg] = ('ptr', ('resheap',))
            reg += 1
        if root['flags'] & 0x800:
            self.sym[reg] = ('ptr', ('smpheap',))
            reg += 1
        self.user_end = reg
        # s3: merged_wave_info (bits 27:24 = wave index in the group, Mesa radv_shader_args.c / ac_unpack_param).
        self.sym[3] = ('waveinfo',)

    # ---- taint (lane dependence) ---------------------------------------------------------------
    # Three levels per register: uniform (not in `tainted`: the same in every lane of the wave), active-uniform
    # (in `tainted` and au[reg] == epoch: the same in every lane active since the last widening of exec; e.g. a
    # value computed from uniform values under a partial exec, or `exec & uniform`), and lane-dependent.
    # `epoch` counts exec writes that can turn lanes back on (restores); narrowing writes (v_cmpx, `exec & x`, any
    # write while every lane was active) keep active-uniform values valid.
    def emit(self, text):
        self.lines.append(text)
        for stmt in text.split(';'):
            m = SIMPLE.match(stmt)
            if not m:
                continue
            target, expr = m.group(1), m.group(2)
            deps = set(TOKEN.findall(expr))
            if stmt.strip().startswith('if (exec & 1)'):
                deps.add('exec')
            if target == 'exec':
                narrow = expr.startswith('(exec & ') or getattr(self, '_known_at_start', True)
                if not narrow:
                    self.epoch += 1
            # (a scalar computed inside a per-lane region from uniform values is what the wave computes there: the
            # lanes that skip the region never see it, and check_skipped_scalars refuses it being live after)
            if not deps & self.tainted:
                self.tainted.discard(target)
                self.au.pop(target, None)
                continue
            self.tainted.add(target)
            lane = {d for d in deps & self.tainted if d != 'exec' and self.au.get(d) != self.epoch}
            if lane or target == 'exec':
                self.au.pop(target, None)
            else:
                self.au[target] = self.epoch

    def is_tainted(self, expr):
        return bool(set(TOKEN.findall(expr)) & self.tainted)

    def lane_dependent(self, name):
        """Neither uniform nor uniform over the active lanes."""
        return name in self.tainted and self.au.get(name) != self.epoch

    # ---- instructions --------------------------------------------------------------------------
    def one(self, name, asm):
        _, ops, mods = parse(asm)
        base = re.sub(r'_e(32|64)$', '', name)
        if self.pending:
            self.lines.extend(self.pending)
            self.pending = []
        if self.pc in self.branch_targets():
            self.shadow.clear()      # another path may arrive here without the shadow assignments
        if base in ('s_waitcnt_depctr',):
            return None
        if base.startswith('s_cbranch_exec') and self.exec_static is not None:
            # exec is a known mask (the same in every wave): the wave-level branch is decided here
            taken = (self.exec_static == 0) == (base == 's_cbranch_execz')
            return ('jump', int(ops[0].split('_')[1], 16)) if taken else None
        if base.startswith('ds_'):
            return self.lds(base, ops, mods)
        if base in FUSED and EXACT_FMA:
            # v_fma / v_fmac / v_fmamk / v_fmaak are fused (one rounding); `a * b + c` lets the compiler round the
            # product first. The product of two binary32 values is exact in binary64, so fma in double then one
            # rounding to float gives the fused result (except for rare double-rounding ties).
            arity, _ = G.VEC_F[base]
            args = [self.f(o) for o in ops[1:1 + arity]]
            if base == 'v_fmac_f32':
                args.append(self.f(ops[0]))
            return self.set_vf(reg_range(ops[0])[1], 'x2w_fma(%s, %s, %s)' % tuple(args), mods)
        if base == 's_sendmsg':
            if ops != ['0x9']:
                raise Unsupported('s_sendmsg %s' % ops)
            self.alloc_seen = True
            if self.au_scalars.get('m0', self.epoch) != self.epoch:
                raise Unsupported('m0 (from v_readfirstlane under a partial exec) sent after exec was widened')
            # Wave-level message: the wave sends it when it reaches it. Lanes active here all hold the same m0.
            self.emit('if (exec & 1) x2w_alloc = m0;')
            self.alloc_guarded = True
            return None
        if base == 's_barrier':
            if not self.has_lds:
                # A barrier orders LDS (and memory) traffic between the waves; a program without LDS instructions
                # writes nothing another wave reads here (the archive form's barriers sit in per-lane regions
                # around the allocation, which the one-lane model could not place anyway)
                return None
            # s_barrier runs whatever exec is; a lane that skipped it per lane would arrive at a different one
            if self.pc < self.divergent_until:
                raise Unsupported('s_barrier inside a per-lane skipped range at %#x' % self.pc)
            self.lds_written_in_phase = False
            return ('barrier',)
        if base in ('s_waitcnt_vmcnt', 's_waitcnt_lgkmcnt', 's_waitcnt_expcnt', 's_waitcnt_vscnt'):
            return None
        # Every scalar destination stops being a lane mask unless the handler marks it again (the base
        # translator only updates the mask set in its ALU paths, so a load into a former mask register kept
        # the mark and a later s_or of it was cut to bit 0).
        reads, writes, _, _ = reads_writes(name, asm)
        self.masks -= writes
        if base == 's_sendmsg' or 'm0' in ops[1:]:
            reads = reads | {'m0'}
        if ops and ops[0] == 'm0':
            writes = writes | {'m0'}
        # A scalar read from the first active lane while some lanes were off holds, in the one-lane model, each
        # lane's own value: right in the lanes that were active, stale in the others. Once exec has been widened
        # those other lanes run again, so such a register must not be read any more.
        stale = [r for r in reads if r in self.au_scalars and self.au_scalars[r] != self.epoch]
        if stale:
            raise Unsupported('%s (from v_readfirstlane under a partial exec) read after exec was widened at %#x'
                              % (stale[0], self.pc))
        derived = any(r in self.au_scalars for r in reads)
        for r in writes:
            if derived and base.startswith('s_'):
                self.au_scalars[r] = self.epoch   # computed from such a value: the same caveat
            else:
                self.au_scalars.pop(r, None)
        self._known_at_start = self.exec_known
        self._static_set = False
        for r in writes:
            self.static_copies.pop(r, None)
        term = super().one(name, asm)
        if not self._static_set and self.exec_written(name, ops):
            self.exec_static = None   # exec changed in a way not modelled as a constant mask
        if term and term[0] == 'branch':
            cond = term[1]
            off = self.pc
            target = term[2]
            per_lane = base[10:] in ('execz', 'execnz', 'vccz', 'vccnz')
            lane_cond = self.is_tainted(cond)
            if base[10:] in ('scc0', 'scc1') and lane_cond:
                raise Unsupported('branch on an SCC that depends on lane masks at %#x' % off)
            if per_lane and lane_cond:
                if target <= off:
                    raise Unsupported('per-lane loop branch %s at %#x' % (base, off))
                # vcc must be the same in every active lane (vcc = exec & uniform): inactive lanes then read 0 and
                # take the branch the way execz would, which the skipped-range check below covers
                if base[10:] in ('vccz', 'vccnz') and self.lane_dependent('vcc'):
                    raise Unsupported('%s on a lane-dependent vcc at %#x' % (base, off))
                self.check_skipped_scalars(off, target)
                self.divergent_until = max(self.divergent_until, target)
        return term

    @staticmethod
    def exec_written(name, ops):
        base = re.sub(r'_e(32|64)$', '', name)
        return ('saveexec' in base or base.startswith('v_cmpx') or
                (base.startswith('s_') and ops and ops[0] in ('exec', 'exec_lo')))

    def exec_mask_expr(self, mask):
        """This lane's bit of a constant 64-bit exec mask."""
        lo, hi = mask & 0xffffffff, mask >> 32
        if hi == 0 and lo == 1:
            return '(lane == 0u ? 1u : 0u)'
        return '((lane < 32u ? (0x%08xu >> lane) : (0x%08xu >> (lane - 32u))) & 1u)' % (lo, hi)

    def set_v(self, n, expr):
        # Shadow: under a constant partial exec, a value computed only from wave-uniform inputs is also kept in an
        # unguarded variable, so a later v_readfirstlane can read it although the HLSL thread's lane was off
        # (the hardware reads the first active lane, which held that same value if it was active at the write).
        sub = expr
        if self.exec_static is not None and not self.exec_known:
            for v in set(re.findall(r'\bv(\d+)\b', expr)):
                if int(v) in self.shadow:
                    sub = re.sub(r'\bv%s\b' % v, self.shadow[int(v)][0], sub)
        super().set_v(n, expr)
        self.shadow.pop(n, None)
        if self.exec_static is None or self.exec_known or self.exec_static == 0:
            return
        if set(TOKEN.findall(sub)) & (self.tainted | {'exec', 'lane', 'tid'}):
            return
        var = 'x2w_u%d' % len(self.shadow_vars)
        self.shadow_vars.append(var)
        self.lines.append('%s = %s;' % (var, sub))
        self.shadow[n] = (var, self.exec_static)

    def declarations(self):
        lines = super().declarations()
        if self.shadow_vars:
            lines.append('    uint %s;' % ', '.join('%s = 0' % v for v in self.shadow_vars))
        return lines

    def snapshot(self):
        return super().snapshot() + (self.exec_static, dict(self.static_copies))

    def merge(self, states):
        super().merge(states)
        statics = [s[3] for s in states]
        self.exec_static = statics[0] if all(x == statics[0] for x in statics) else None
        self.static_copies = {r: m for r, m in states[0][4].items() if all(s[4].get(r) == m for s in states)}

    def set_exec_static(self, mask):
        """exec = a constant lane mask (s_mov_b64 exec, <constant> or a copy of one)."""
        mask &= ALL_LANES
        self._static_set = True
        self.exec_static = mask
        if mask == ALL_LANES:
            self.emit('exec = 1;')
            self.exec_known = True
        else:
            self.emit('exec = %s;' % self.exec_mask_expr(mask))
            self.exec_known = False

    def op_s_and_b32(self, ops, mods):
        # merged_wave_info & mask: the wave index field (bits 27:24) is the only part this translation models
        for i, j in ((1, 2), (2, 1)):
            src = reg_range(ops[i])
            if src and src[0] == 's' and self.sym.get(src[1]) == ('waveinfo',):
                if not re.fullmatch(r'0x[0-9a-f]+|\d+', ops[j]) or int(ops[j], 0) & ~0x0f000000:
                    raise Unsupported('merged_wave_info & %s' % ops[j])
                d = reg_range(ops[0])
                if not d or d[0] != 's':
                    raise Unsupported('s_and_b32 of merged_wave_info to %s' % ops[0])
                self.masks.discard(d[1])
                self.set_s(d[1], '((wave << 24) & 0x%08xu)' % int(ops[j], 0), scc='$ != 0u')
                return None
        return self.scalar('s_and_b32', ops)

    def lds(self, base, ops, mods):
        """ds_read_b32 / ds_write_b32 / _b64 with offset:N as groupshared uint x2w_lds[] (byte address / 4).
        Lanes of one wave run independently between barriers here, so an LDS read after an LDS write in the same
        barrier phase (lane-to-lane traffic inside a wave) is refused."""
        size = {'ds_read_b32': 1, 'ds_write_b32': 1, 'ds_read_b64': 2, 'ds_write_b64': 2}.get(base)
        if size is None:
            raise Unsupported(base)
        off = 0
        for m in mods:
            if m.startswith('offset:'):
                off = int(m[7:], 0)
            else:
                raise Unsupported('%s with %s' % (base, m))
        if off + 4 * size > 4 * LDS_DWORDS:
            raise Unsupported('%s offset %d beyond the modelled LDS' % (base, off))
        self.lds_used = True
        if base.startswith('ds_write'):
            addr, data = ops[0], reg_range(ops[1])
            self.emit('a = (%s + %du) >> 2;' % (self.u(addr), off))
            for i in range(size):
                self.emit('if (exec & 1) x2w_lds[min(a + %du, %du)] = v%d;' % (i, LDS_DWORDS - 1, data[1] + i))
                self.used_v.add(data[1] + i)
            self.lds_written_in_phase = True
            return None
        if self.lds_written_in_phase:
            raise Unsupported('LDS read after an LDS write without a barrier at %#x' % self.pc)
        dst, addr = reg_range(ops[0]), ops[1]
        ar = reg_range(addr)
        if ar and ar[0] == 'v' and dst[1] <= ar[1] < dst[1] + size - 1:   # a later word would read the new value
            raise Unsupported('%s overwrites its address register' % base)
        for i in range(size):
            # the address expression is repeated (no temporary), so a shadow (see set_v) can substitute into it
            self.set_v(dst[1] + i, 'x2w_lds[min(((%s + %du) >> 2) + %du, %du)]' % (self.u(addr), off, i, LDS_DWORDS - 1))
        return None

    def liveness(self):
        """Scalar registers (numbers; -1 = vcc) live on entry to each instruction offset (backward dataflow over
        fall-through and branch edges)."""
        if getattr(self, '_live', None) is not None:
            return self._live
        order = [x[0] for x in self.ins]
        info = {x[0]: (x[2], x[3]) for x in self.ins}
        following = dict(zip(order, order[1:]))
        rw = {}
        succ = {}
        for off in order:
            name, asm = info[off]
            r, w, wv, rv = reads_writes(name, asm)
            rw[off] = (frozenset(r | ({-1} if rv else set())), frozenset(w | ({-1} if wv else set())))
            nxt = [] if name in ('s_endpgm', 's_branch') else ([following[off]] if off in following else [])
            if name.startswith('s_cbranch') or name == 's_branch':
                nxt.append(int(parse(asm)[1][0].split('_')[1], 16))
            succ[off] = [o for o in nxt if o in info]
        live = {off: frozenset() for off in order}
        changed = True
        while changed:
            changed = False
            for off in reversed(order):
                out = frozenset().union(*(live[s] for s in succ[off])) if succ[off] else frozenset()
                r, w = rw[off]
                new = r | (out - w)
                if new != live[off]:
                    live[off], changed = new, True
        self._live = live
        return live

    def check_skipped_scalars(self, off, target):
        """Refuse when a scalar register written in (off, target) is live at target (some path reads it before a
        write): lanes that skip the range would see the old value where the wave saw the new one."""
        written = set()
        copies = set()   # registers holding exec as it was inside the range (nested if/else masks)
        for o, n, name, asm in self.ins:
            if off < o < target:
                r, w, wv, _ = reads_writes(name, asm)
                written |= w | ({-1} if wv else set())
                ops = parse(asm)[1]
                low = [reg_range(t)[1] if reg_range(t) and reg_range(t)[0] == 's' else t for t in ops]
                if name.startswith('s_') and 'saveexec' in name:
                    copies.add(low[0])
                    continue
                if name in ('s_mov_b64', 's_mov_b32') and len(ops) == 2 and ops[1] in ('exec', 'exec_lo'):
                    copies.add(low[0])
                    continue
                if (re.match(r's_(and|andn2)_b(32|64)$', name) and len(ops) == 3 and ops[0] not in ('exec', 'exec_lo')
                        and low[1] in copies):
                    copies.add(low[0])   # a subset of a copy taken in the range (`copy & ~exec`: the else lanes)
                    continue
                if name.startswith('s_') and ops and ops[0] in ('exec', 'exec_lo'):
                    # A wave-level exec write can turn on lanes the per-lane model left behind, unless it only
                    # narrows exec (and / andn2 with exec) or restores / complements a copy taken in the range.
                    srcs = low[1:]
                    if re.match(r's_(and|andn2)_b(32|64)$', name) and 'exec' in ops[1:]:
                        pass
                    elif name in ('s_mov_b64', 's_mov_b32') and srcs[0] in copies:
                        pass
                    elif re.match(r's_andn2_b(32|64)$', name) and srcs[0] in copies:
                        pass   # else branch: copy & ~exec is inside the copy
                    else:
                        raise Unsupported('exec written by %s inside the per-lane skipped range %#x-%#x'
                                          % (name, off, target))
                for t in w:
                    copies.discard(t)
        hit = written & self.liveness().get(target, frozenset())
        if hit:
            raise Unsupported('scalar %s written in the per-lane skipped range %#x-%#x is live at its end'
                              % ('vcc' if min(hit) == -1 else 's%d' % min(hit), off, target))

    def scalar(self, base, ops):
        dname = ops[0]
        r0 = reg_range(dname)
        if dname == 'm0':
            text, _ = G.SCA[base]
            srcs = [('s%d' % reg_range(t)[1]) if reg_range(t) and reg_range(t)[0] == 's' else t for t in ops[1:]]
            self.emit('m0 = %s;' % text.format(*[self.u(t) for t in srcs]))
            return None
        # restoring exec from a copy taken while every lane was active makes every lane active again;
        # s_mov_b64 exec, -1 (the program's first instruction) activates all 64 lanes, which all exist
        # (128 threads = two full waves)
        if dname == 'exec' and base == 's_mov_b64':
            if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', ops[1]):
                self.set_exec_static(int(ops[1], 0))
                return None
            src = reg_range(ops[1])
            if src and src[0] == 's' and src[1] in self.static_copies:
                self.set_exec_static(self.static_copies[src[1]])
                return None
            if src and src[0] == 's' and self.sym.get(src[1]) == ('execcopy',):
                self.set_exec_static(ALL_LANES)
                return None
        if base == 's_mov_b64' and len(ops) == 2 and ops[1] == 'exec' and self.exec_static is not None:
            d = reg_range(dname)
            if d and d[0] == 's':
                mask = self.exec_static
                term = super().scalar(base, ops)
                self.static_copies[d[1]] = mask
                return term
        if r0 and r0[0] == 's':
            for t in ops[1:]:
                rr = reg_range(t)
                if rr and rr[0] == 's' and self.sym.get(rr[1]) == ('waveinfo',):
                    raise Unsupported('merged_wave_info used by %s' % base)
        if dname == 'null':
            # the null destination discards the result; only SCC would change, and the moves seen write none
            if base not in ('s_mov_b32', 's_mov_b64'):
                raise Unsupported('%s to null' % base)
            return None
        if not r0 and dname not in ('exec', 'exec_lo', 'vcc', 'vcc_lo'):
            raise Unsupported('%s to %s' % (base, dname))
        return super().scalar(base, ops)

    def op_s_bfe_u32(self, ops, mods):
        src = reg_range(ops[1])
        if src and src[0] == 's' and self.sym.get(src[1]) == ('waveinfo',):
            if ops[2] != '0x40018':
                raise Unsupported('merged_wave_info field %s' % ops[2])
            self.set_s(reg_range(ops[0])[1], 'wave')
            return None
        return self.scalar('s_bfe_u32', ops)

    def op_s_bfe_i32(self, ops, mods):
        # D = sext(S0 >> S1[4:0]) of width S1[22:16]; a width of 0 gives 0 (RDNA 2 ISA, S_BFE_I32).
        a, b = self.u(ops[1]), self.u(ops[2])
        expr = ('((({1} >> 16) & 127u) == 0u ? 0u : asuint(asint({0} << (32u - min(({1} & 31u) + (({1} >> 16) & 127u), 32u))) '
                '>> (32u - min(({1} & 31u) + (({1} >> 16) & 127u), 32u) + ({1} & 31u))))').format(a, b)
        self.set_s(reg_range(ops[0])[1], expr, scc='$ != 0')

    def op_s_bitset1_b32(self, ops, mods):
        d = reg_range(ops[0])[1]
        own = self.sym.get(d)
        if own and own[0] in ('ptrhi', 'descnum'):
            # a descriptor word built from a root pointer (stride / swizzle bits): keeps its descriptor meaning
            self.sym[d] = ('descnum',) if own[0] == 'descnum' else own
            return None
        self.set_s(d, '%s | (1u << (%s & 31u))' % (self.S(d), self.u(ops[1])))

    # s_pack_{lh,hl,hh}_b32_b16: D = low half from S0 (l = bits 15:0, h = bits 31:16), high half from S1
    # (RDNA 2 ISA, S_PACK_*). s_pack_ll is in the base table.
    def pack(self, ops, text):
        d = reg_range(ops[0])
        if not d or d[0] != 's':
            raise Unsupported('s_pack to %s' % ops[0])
        self.masks.discard(d[1])
        self.set_s(d[1], text.format(self.u(ops[1]), self.u(ops[2])))

    def op_s_pack_lh_b32_b16(self, ops, mods):
        self.pack(ops, '(({0} & 0xffffu) | ({1} & 0xffff0000u))')

    def op_s_pack_hl_b32_b16(self, ops, mods):
        self.pack(ops, '(({0} >> 16) | ({1} << 16))')

    def op_s_pack_hh_b32_b16(self, ops, mods):
        self.pack(ops, '(({0} >> 16) | ({1} & 0xffff0000u))')

    def carry_op(self, base, ops, mods):
        """v_add_co_u32 / v_sub_co_u32 / v_subrev_co_u32 and the _ci forms: D, carry-out mask, S0, S1[, carry-in mask].
        Add: sum = S0 + S1 + cin (mod 2^32), carry = cin ? sum <= S0 : sum < S0. Sub: d = S0 - S1 - bin, borrow =
        bin ? d >= S0 : d > S0 (subrev swaps the sources). Both read only S0, the result and the carry-in, so the
        carry-in may be the destination mask. The carry is the lane's bit of the destination mask."""
        m = CARRY.fullmatch(base)
        if mods or len(ops) != (5 if m.group(2) else 4):
            raise Unsupported('%s form %s %s' % (base, ops, mods))
        x, y = self.u(ops[2]), self.u(ops[3])
        if m.group(1) == 'subrev':
            x, y = y, x
        cin = '(%s & 1u)' % self.u(ops[4]) if m.group(2) else '0u'
        if m.group(1) == 'add':
            self.emit('a = %s; w = a + %s + %s;' % (x, y, cin))
            carry = '((%s != 0u ? w <= a : w < a) ? 1u : 0u)' % cin
        else:
            self.emit('a = %s; w = a - %s - %s;' % (x, y, cin))
            carry = '((%s != 0u ? w >= a : w > a) ? 1u : 0u)' % cin
        dst = ops[1]
        if dst.startswith('vcc'):
            dst = 'vcc'
        else:
            r = reg_range(dst)
            if not r or r[0] != 's':
                raise Unsupported('%s carry to %s' % (base, dst))
            dst = 's%d' % r[1]
        self.write_compare(False, dst, carry)
        self.set_v(reg_range(ops[0])[1], 'w')

    def op_v_mbcnt_lo_u32_b32(self, ops, mods):
        self.set_v(reg_range(ops[0])[1], 'x2w_mbcnt_lo(%s, lane) + %s' % (self.u(ops[1]), self.u(ops[2])))

    def op_v_mbcnt_hi_u32_b32(self, ops, mods):
        self.set_v(reg_range(ops[0])[1], 'x2w_mbcnt_hi(%s, lane) + %s' % (self.u(ops[1]), self.u(ops[2])))

    def op_v_perm_b32(self, ops, mods):
        if mods:
            raise Unsupported('v_perm_b32 with %s' % mods)
        self.set_v(reg_range(ops[0])[1], 'x2w_perm(%s, %s, %s)' % (self.u(ops[1]), self.u(ops[2]), self.u(ops[3])))

    def op_v_pack_b32_f16(self, ops, mods):
        sel = [0, 0, 0]
        for m in mods:
            if m.startswith('op_sel:['):
                sel = [int(x) for x in m[8:-1].split(',')]
            else:
                raise Unsupported('v_pack_b32_f16 with %s' % m)
        if any(t.startswith(('-', '|')) for t in ops[1:3]):
            raise Unsupported('v_pack_b32_f16 with source modifiers')
        lo = '(%s >> 16)' % self.h(ops[1]) if sel[0] else self.h(ops[1])
        hi = '(%s >> 16)' % self.h(ops[2]) if sel[1] else self.h(ops[2])
        self.set_v(reg_range(ops[0])[1], '(%s & 0xffffu) | ((%s & 0xffffu) << 16)' % (lo, hi))

    def op_v_readfirstlane_b32(self, ops, mods):
        src, dst = reg_range(ops[1]), reg_range(ops[0])
        if mods or not dst or dst[0] != 's' or not src or src[0] != 'v':
            raise Unsupported('v_readfirstlane_b32 form %s' % ops)
        self.masks.discard(dst[1])   # a number now, not a lane mask (s_or of it must not be masked to bit 0)
        if src[1] == 5 and 5 not in self.written_v:
            # v5 = group * 128 + thread (inferred fast-launch vertex id); the wave's first lane is its thread & ~63
            # (`wbase` = the wave's first thread: the same in every lane of a wave, so not lane-tainted)
            self.set_s(dst[1], '(gid * %du + wbase)' % GROUP)
            return None
        shadow = self.shadow.get(src[1])
        read_mask = ALL_LANES if self.exec_known else self.exec_static
        if shadow and read_mask:
            first = (read_mask & -read_mask).bit_length() - 1
            if (shadow[1] >> first) & 1:
                # the first active lane wrote this wave-uniform value: every thread takes it from the shadow
                self.set_s(dst[1], shadow[0])
                return None
        # A source that is the same in every active lane: each active lane's own value is the first active lane's
        # (the result is then active-uniform itself, through emit).
        if self.lane_dependent('v%d' % src[1]):
            # Lane-dependent source: the console wave reads its first active lane. With every lane active and
            # no per-lane region open (so every thread of the group reaches this statement, a group barrier is
            # legal), that lane is the wave's first thread: broadcast through groupshared memory.
            if not self.exec_known or self.pc < self.divergent_until:
                raise Unsupported('v_readfirstlane_b32 of a lane-dependent v%d with lanes off at %#x' % (src[1], self.pc))
            # The group barrier is a phase boundary of the body (see body()): the read starts the next block, so
            # that block must not also be entered by a branch.
            nxt = self.next_offset()
            if nxt in self.branch_targets():
                raise Unsupported('v_readfirstlane_b32 broadcast followed by a branch target at %#x' % self.pc)
            slot = self.broadcasts
            self.broadcasts += 1
            self.emit('if (lane == 0u) x2w_rfl[%d][wave] = %s;' % (slot, self.u(ops[1])))
            self.sym.pop(dst[1], None)
            self.used_s.add(dst[1])
            self.pending.append('s%d = x2w_rfl[%d][wave];' % (dst[1], slot))
            self.tainted.discard('s%d' % dst[1])
            self.au.pop('s%d' % dst[1], None)
            self.lds_written_in_phase = False
            return ('barrier',)
        self.set_s(dst[1], self.u(ops[1]))
        if self.is_tainted('v%d' % src[1]):
            self.au_scalars[dst[1]] = self.epoch

    def op_s_buffer_load_dword(self, ops, mods, count=1):
        """As the pipeline translator, plus: the offset is read once before the destination is written (the
        title's `s_buffer_load_dwordx2 s[4:5], s[16:19], s4` overwrote s4 before the second word), the
        instruction's offset:N is added, and the address is dword aligned (the hardware ignores bits 1:0)."""
        extra = 0
        for m in mods:
            if m.startswith('offset:'):
                extra += int(m[7:], 0)
            else:
                raise Unsupported('s_buffer_load with %s' % m)
        dst = reg_range(ops[0])
        buf = self.pipe.resource(self.descriptor(ops[1]), 'buf')
        fixed = re.fullmatch(r'0x[0-9a-f]+|\d+', ops[2])
        if fixed:
            base = '%du' % ((int(ops[2], 0) + extra) & ~3)
        else:
            self.emit('a = (%s + %du) & ~3u;' % (self.u(ops[2]), extra))
            base = 'a'
        for i in range(count):
            self.set_s(dst[1] + i, '%s.Load(%s + %d)' % (buf, base, 4 * i))
            if fixed:
                self.sym[dst[1] + i] = ('fixed_buffer_word',)

    # Typed buffer formats the pipeline translator's table lacks. SNORM: max(x / (2^(n-1) - 1), -1) (RDNA 2 ISA
    # data-format conversion). 2_10_10_10 lists fields from the most significant: x is bits 9:0 and w bits 31:30
    # (Mesa gfx10_format_table.py maps R10G10B10A2 to 2_10_10_10).
    MESH_FORMATS = {
        'BUF_FMT_16_16_SNORM': ('ldu({0}, a)', ['U(max(float(asint(w << 16) >> 16) / 32767.0, -1.0))',
                                                'U(max(float(asint(w) >> 16) / 32767.0, -1.0))', '0u', 'U(1.0)']),
        'BUF_FMT_16_16_16_16_SNORM': ('ldu({0}, a)', ['U(max(float(asint(w << 16) >> 16) / 32767.0, -1.0))',
                                                      'U(max(float(asint(w) >> 16) / 32767.0, -1.0))',
                                                      'U(max(float(asint(ldu({0}, a + 4) << 16) >> 16) / 32767.0, -1.0))',
                                                      'U(max(float(asint(ldu({0}, a + 4)) >> 16) / 32767.0, -1.0))']),
        'BUF_FMT_16_16_16_16_UNORM': ('ldu({0}, a)', ['U((w & 65535) / 65535.0)', 'U((w >> 16) / 65535.0)',
                                                      'U((ldu({0}, a + 4) & 65535) / 65535.0)',
                                                      'U((ldu({0}, a + 4) >> 16) / 65535.0)']),
        'BUF_FMT_8_8_8_8_SNORM': ('ldu({0}, a)', ['U(max(float(asint(w << 24) >> 24) / 127.0, -1.0))',
                                                  'U(max(float(asint(w << 16) >> 24) / 127.0, -1.0))',
                                                  'U(max(float(asint(w << 8) >> 24) / 127.0, -1.0))',
                                                  'U(max(float(asint(w) >> 24) / 127.0, -1.0))']),
        'BUF_FMT_2_10_10_10_UNORM': ('ldu({0}, a)', ['U((w & 1023) / 1023.0)', 'U(((w >> 10) & 1023) / 1023.0)',
                                                     'U(((w >> 20) & 1023) / 1023.0)', 'U((w >> 30) / 3.0)']),
        # integer formats: the register gets the integer; missing components read 0, w reads 1 (as the float
        # formats' 1.0)
        'BUF_FMT_8_8_8_8_UINT': ('ldu({0}, a)', ['w & 255u', '(w >> 8) & 255u', '(w >> 16) & 255u', 'w >> 24']),
        'BUF_FMT_8_8_UINT': ('ldu({0}, a)', ['w & 255u', '(w >> 8) & 255u', '0u', '1u']),
        'BUF_FMT_16_16_UINT': ('ldu({0}, a)', ['w & 65535u', 'w >> 16', '0u', '1u']),
        'BUF_FMT_16_UINT': ('ldu({0}, a)', ['w & 65535u', '0u', '0u', '1u']),
        'BUF_FMT_16_16_16_16_UINT': ('ldu({0}, a)', ['w & 65535u', 'w >> 16', 'ldu({0}, a + 4) & 65535u',
                                                     'ldu({0}, a + 4) >> 16']),
        'BUF_FMT_32_UINT': ('ldu({0}, a)', ['w', '0u', '0u', '1u']),
        'BUF_FMT_32_32_UINT': ('ldu({0}, a)', ['w', 'ldu({0}, a + 4)', '0u', '1u']),
        'BUF_FMT_32_32_32_32_UINT': ('ldu({0}, a)', ['w', 'ldu({0}, a + 4)', 'ldu({0}, a + 8)', 'ldu({0}, a + 12)']),
    }

    def tbuffer(self, ops, mods, count):
        formats = [m for m in mods if m.startswith('format:')]
        fmt = formats[0][8:-1] if formats else None
        if fmt not in self.MESH_FORMATS:
            return super().tbuffer(ops, mods, count)
        dst = reg_range(ops[0])
        buf, address = self.buffer_address(ops, [m for m in mods if not m.startswith('format:')])
        self.emit('a = %s;' % address)
        load, comps = self.MESH_FORMATS[fmt]
        self.emit('w = %s;' % load.format(buf))
        for i in range(count):
            self.set_v(dst[1] + i, comps[i].format(buf))

    def export(self, asm):
        m = re.fullmatch(r'exp (\w+) (.*?)((?: done| compr| vm)*)', asm)
        if not m:
            raise Unsupported('export form %r' % asm)
        target, vals = m.group(1), [v.strip() for v in m.group(2).split(',')]
        if target in ('prim', 'target_20'):
            if vals[0] == 'off' or any(v != 'off' for v in vals[1:]) or 'compr' in m.group(3):
                raise Unsupported('primitive export form %r' % asm)
            self.exported_prim = True
            self.emit('if (exec & 1) { x2w_prim = %s; }' % self.u(vals[0]))
            return None
        if target != 'pos0' and not target.startswith('param'):
            raise Unsupported('mesh export target %s' % target)
        before = len(self.lines)
        self.kind = 'vs'  # the base export only accepts pos/param outside a pixel stage
        try:
            super().export(asm)
        finally:
            self.kind = 'ms'
        body = self.lines[before:]
        del self.lines[before:]
        self.lines.append('if (exec & 1) { %s }' % ' '.join(body))
        self.exported_vertex = True
        return None


for _op in ('add', 'sub', 'subrev'):
    for _ci in ('', '_ci'):
        _name = 'v_%s_co%s_u32' % (_op, _ci)
        setattr(MeshStage, 'op_' + _name, lambda self, ops, mods, _n=_name: self.carry_op(_n, ops, mods))


class MeshPipeline(G.Pipeline):
    """Mesh + pixel pipeline. ps_code None: the mesh stage alone (coverage runs; params = what it exports)."""

    def __init__(self, ms_code, ps_code, root_blob, sampler_heap=None, rtv_formats=()):
        self.root = G.parse_root(root_blob)
        self.rtv_formats = rtv_formats
        self.sampler_heap = os.environ.get('X2W_SAMPLER_HEAP') == '1' if sampler_heap is None else sampler_heap
        self.uses_sampler_heap = False
        self.resources, self.samplers, self.uds = {}, {}, []
        self.notes = set()
        self.modified = set()
        self.helper_text = None
        self.rect = self.ud(('rect',))
        self.vs = MeshStage(self, ms_code)
        self.ps = G.Stage(self, 'ps', ps_code) if ps_code is not None else None
        self.params = sorted(int(k[5:]) for k in self.vs.exports if k.startswith('param'))
        if self.ps is not None:
            self.ps_entry_reads = dict(self.ps.entry_reads)
            self.flat = sorted(a for a, how in self.ps.attrs.items() if 'flat' in how)
            missing = [a for a in self.ps.attrs if a not in self.params]
            if missing:
                raise Unsupported('pixel stage reads attributes %s the mesh stage does not export' % missing)
        else:
            self.ps_entry_reads, self.flat = {}, []
        if 'pos' not in self.vs.exports:
            raise Unsupported('mesh stage exports no position')
        if not self.vs.exported_prim:
            raise Unsupported('mesh stage exports no primitive')

    def common(self):
        if self.helper_text is None:
            self.helper_text = '\n'.join(self.vs.body() + (self.ps.body() if self.ps else []))
        return super().common()

    def hlsl_ms(self, dump=False):
        """The mesh shader. dump=True (tests only): also store each thread's outputs and the group's allocation
        word in RWByteAddressBuffer u0 (per group: 16 bytes header [alloc], then per thread pos, param0..N-1,
        primitive word) and give each primitive SV_PrimitiveID = group * 128 + index."""
        s = self.vs
        nparams = max(self.params) + 1 if self.params else 0
        stride = 4 * (4 + 4 * nparams + 1)
        out = self.common()
        body_text = self.helper_text
        out += [text for name, text in MESH_HELPERS.items() if re.search(r'\b%s\(' % name, body_text)]
        out += ['struct MSOut {'] + self.interface(False) + ['};',
                'struct MSPrim { bool cull : SV_CullPrimitive;%s };' % (' uint pid : SV_PrimitiveID;' if dump else '')] + (
                ['RWByteAddressBuffer x2w_dump : register(u0);'] if dump else []) + [
                'groupshared uint x2w_alloc;'] + (
                ['groupshared uint x2w_rfl[%d][2];  // v_readfirstlane broadcasts, one slot per instruction' % s.broadcasts]
                if s.broadcasts else []) + (
                ['groupshared uint x2w_lds[%d];  // LDS (ds_read / ds_write), dword addressed' % LDS_DWORDS]
                if s.lds_used else []) + [
                '[outputtopology("triangle")]',
                '[numthreads(%d, 1, 1)]' % GROUP,
                'void main(uint tid : SV_GroupIndex, uint3 group : SV_GroupID,',
                '          out vertices MSOut verts[%d], out indices uint3 tris[%d], out primitives MSPrim prims[%d]) {'
                % (MAX_VERTICES, MAX_PRIMITIVES, MAX_PRIMITIVES),
                '    uint gid = group.x, lane = tid & 63u, wave = tid >> 6, wbase = tid & ~63u;',
                '    if (tid == 0) x2w_alloc = 0;',
                '    GroupMemoryBarrierWithGroupSync();',
                '    MSOut o = (MSOut)0;',
                '    uint x2w_prim = 0x80000000u;']
        s.used_v.add(5)
        flat = re.compile(r'.*o\.fparam(\d+) = ')
        body = []
        for line in s.body():
            # attributes the pixel stage reads only interpolated: drop the flat copy (as hlsl_vs)
            line = re.sub(r'o\.fparam(\d+) = [^;]*;', lambda mm: mm.group(0) if int(mm.group(1)) in self.flat else '', line)
            body.append(line)
        out += s.declarations() + ['    v5 = gid * %du + tid;  // inferred fast-launch vertex id' % GROUP] + body
        out += ['    GroupMemoryBarrierWithGroupSync();',
                '    uint nv = min(x2w_alloc & 0x7ffu, %du), np = min((x2w_alloc >> 12) & 0x7ffu, %du);' % (MAX_VERTICES, MAX_PRIMITIVES),
                '    SetMeshOutputCounts(nv, np);',
                '    if (tid < nv) verts[tid] = o;',
                '    if (tid < np) {',
                '        uint3 t = uint3(x2w_prim & 0x1ffu, (x2w_prim >> 10) & 0x1ffu, (x2w_prim >> 20) & 0x1ffu);',
                '        bool cull = (x2w_prim >> 31) != 0u || t.x >= nv || t.y >= nv || t.z >= nv;',
                '        tris[tid] = cull ? uint3(0, 0, 0) : t;',
                '        prims[tid].cull = cull;'] + (
                ['        prims[tid].pid = gid * %du + tid;' % GROUP] if dump else []) + [
                '    }']
        if dump:
            out += ['    uint db = gid * %du, tb = db + 16u + tid * %du;' % (16 + GROUP * stride, stride),
                    '    if (tid == 0) x2w_dump.Store(db, x2w_alloc);',
                    '    x2w_dump.Store4(tb, asuint(o.pos));']
            out += ['    x2w_dump.Store4(tb + %du, asuint(o.param%d));' % (16 + 16 * p, p) for p in range(nparams)]
            out += ['    x2w_dump.Store(tb + %du, x2w_prim);' % (16 + 16 * nparams)]
        out += ['}']
        return '\n'.join(out) + '\n'

    def manifest(self):
        text = super().manifest() if self.ps else self.manifest_ms_only()
        return text + 'stage mesh\nms %d %d %d\n' % (GROUP, MAX_VERTICES, MAX_PRIMITIVES)

    def manifest_ms_only(self):
        class _P:  # base manifest() reads self.ps.exports for the targets line
            exports = {}
        saved, self.ps = self.ps, _P()
        try:
            return G.Pipeline.manifest(self)
        finally:
            self.ps = saved


DXC = os.environ.get('X2W_DXC', r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe')


def compile_hlsl(path, profile, out):
    r = subprocess.run([DXC, '-nologo', '-T', profile, '-E', 'main', '-Fo', out, path], capture_output=True, text=True)
    return r.returncode, r.stderr


def translate_to(out_dir, key, ms_code, ps_code, root_blob, check=True, rtv_formats=(), rtv_request=''):
    """Writes <key>.ms.hlsl, <key>.ps.hlsl, the .cso files and <key>.txt (last), or <key>.fail."""
    name = os.path.join(out_dir, '%016x' % key)
    try:
        pipe = MeshPipeline(ms_code, ps_code, root_blob, rtv_formats=rtv_formats)
        if pipe.ps_entry_reads:
            reg, off = min(pipe.ps_entry_reads.items(), key=lambda item: item[1])
            raise Unsupported('pixel stage reads entry VGPR v%d (an SPI input; layout not in the request) at %#x' % (reg, off))
        ms = pipe.hlsl_ms()
        ps = pipe.hlsl_ps() if ps_code is not None else None
        manifest = pipe.manifest() + (rtv_request + '\n' if rtv_request else '')
    except Unsupported as reason:
        open(name + '.fail', 'w').write(str(reason) + '\n')
        return 'unsupported: %s' % reason
    open(name + '.ms.hlsl', 'w').write(ms)
    if ps is not None:
        open(name + '.ps.hlsl', 'w').write(ps)
    if check:
        # The pixel stage is compiled with dxc too: a pipeline with a DXIL mesh stage takes DXIL stages only.
        for stage, profile, present in (('ms', 'ms_6_5', True), ('ps', 'ps_6_0', ps is not None)):
            if not present:
                continue
            code, err = compile_hlsl(name + '.%s.hlsl' % stage, profile, name + '.%s.cso' % stage)
            if code:
                open(name + '.fail', 'w').write('dxc %s: %s\n' % (stage, err[-2000:]))
                return 'dxc %s failed: %s' % (stage, err.strip().splitlines()[0] if err.strip() else '?')
    temporary = '%s.txt.%d.tmp' % (name, os.getpid())
    with open(temporary, 'w') as result:
        result.write(manifest)
    os.replace(temporary, name + '.txt')
    return 'ok'


def is_mesh(code):
    """NGG mesh stage: it sends GS_ALLOC_REQ and exports a primitive."""
    text = [x[3] for x in G.decode(G.strip_header(code))]
    return any(t == 's_sendmsg 0x9' for t in text) and any(re.match(r'exp (prim|target_20)', t) for t in text)


def translate_request(out_dir, key):
    """A mesh pipeline request req_<key>.{a,b,root}: whichever blob is the mesh stage, the other the pixel stage."""
    base = os.path.join(out_dir, 'req_%016x' % key)
    try:
        a, b, root = (open(base + ext, 'rb').read() for ext in ('.a', '.b', '.root'))
    except OSError as error:
        return 'unreadable: %s' % error
    if not a and b and is_mesh(b):
        return translate_to(out_dir, key, b, None, root)   # depth-only mesh pipeline (no pixel stage)
    if a and b and is_mesh(b) and not is_mesh(a):
        return translate_to(out_dir, key, b, a, root)
    if a and b and is_mesh(a) and not is_mesh(b):
        return translate_to(out_dir, key, a, b, root)
    return 'not a mesh pipeline'


# ---- root signature synthesis for coverage runs over the archive (no real root blob there) ----------------------
def synth_root(*codes):
    """A root signature blob whose SGPR layout from s8 matches how the programs (the stages of one pipeline, which
    share the layout) use their entry SGPRs: a register read as a 32-bit table pointer (with a zero high word)
    becomes a descriptor table, a pair used as a 64-bit base a root SRV, everything else 32-bit constants.
    COVERAGE ONLY: the title's real root signature comes with the request."""
    tables, pairs, used = set(), set(), set()
    for code in codes:
        if code:
            synth_scan(code, tables, pairs, used)
    return synth_layout(tables, pairs, used)


def synth_scan(code, tables, pairs, used):
    ins = G.decode(G.strip_header(code))
    origin = {}          # current register -> entry register it holds
    zero = set()
    for r in range(8, 64):
        origin[r] = r
    for off, n, name, asm in ins:
        _, ops, mods = parse(asm)
        base = re.sub(r'_e(32|64)$', '', name)
        for t in ops[1:]:
            for r in scalar_regs(t):
                if r in origin:
                    used.add(origin[r])
        if base.startswith(('s_load', 's_buffer_load', 'buffer_', 'tbuffer_', 'image_')):
            sb = ops[1] if base.startswith(('s_load', 's_buffer_load')) else (ops[2] if len(ops) > 2 else '')
            rr = reg_range(sb)
            if rr and rr[0] == 's':
                lo = origin.get(rr[1])
                if lo is not None and (rr[1] + 1) in zero and base.startswith('s_load'):
                    tables.add(lo)
                elif lo is not None and origin.get(rr[1] + 1) == lo + 1:
                    pairs.add(lo)
        dst = reg_range(ops[0]) if ops else None
        if dst and dst[0] == 's' and base.startswith('s_') and not base.startswith(NO_DEST):
            src = reg_range(ops[1]) if len(ops) > 1 else None
            if base == 's_bitset1_b32':
                continue  # sets one bit in place (descriptor word 1 from a pointer's high word): same origin
            for r in range(dst[1], dst[2] + 1):
                origin.pop(r, None)
                zero.discard(r)
            if base == 's_mov_b32' and src and src[0] == 's' and src[1] in origin:
                origin[dst[1]] = origin[src[1]]
            elif base in ('s_or_b32', 's_bitset1_b32') and src and src[0] == 's' and src[1] in origin:
                origin[dst[1]] = origin[src[1]]   # descriptor word 1 built from a pointer's high word
            elif base == 's_mov_b32' and ops[1] == '0':
                zero.add(dst[1])


def synth_layout(tables, pairs, used):
    params, r = [], 8
    top = max(used | {7}) + 1
    while r < top:
        if r in tables:
            params.append((0, None))
            r += 1
        elif r in pairs:
            params.append((3, None))
            r += 2
        else:
            n = 1
            while r + n < top and r + n not in tables and r + n not in pairs:
                n += 1
            params.append((1, n))
            r += n
    return root_blob(params)


def root_blob(params, flags=0):
    """DXBC container with an RTS0 part (version 2) for params [(type, count)]: type 0 table (no ranges), 1 constants,
    3 root SRV."""
    head = 24
    body = bytearray()
    extra = bytearray()
    poff = head
    eoff = head + 12 * len(params)
    for i, (t, count) in enumerate(params):
        if t == 1:
            body += struct.pack('<3I', t, 0, eoff + len(extra))
            extra += struct.pack('<3I', i, 0, count)
        elif t == 0:
            body += struct.pack('<3I', t, 0, eoff + len(extra))
            extra += struct.pack('<2I', 0, eoff + len(extra) + 8)
        else:
            body += struct.pack('<3I', t, 0, eoff + len(extra))
            extra += struct.pack('<3I', i, 0, 0)
    rts = struct.pack('<6I', 2, len(params), poff, 0, eoff + len(extra), flags) + bytes(body) + bytes(extra)
    part = b'RTS0' + struct.pack('<I', len(rts)) + rts
    total = 32 + 4 + len(part)
    return b'DXBC' + b'\0' * 16 + struct.pack('<III', 1, total, 1) + struct.pack('<I', 36) + part


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--requests':
        out_dir = sys.argv[2]
        for name in sorted(os.listdir(out_dir)):
            m = re.fullmatch(r'req_([0-9a-f]{16})\.a', name)
            if not m or os.path.exists(os.path.join(out_dir, '%s.txt' % m.group(1))):
                continue
            result = translate_request(out_dir, int(m.group(1), 16))
            if result != 'not a mesh pipeline':
                print('%s: %s' % (m.group(1), result))
        return
    if len(sys.argv) >= 4 and sys.argv[1] == '--ms-only':
        # --ms-only <out_dir> <ms.bin> [...]: the mesh stage alone with a synthesized root (coverage)
        out_dir = sys.argv[2]
        os.makedirs(out_dir, exist_ok=True)
        for i, path in enumerate(sys.argv[3:]):
            code = open(path, 'rb').read()
            print('%s: %s' % (os.path.basename(path), translate_to(out_dir, i, code, None, synth_root(code))))
        return
    print(__doc__)


if __name__ == '__main__':
    main()
