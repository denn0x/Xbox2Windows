"""gfx1013 machine code -> HLSL for a vertex+pixel pipeline (startup tools; see docs/startup-runtime.md).

Model: one lane. Registers are uint variables; exec/vcc/mask registers hold the lane's bit in bit 0.
Scalar registers are also tracked symbolically at translation time so that pointer and descriptor
values (tables, vertex-buffer table, root buffers) turn into HLSL resources instead of arithmetic.
The output is HLSL for both stages plus a manifest that tells the runtime what to bind where.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import disasm_rdna2 as D  # noqa: E402


class Unsupported(Exception):
    pass


class StoredSrv(Unsupported):
    """A descriptor first read as an SRV and later stored to; translate_to retries with it in rw_keys."""

    def __init__(self, key, message):
        super().__init__(message)
        self.key = key


PREFETCH = bytes.fromhex('0300a0bf')

# SDWA (sub-dword addressing) selections, with the same meaning as tools/pixel_aot.py: a source
# selection is zero-extended, or sign-extended under sext(); a destination selection writes the low
# bits of the result into that field, UNUSED_PAD zeroes the others and UNUSED_PRESERVE keeps them.
SDWA_SEL = {'BYTE_0': (0, 8), 'BYTE_1': (8, 8), 'BYTE_2': (16, 8), 'BYTE_3': (24, 8),
            'WORD_0': (0, 16), 'WORD_1': (16, 16), 'DWORD': (0, 32)}


def sdwa_source(word, sel, sext):
    """The uint expression a source selection reads out of a 32-bit register."""
    offset, width = SDWA_SEL[sel]
    if width == 32:
        return word
    if sext:
        return 'asuint(asint(%s << %du) >> %du)' % (word, 32 - offset - width, 32 - width)
    return '((%s >> %du) & %#xu)' % (word, offset, (1 << width) - 1)


def sdwa_dest(result, sel, unused, old):
    """The uint expression a destination selection writes back into a 32-bit register."""
    offset, width = SDWA_SEL[sel]
    if width == 32:
        return result
    if unused == 'UNUSED_SEXT':
        raise Unsupported('SDWA UNUSED_SEXT')
    field = '((%s & %#xu) << %du)' % (result, (1 << width) - 1, offset)
    if unused == 'UNUSED_PAD':
        return field
    if unused != 'UNUSED_PRESERVE':
        raise Unsupported('SDWA dst_unused %s' % unused)
    keep = ~(((1 << width) - 1) << offset) & 0xffffffff
    return '((asuint(%s) & %#xu) | %s)' % (old, keep, field)


def strip_header(code):
    at = code.find(PREFETCH, 0, 0x80)
    return code[at:] if at >= 0 and at % 4 == 0 else code


def decode(code):
    out, off = [], 0
    while off + 4 <= len(code):
        try:
            n, name, asm, _ = D.decode_instruction(code, off)
        except Exception as error:
            raise Unsupported('undecodable word at +0x%x: %s' % (off, error))
        if name == 'image_load_mip':
            # The shared disassembler does not expose these gfx10 MIMG flags.
            word0, word1 = struct.unpack_from('<2I', code, off)
            if word0 & (1 << 16):
                raise Unsupported('image_load_mip tfe')
            if word1 & (1 << 30):
                raise Unsupported('image_load_mip a16')
        out.append((off, n, name, asm))
        off += n
    last = max((i for i, x in enumerate(out) if x[2] == 's_endpgm'), default=len(out) - 1)
    return out[:last + 1]


def split_top(text, sep=','):
    parts, depth, cur = [], 0, ''
    for ch in text:
        if ch in '[(':
            depth += 1
        elif ch in '])':
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def parse(asm):
    """-> (mnemonic, [operands], [modifier tokens])"""
    name, _, rest = asm.partition(' ')
    ops = split_top(rest)
    mods = []
    if ops:
        tail = split_top(ops[-1], ' ')
        ops[-1], mods = tail[0], tail[1:]
    return name, ops, mods


FLOATS = {'0.5': 0.5, '-0.5': -0.5, '1.0': 1.0, '-1.0': -1.0, '2.0': 2.0, '-2.0': -2.0, '4.0': 4.0, '-4.0': -4.0,
          '0.15915494': 0.15915494}


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reg_range(tok):
    """'s[8:11]' -> ('s', 8, 11); 's8' -> ('s', 8, 8); 'v[1:3]' -> ('v', 1, 3); 'exec'/'vcc' pairs too."""
    m = re.fullmatch(r'([sv])\[(\d+):(\d+)\]', tok)
    if m:
        return m.group(1), int(m.group(2)), int(m.group(3))
    m = re.fullmatch(r'([sv])(\d+)', tok)
    if m:
        return m.group(1), int(m.group(2)), int(m.group(2))
    return None


SAMPLER_CLAMP = {0: 'WRAP', 1: 'MIRROR', 2: 'CLAMP', 3: 'MIRROR_ONCE', 4: 'CLAMP', 5: 'MIRROR_ONCE', 6: 'BORDER', 7: 'BORDER'}


def smem_offset(tok):
    """The SMEM offset operand as a token: 'off' (no offset operand) is the literal 0, anything else unchanged."""
    return '0' if tok == 'off' else tok


class Stage:
    """Translates one stage. Shares the resource and entry-constant registries of its pipeline."""
    uses_q = False
    uses_swap = False

    def __init__(self, pipe, kind, code):
        self.pipe, self.kind = pipe, kind
        self.raw = strip_header(code)  # instruction words, for encoding bits the decoder does not print
        self.ins = decode(self.raw)
        self.lines = []           # body statements (per block)
        self.sym = {}             # scalar register -> symbolic value
        self.used_s, self.used_v = set(), set()
        self.exec_known = True    # exec still equals the entry mask (the lane is active)
        self.exports = {}         # 'pos0' / 'paramN' / 'mrtN' -> [4 expressions]
        self.attrs = {}           # pixel: attr index -> 'linear' | 'flat' | 'half'
        self.uses_pos = False
        self.masks = set()        # scalar registers currently holding lane masks
        self.uses_q = False       # the uint2 temporary of the 64-bit operations
        self.used_h = set()       # heap descriptor offsets h<k> this stage assigns (manifest `heap 1`)
        self.written_v = set()    # VGPRs assigned so far in program order
        self.entry_reads = {}     # VGPR read before any write (its entry value) -> offset of the first such read
        self.pc = 0
        self.setup_entry()
        self.translate()

    # ---- entry state ---------------------------------------------------------------------
    def setup_entry(self):
        root = self.pipe.root
        reg = 8
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
        if self.kind == 'vs':
            self.sym[0] = ('ptr', ('vbtab',))
            self.sym[1] = ('ptrhi', ('vbtab',))
            self.sym[2] = ('ud', self.pipe.ud(('basevertex',)))
            self.sym[3] = ('ud', self.pipe.ud(('startinstance',)))
        else:
            self.sym[reg] = ('const', 0)  # primitive mask: only ever moved to m0

    # ---- operand helpers ---------------------------------------------------------------------
    def S(self, n):
        """Runtime expression of scalar register n (uint)."""
        s = self.sym.get(n)
        if s:
            if s[0] == 'const':
                return '0x%08xu' % (s[1] & 0xffffffff)
            if s[0] == 'ud':
                return 'ud%d' % s[1]
            if s[0] in ('execcopy', 'fixed_buffer_word'):
                self.used_s.add(n)
                return 's%d' % n
            raise Unsupported('numeric use of s%d holding %s' % (n, s[0]))
        self.used_s.add(n)
        return 's%d' % n

    def u(self, tok):
        """uint expression for a 32-bit operand."""
        if tok in ('vcc_lo', 'vcc'):
            return 'vcc'
        if tok in ('exec_lo', 'exec'):
            return 'exec'
        if tok == 'm0':
            return 'm0'
        if tok == 'src_scc':
            # SCC read as a 32-bit operand (0 or 1)
            return '(scc ? 1u : 0u)'
        if tok in getattr(self, 'dpp_names', ()):
            return tok  # the quad-read source of a DPP instruction (quad_dpp)
        r = reg_range(tok)
        if r:
            if r[0] == 'v':
                written = getattr(self, 'written_v', None)  # absent on the tests' bare one-instruction stages
                if written is not None and r[1] not in written:
                    self.entry_reads.setdefault(r[1], self.pc)
                self.used_v.add(r[1])
                return 'v%d' % r[1]
            return self.S(r[1])
        if tok in FLOATS:
            return '0x%08xu' % fbits(FLOATS[tok])
        try:
            return '0x%08xu' % (int(tok, 0) & 0xffffffff)
        except ValueError:
            raise Unsupported('operand %r' % tok)

    def f(self, tok):
        """float expression, with the inline neg/abs operand modifiers."""
        neg = False
        if tok.startswith('-') and not re.match(r'-[\d.]', tok):
            neg, tok = True, tok[1:]
        absolute = tok.startswith('|') and tok.endswith('|')
        if absolute:
            tok = tok[1:-1]
        if tok in FLOATS:
            text = repr(FLOATS[tok])
        elif tok == '-0':
            text = '0.0'
        else:
            text = 'F(%s)' % self.u(tok)
        if absolute:
            text = 'abs(%s)' % text
        return '(-%s)' % text if neg else text

    def u_mod(self, tok, sign=0x80000000):
        """uint expression for an operand with VOP3 neg/abs applied as sign-bit operations (no float rounding)."""
        neg = tok.startswith('-') and not re.match(r'-[\d.]', tok)
        if neg:
            tok = tok[1:]
        absolute = tok.startswith('|') and tok.endswith('|')
        if absolute:
            tok = tok[1:-1]
        text = self.u(tok)
        if absolute:
            text = '(%s & %#xu)' % (text, ~sign & 0xffffffff)
        if neg:
            text = '(%s ^ %#xu)' % (text, sign)
        return text

    def h(self, tok):
        """uint expression whose low 16 bits are a binary16 operand: inline float constants take their half form."""
        if tok in FLOATS:
            return '0x%04xu' % struct.unpack('<H', struct.pack('<e', FLOATS[tok]))[0]
        return self.u(tok)

    def is_const(self, n):
        s = self.sym.get(n)
        return s[1] if s and s[0] == 'const' else None

    def emit(self, text):
        # The address temporary `a` is lane-invariant while its last assignment was an unconditional one from a
        # lane-invariant expression (a buffer load with no VGPR address, or one computed from uniform values).
        if hasattr(self, 'lane_s') and re.search(r'\ba = ', text):
            m = re.fullmatch(r'a = (.*);', text)
            self.a_invariant = bool(m) and self.lane_invariant(m.group(1))
        if re.search(r'\bexec = ', text):
            self.exec_writes = getattr(self, 'exec_writes', 0) + 1
        self.lines.append(text)

    def lane_invariant(self, expr):
        """True when the HLSL expression has the same value in every lane of the wave.

        It may name only entry constants (udN), resources (rN, read at any address the expression itself
        computes), pure functions and literals, SGPR variables that hold a scalar number (not a lane mask: the
        one-lane model keeps only the lane's bit of a mask) and VGPRs proven lane-invariant. vcc, exec, scc, m0,
        the temporaries (a, w, t, q) and the interpolants (i.) depend on the lane or on state not tracked here.
        """
        for m in re.finditer(r'\b[A-Za-z_]\w*', expr):
            name = m.group(0)
            if name in LANE_INVARIANT_NAMES or name.startswith('x2w_'):
                continue
            if name == 'a' and getattr(self, 'a_invariant', False):
                continue
            if re.fullmatch(r'xv\d+', name):  # assigned in every lane from a lane-invariant value (readfirstlane)
                continue
            reg = re.fullmatch(r'(ud|r|s|v)(\d+)', name)
            if reg and reg.group(1) in ('ud', 'r'):
                continue
            if reg and reg.group(1) == 's' and int(reg.group(2)) not in self.lane_s and int(reg.group(2)) not in self.masks:
                continue
            if reg and reg.group(1) == 'v' and int(reg.group(2)) in self.uniform_v:
                continue
            return False
        return True

    def set_v(self, n, expr):
        if hasattr(self, 'written_v'):
            self.written_v.add(n)
        # A VGPR written in every lane (exec still the entry mask) from lane-invariant values holds one value in
        # all of them, so a later v_readfirstlane_b32 reads that value whatever exec is then.
        invariant = self.exec_known and hasattr(self, 'lane_s') and self.lane_invariant(expr)
        self.uniform_v.discard(n)
        if invariant:
            self.uniform_v.add(n)
        partial = getattr(self, 'partial_uniform', None)
        if partial is not None:
            partial.pop(n, None)
            partial_ok = not self.exec_known and self.lane_invariant(expr)
        self.used_v.add(n)
        self.emit('v%d = %s;' % (n, expr) if self.exec_known else 'if (exec & 1) v%d = %s;' % (n, expr))
        if partial is not None and partial_ok:
            # Written in the active lanes only, from a lane-invariant value: every lane active now holds that value.
            # Kept with the emitted line so v_readfirstlane_b32 can take the value for every lane (see there).
            partial[n] = (self.lines, len(self.lines) - 1, expr, getattr(self, 'exec_writes', 0))

    def set_vf(self, n, expr, mods=()):
        # The hardware applies the output modifier before the clamp, but the disassembler prints `clamp`
        # first, so the order of `mods` is not the order of the operations: scale first, saturate last.
        scale = 1.0
        clamp = False
        for m in mods:
            if m == 'clamp':
                clamp = True
            elif m in ('mul:2', 'mul:4'):
                scale *= float(m[-1])
            elif m == 'div:2':
                scale *= 0.5
            elif m.startswith('op_sel'):
                # op_sel selects which half of a packed source the operation reads. This path does not
                # model that, so dropping it would change the operand: refuse instead of ignoring it.
                raise Unsupported('modifier %s' % m)
            else:
                raise Unsupported('modifier %s' % m)
        if scale != 1.0:
            factor = ('%d.0' % int(scale)) if scale == int(scale) else ('%g' % scale)
            expr = '(%s) * %s' % (expr, factor)
        if clamp:
            expr = 'saturate(%s)' % expr
        self.set_v(n, 'U(%s)' % expr)

    def set_s(self, n, expr, sym=None, scc=None):
        """Assign scalar register n at run time and/or symbolically.

        Any write ends n's life as a lane mask; a caller that writes a mask marks it again after this call."""
        getattr(self, 'masks', set()).discard(n)  # (the tests' bare stages have no mask set)
        if sym is not None:
            self.sym[n] = sym
            if sym[0] != 'const':
                return
        else:
            self.sym.pop(n, None)
        if hasattr(self, 'lane_s'):
            if self.lane_invariant(expr):
                self.lane_s.discard(n)
            else:
                self.lane_s.add(n)
            self.s_written_at.setdefault(self.pc, set()).add(n)
        self.used_s.add(n)
        self.emit('s%d = %s;' % (n, expr))
        if scc:
            self.emit('scc = %s;' % scc.replace('$', 's%d' % n))

    def descriptor(self, tok):
        """Resource key for a descriptor operand like s[8:11]: (source, byte offset or None)."""
        r = reg_range(tok)
        s = self.sym.get(r[1]) if r else None
        if s and s[0] == 'desc':
            # A descriptor can start inside a wider load (s_load_dwordx16 fetches two image
            # descriptors): its offset is the load's offset plus the first register's dword index.
            return s[1], (None if s[2] is None else s[2] + 4 * s[3])
        if s and s[0] == 'ptr':
            return s[1], None
        raise Unsupported('descriptor operand %s holds %s' % (tok, s))

    def heap_sampler(self, tok):
        """'qh[...]' when the four sampler words of tok came from one run-time sampler heap load."""
        r = reg_range(tok)
        if not r or r[2] - r[1] != 3:
            return None
        first = self.sym.get(r[1])
        if not first or first[0] != 'smpheap':
            return None
        if any(self.sym.get(r[1] + i) != ('smpheap', first[1], i) for i in range(4)):
            raise Unsupported('sampler operand %s mixes sampler heap words' % tok)
        self.pipe.uses_sampler_heap = True
        # Sampler descriptors are 16 bytes; x2w_sampler_entry() maps the byte offset to its entry and
        # sends unaligned or out-of-range offsets to the fallback entry.
        return 'qh[x2w_sampler_entry(s%d)]' % first[1]

    def const_words(self, tok):
        r = reg_range(tok)
        words = [self.is_const(n) for n in range(r[1], r[2] + 1)]
        return None if any(w is None for w in words) else words

    # ---- instruction translation -------------------------------------------------------------
    # Symbolic values that are also assigned to their HLSL variable, so a path where the register holds one of them
    # and a path where it holds a plain run-time number agree on the variable's value.
    RUNTIME_BACKED = ('const', 'fixed_buffer_word', 'execcopy')

    def snapshot(self):
        return dict(self.sym), set(self.masks), self.exec_known, set(self.lane_s)

    def merge(self, states):
        """The symbolic state at a join of forward edges: what every incoming path agrees on.

        A register whose paths disagree keeps working as a number when every path also assigned it at run time;
        otherwise it becomes ('conflict',), which any later use refuses (a descriptor on one path and a loaded
        word on the other has no single translation). A mask on any path stays a mask, and exec is the entry
        mask only when it is on every path.
        """
        sym = {}
        for reg in set().union(*(s[0] for s in states)):
            values = [s[0].get(reg) for s in states]
            if all(v == values[0] for v in values):
                sym[reg] = values[0]
            elif not all(v is None or v[0] in self.RUNTIME_BACKED for v in values):
                sym[reg] = ('conflict',)
        self.sym = sym
        self.masks = set().union(*(s[1] for s in states))
        self.exec_known = all(s[2] for s in states)
        self.lane_s = set().union(*(s[3] for s in states))  # lane-dependent on any incoming path

    def translate(self):
        targets, sources = set(), {}
        for off, n, name, asm in self.ins:
            if name.startswith('s_cbranch') or name == 's_branch':
                target = int(parse(asm)[1][0].split('_')[1], 16)
                targets.add(target)
                sources.setdefault(target, []).append(off)
        self.blocks = []          # (offset, [lines], terminator)
        self.uniform_v = set()    # VGPRs proven to hold one value in every lane (see set_v)
        self.partial_uniform = {}  # VGPRs holding one value in every lane active at their write (see set_v)
        self.shadow_v = set()     # shadow variables xvN that v_readfirstlane_b32 introduced
        self.lane_s = set()       # SGPR variables that may differ between lanes in the one-lane model (see set_s)
        self.s_written_at = {}    # instruction offset -> SGPRs it assigns at run time
        self.has_branches = bool(targets)
        start = 0
        incoming = {}             # branch target -> symbolic states of the forward branches seen so far
        previous = None
        scc_written, scc_unknown = set(), set()
        for off, n, name, asm in self.ins:
            if off in targets and off != start:
                self.blocks.append((start, self.lines, None))
                self.lines, start = [], off
            # The scan is linear, so at a join the state left by the instruction above is only one of the
            # incoming paths (and none after an unconditional branch). Merge the states of every incoming
            # edge when they are all forward; a loop head keeps the linear state.
            if off in targets and all(src < off for src in sources[off]):
                states = list(incoming.get(off, []))
                if previous not in (None, 's_branch', 's_endpgm'):
                    states.append(self.snapshot())
                if states:
                    self.merge(states)
            if off in targets:
                # Lane invariance is not merged across edges for VGPRs; at a loop head the linear state misses the
                # back edges, so every SGPR becomes suspect until it is written again.
                self.uniform_v.clear()
                self.partial_uniform.clear()
                self.a_invariant = False
                if not all(src < off for src in sources[off]):
                    self.lane_s = set(range(128))
                else:
                    # In the one-lane model a branch condition (scc, vccz, execz) may differ between lanes, so an
                    # SGPR assigned on only some paths into this join may too, however invariant each value is.
                    first = min(sources[off])
                    for at, regs in self.s_written_at.items():
                        if first <= at < off:
                            self.lane_s |= regs
            self.pc = off
            before = len(self.lines)
            term = self.one(name, asm)
            if any('scc = ' in line for line in self.lines[before:]):
                scc_written.add(off)
            elif re.sub(r'_e(32|64)$', '', name) in SCC_UNMODELLED or (name in SCA and SCA[name][1]):
                # a hardware SCC write the HLSL did not make (or, for an SCA op with an SCC rule, one that a
                # descriptor or m0 path skipped)
                scc_unknown.add(off)
            previous = name
            if name.startswith('s_cbranch') or name == 's_branch':
                incoming.setdefault(int(parse(asm)[1][0].split('_')[1], 16), []).append(self.snapshot())
            if term:
                self.blocks.append((start, self.lines, term))
                self.lines, start = [], off + n
        if self.lines:
            self.blocks.append((start, self.lines, None))
        self.check_scc(scc_written, scc_unknown)
        self.check_quad_ops()

    def check_scc(self, written, unknown):
        """Refuse when an SCC value the translation does not compute can be read.

        The instructions in `unknown` write SCC in hardware but the HLSL leaves `scc` as it was. That is only
        a wrong translation if some path from one of them reaches an SCC reader before an instruction whose
        translation assigns `scc` (one in `written`). Paths follow fall-through and branch targets, loops
        included; s_endpgm and s_branch end the fall-through.
        """
        if not unknown:
            return
        order = [ins[0] for ins in self.ins]
        info = {off: (name, asm) for off, n, name, asm in self.ins}
        following = dict(zip(order, order[1:]))
        def successors(off):
            name, asm = info[off]
            if name == 's_endpgm':
                return []
            nxt = [following[off]] if off in following else []
            if name == 's_branch':
                nxt = []
            if name.startswith('s_cbranch') or name == 's_branch':
                nxt.append(int(parse(asm)[1][0].split('_')[1], 16))
            return [o for o in nxt if o in info]
        for start in sorted(unknown):
            seen, todo = set(), list(successors(start))
            while todo:
                off = todo.pop()
                if off in seen:
                    continue
                seen.add(off)
                name, asm = info[off]
                if reads_scc(name, asm):
                    raise Unsupported('SCC of %s at %#x (not modelled) is read by %s at %#x'
                                      % (info[start][0], start, name, off))
                if off in written or off in unknown:
                    continue
                todo.extend(successors(off))

    def one(self, name, asm):
        _, ops, mods = parse(asm)
        base = re.sub(r'_e(32|64)$', '', name)
        if base in ('s_inst_prefetch', 's_waitcnt', 's_nop', 'v_nop', 's_clause', 's_sopp_0f', 's_wqm_b64', 's_wqm_b32',
                    's_setreg_imm32_b32', 's_code_end', 's_setprio'):
            return None
        if base == 's_endpgm':
            return ('end',)
        if base == 's_branch':
            return ('jump', int(ops[0].split('_')[1], 16))
        if base.startswith('s_cbranch_'):
            cond = {'scc0': '!scc', 'scc1': 'scc', 'vccz': '(vcc & 1) == 0', 'vccnz': '(vcc & 1) != 0',
                    'execz': '(exec & 1) == 0', 'execnz': '(exec & 1) != 0'}[base[10:]]
            return ('branch', cond, int(ops[0].split('_')[1], 16))
        if base == 'exp':
            return self.export(asm)
        if base.endswith('_dpp'):
            return self.quad_dpp(base[:-len('_dpp')], ops, mods)
        handler = getattr(self, 'op_' + base, None)
        if handler:
            return handler(ops, mods)
        if base.startswith('v_cmp'):
            return self.compare(base, ops, mods)
        if base.endswith('_sdwa'):
            return self.sdwa(base[:-len('_sdwa')], ops, mods)
        if base in VEC_F:
            arity, text = VEC_F[base]
            # A missing source used to raise IndexError and an extra one was dropped without a word.
            if len(ops) != arity + 1:
                raise Unsupported('%s with %d operands, expected %d' % (base, len(ops), arity + 1))
            args = [self.f(o) for o in ops[1:1 + arity]]
            if base in ('v_fmac_f32', 'v_mac_f32'):
                args.append(self.f(ops[0]))
            return self.set_vf(reg_range(ops[0])[1], text.format(*args), mods)
        if base in VEC_U_TO_F:
            arity, text_f = VEC_U_TO_F[base]
            if len(ops) != arity + 1:
                raise Unsupported('%s with %d operands, expected %d' % (base, len(ops), arity + 1))
            # Only the output modifiers a conversion can carry; op_sel and the source modifiers are not
            # accepted here, because ignoring them would silently change the operand.
            if any(m not in ('clamp', 'mul:2', 'mul:4', 'div:2') for m in mods):
                raise Unsupported('%s with %s' % (base, mods))
            return self.set_vf(reg_range(ops[0])[1],
                               text_f.format(*[self.u(o) for o in ops[1:1 + arity]]), mods)
        if base in VEC_U:
            arity, text = VEC_U[base]
            if len(ops) != arity + 1:
                raise Unsupported('%s with %d operands, expected %d' % (base, len(ops), arity + 1))
            result = text.format(*[self.u(o) for o in ops[1:1 + arity]])
            if mods:
                # VOP3 output modifiers (omod, clamp) act on a binary32 result, so they are only taken on the
                # integer-source conversions that produce one; set_vf applies omod and then the clamp.
                if base not in FLOAT_RESULT_U or any(m not in ('clamp', 'mul:2', 'mul:4', 'div:2') for m in mods):
                    raise Unsupported('%s with %s' % (base, mods))
                return self.set_vf(reg_range(ops[0])[1], 'F(%s)' % result, mods)
            return self.set_v(reg_range(ops[0])[1], result)
        if base in SCA:
            return self.scalar(base, ops)
        raise Unsupported(base)

    # DPP16 quad permutations in a pixel stage: src0 is read from lane perm[lane & 3] of the lane's 2x2 quad, the
    # rest of the instruction is the plain VOP1/VOP2 operation. LLVM AMDGPUModifierSyntax.md: quad_perm is a "full
    # permute of 4 threads"; fi:1 "fetch pre-existing values from inactive lanes"; bound_ctrl only acts on invalid
    # lanes, which a quad permutation never names. Quad lane 1 is +x and lane 2 is +y (Mesa ACO
    # aco_select_nir_intrinsics.cpp: ddx_coarse = v_subrev_f32 with quad_perm(0,0,0,0) then v_mov_b32
    # quad_perm(1,1,1,1), ddy_coarse with (2,2,2,2)), the order of D3D QuadReadLaneAt (0 top-left, 1 top-right,
    # 2 bottom-left, 3 bottom-right). The HLSL quad reads need SM 6.0 (the mesh pipelines' dxc ps_6_0, which sets
    # pipe.quad_ops; draws compile with fxc ps_5_1 and refuse), and every lane of the quad must execute them:
    # each read is its own statement outside the exec guard, and check_quad_ops refuses one that a lane can skip.
    QUAD_READS = {(1, 0, 3, 2): 'QuadReadAcrossX({0})', (2, 3, 0, 1): 'QuadReadAcrossY({0})',
                  (3, 2, 1, 0): 'QuadReadAcrossDiagonal({0})'}

    def quad_dpp(self, base, ops, mods):
        if self.kind != 'ps' or not getattr(self.pipe, 'quad_ops', False):
            raise Unsupported('%s_dpp' % base)  # as before: no lane reads outside an SM 6.0 pixel stage
        control = mods[0] if mods else 'no control'
        m = re.fullmatch(r'quad_perm:\[([0-3]),([0-3]),([0-3]),([0-3])\]', control)
        if not m:
            raise Unsupported('%s_dpp with %s (not a quad permutation)' % (base, control))
        rest = set(mods[1:])
        if len(rest) != len(mods) - 1 or not {'row_mask:0xf', 'bank_mask:0xf', 'fi:1'} <= rest \
                or rest - {'row_mask:0xf', 'bank_mask:0xf', 'fi:1', 'bound_ctrl:1'}:
            raise Unsupported('%s_dpp with %s' % (base, ' '.join(mods[1:])))
        perm = tuple(int(x) for x in m.groups())
        src = re.fullmatch(r'(-?\|?)(v\d+)(\|?)', ops[1]) if len(ops) > 1 else None
        if not src:
            raise Unsupported('%s_dpp source %s' % (base, ops[1] if len(ops) > 1 else None))
        value = self.u(src.group(2))
        if len(set(perm)) == 1:
            expr = 'QuadReadLaneAt(%s, %du)' % (value, perm[0])
        elif perm in self.QUAD_READS:
            expr = self.QUAD_READS[perm].format(value)
        else:
            raise Unsupported('%s_dpp with %s' % (base, control))
        names = self.__dict__.setdefault('dpp_names', [])
        name = 'dpp%d' % len(names)
        names.append(name)
        self.__dict__.setdefault('dpp_at', []).append(self.pc)
        self.emit('uint %s = %s;' % (name, expr))
        return self.one(base, '%s %s' % (base, ', '.join([ops[0], src.group(1) + name + src.group(3)] + ops[2:])))

    def check_quad_ops(self):
        """Refuse a quad read (quad_dpp) that some lane of the HLSL may not execute: the program has a loop (the
        dispatch loop lets lanes reach a block in different iterations), or a path from the entry to the end
        avoids the instruction (a per-lane branch around it)."""
        at = getattr(self, 'dpp_at', ())
        if not at:
            return
        info = {off: (name, asm) for off, n, name, asm in self.ins}
        order = [ins[0] for ins in self.ins]
        following = dict(zip(order, order[1:]))
        def successors(off):
            name, asm = info[off]
            if name == 's_endpgm':
                return []
            nxt = [] if name == 's_branch' else [following[off]] if off in following else []
            if name.startswith('s_cbranch') or name == 's_branch':
                nxt.append(int(parse(asm)[1][0].split('_')[1], 16))
            return [o for o in nxt if o in info]
        for off in order:
            if any(t <= off for t in successors(off) if t != following.get(off)):
                raise Unsupported('quad read (DPP) at %#x in a program with a loop (branch at %#x)' % (at[0], off))
        for target in at:
            seen, todo = set(), [order[0]]
            while todo:
                off = todo.pop()
                if off in seen or off == target:
                    continue
                seen.add(off)
                nxt = successors(off)
                if not nxt:
                    raise Unsupported('quad read (DPP) at %#x is skipped on a path to %#x' % (target, off))
                todo.extend(nxt)

    # VOP1/VOP2 with sub-dword addressing: select the source fields, run the same operation, then place
    # the low bits of the result in the destination field. The integer forms reuse VEC_U unchanged;
    # float forms are not lowered yet, and dst_unused UNUSED_SEXT has no lowering.
    def sdwa(self, base, ops, mods):
        fields = {}
        outmods = []
        for m in mods:
            key, _, value = m.partition(':')
            if key in ('src0_sel', 'src1_sel', 'src2_sel', 'dst_sel', 'dst_unused') and value:
                fields[key] = value
            elif m in ('clamp', 'mul:2', 'mul:4', 'div:2') and base in VEC_U_TO_F:
                # The SDWA9 encoding (gfx9+, LLVM VOPInstructions.td VOP_SDWA9 has omod and clamp for VOP1/VOP2)
                # carries the output modifiers; on a float-result conversion they act on the binary32 result as
                # in VOP3. Taken only with dst_sel:DWORD (below), where no field placement follows them.
                outmods.append(m)
            else:
                raise Unsupported('%s with SDWA modifier %s' % (base, m))
        if base == 'v_cndmask_b32':
            # VOP2 SDWA select: the mask is always vcc; the selected source fields are moved as bits.
            if len(ops) != 4 or ops[3] not in ('vcc', 'vcc_lo'):
                raise Unsupported('SDWA v_cndmask_b32 form %s' % ops)
            arity, text = 2, '(vcc & 1) ? {1} : {0}'
        elif base == 'v_mov_b32':
            # VOP1 SDWA move (LLVM VOP1Instructions.td V_MOV_B32 has an SDWA form): the selected source field,
            # placed by dst_sel / dst_unused like every integer SDWA result.
            arity, text = 1, '{0}'
        elif base in VEC_U_TO_F:
            # The float-result conversions moved out of VEC_U (#1707) keep their SDWA form: the same expression
            # as the bits, as VEC_U had it ('U(float({0}))' and so on). No output modifier is accepted above.
            arity, text_f = VEC_U_TO_F[base]
            text = 'U(%s)' % text_f
        elif base not in VEC_U:
            raise Unsupported('SDWA form of %s' % base)
        else:
            arity, text = VEC_U[base]
        sources = []
        for index in range(arity):
            token = ops[1 + index]
            sext = token.startswith('sext(') and token.endswith(')')
            if sext:
                token = token[len('sext('):-1]
            selection = fields.get('src%d_sel' % index)
            if not selection:
                raise Unsupported('%s SDWA without src%d_sel' % (base, index))
            sources.append(sdwa_source(self.u(token), selection, sext))
        result = text.format(*sources)
        select = fields.get('dst_sel')
        if not select:
            raise Unsupported('%s SDWA without dst_sel' % base)
        if outmods:
            if select != 'DWORD':
                raise Unsupported('%s SDWA with %s and dst_sel:%s' % (base, ' '.join(outmods), select))
            return self.set_vf(reg_range(ops[0])[1], text_f.format(*sources), outmods)
        # The operation's HLSL carries its own operators, so it has to be grouped before the
        # destination field masks it: "v7 | v8 & 0xffff" would otherwise bind as "v7 | (v8 & 0xffff)".
        if SDWA_SEL[select][1] < 32:
            result = '(%s)' % result
        destination = reg_range(ops[0])[1]
        unused = fields.get('dst_unused', 'UNUSED_PAD')
        # UNUSED_PRESERVE reads the destination's other bits: read it as an operand, so a pixel stage's entry
        # (SPI input) VGPR read is recorded like any other read
        old = self.u(ops[0]) if unused == 'UNUSED_PRESERVE' and SDWA_SEL[select][1] < 32 else 'v%d' % destination
        self.set_v(destination, sdwa_dest(result, select, unused, old))

    # scalar ALU with descriptor-word bookkeeping
    def scalar(self, base, ops):
        text, scc = SCA[base]
        dst = reg_range(ops[0])
        pair = base.endswith('_b64')
        srcs = ops[1:]
        # 64-bit operands name the low register; exec/vcc name themselves.
        def low(tok):
            r = reg_range(tok)
            return ('s%d' % r[1]) if r and r[0] == 's' and r[1] != r[2] else tok
        srcs = [low(t) for t in srcs]
        dname = low(ops[0])
        word_complement = base in ('s_nand_b32', 's_nor_b32', 's_xnor_b32')
        if word_complement:
            # A lane mask operand or destination is the whole wave's mask, and SCC is set when any bit of the
            # complemented 32-bit result is set; the one-lane projection keeps only this lane's bit.
            def lane_mask(t):
                r = reg_range(t)
                return t in ('exec', 'exec_lo', 'vcc', 'vcc_lo') or bool(r and r[0] == 's' and r[1] in self.masks)
            if lane_mask(dname) or any(lane_mask(t) for t in srcs):
                raise Unsupported('%s on a lane mask' % base)
        if dname == 'm0':
            return None
        syms = [self.sym.get(reg_range(t)[1]) if reg_range(t) and reg_range(t)[0] == 's' else None for t in srcs]
        if any(s and s[0] in ('desc', 'descnum', 'ptr', 'ptrhi') for s in syms):
            if word_complement:
                # the descriptor path below records the word symbolically and emits no SCC
                raise Unsupported('scc from descriptor data')
            if dname in ('exec', 'vcc', 'exec_lo', 'vcc_lo'):
                raise Unsupported('mask from descriptor data')
            d = reg_range(dname)[1]
            own = self.sym.get(d)
            if base in ('s_mov_b32', 's_mov_b64'):
                self.sym[d] = syms[0]
                if pair:
                    hi = self.sym.get(reg_range(ops[1])[2])
                    if hi:
                        self.sym[d + 1] = hi
                    else:
                        self.sym.pop(d + 1, None)
            elif own and own[0] in ('desc', 'ptr', 'ptrhi') and any(s == own for s in syms):
                if own[0] == 'desc':
                    self.pipe.modified.add((own[1], own[2]))
                    # which word changed and how (buffer_store accepts edits that keep the base address)
                    other = [t for t, s in zip(srcs, syms) if s != own]
                    self.pipe.__dict__.setdefault('modified_words', {}).setdefault((own[1], own[2]), []).append(
                        (own[3], base, tuple(other)))
            else:
                self.sym[d] = ('descnum',)
            return None
        if base == 's_pack_hh_b32_b16' and any(
                t.startswith(('exec', 'vcc')) or (reg_range(t) and reg_range(t)[0] == 's' and reg_range(t)[1] in self.masks)
                for t in srcs):
            # a lane mask is one bit per lane in the one-lane model: its high half has no per-lane value
            raise Unsupported('s_pack_hh_b32_b16 of a lane mask')
        if base.startswith('s_bcnt') and any(t in ('exec', 'exec_lo', 'vcc', 'vcc_lo') or
                                             (reg_range(t) and reg_range(t)[0] == 's' and reg_range(t)[1] in self.masks)
                                             for t in srcs):
            # a lane count: the one-lane model keeps only this lane's bit of a mask
            raise Unsupported('%s of a lane mask' % base)
        values = [self.u(t) for t in srcs]
        expr = text.format(*values)
        if dname in ('exec', 'exec_lo', 'vcc', 'vcc_lo'):
            var = dname[:4].rstrip('_') if dname.startswith('exec') else 'vcc'
            if var == 'exec':
                copy = base in ('s_mov_b64', 's_mov_b32') and self.exec_known and (syms[0] == ('execcopy',))
                if copy:
                    return None
                self.exec_known = False
            self.emit('%s = (%s) & 1;' % (var, expr))
            if scc:
                self.emit('scc = %s;' % scc.replace('$', var))
            return None
        d = reg_range(dname)[1]
        is_mask = pair or any(t in ('exec', 'exec_lo', 'vcc', 'vcc_lo') or (reg_range(t) and reg_range(t)[0] == 's' and reg_range(t)[1] in self.masks)
                              for t in srcs) and base.split('_')[1] in ('mov', 'and', 'or', 'xor', 'andn2', 'orn2', 'nand', 'nor', 'xnor', 'not', 'cselect')
        if base in ('s_mov_b32', 's_mov_b64', 's_movk_i32') and re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', srcs[0]) and srcs[0] not in FLOATS:
            value = int(srcs[0], 0)
            self.set_s(d, values[0], ('const', value & 0xffffffff))
            if pair:
                self.set_s(d + 1, '0x%08xu' % ((value >> 32) & 0xffffffff), ('const', (value >> 32) & 0xffffffff))
            self.masks.discard(d)
            return None
        if base in ('s_mov_b64', 's_mov_b32') and srcs[0] in ('exec', 'exec_lo') and self.exec_known:
            self.set_s(d, 'exec')
            self.sym[d] = ('execcopy',)
            self.masks.add(d)
            if pair:
                self.sym.pop(d + 1, None)
            return None
        if is_mask:
            expr = '(%s) & 1' % expr
        self.set_s(d, expr, scc=scc)
        if is_mask:
            self.masks.add(d)
        if pair and base != 's_mov_b64':
            self.set_s(d + 1, '0u')
        elif pair:
            r = reg_range(ops[1])
            self.set_s(d + 1, self.S(r[2]) if r and r[1] != r[2] else '0u')
        return None

    def op_s_and_saveexec_b64(self, ops, mods, text='exec & {0}'):
        d = reg_range(ops[0])[1]
        src = ops[1] if not reg_range(ops[1]) else 's%d' % reg_range(ops[1])[1]
        self.set_s(d, 'exec')
        self.masks.add(d)
        self.exec_known = False
        self.emit('exec = (%s) & 1; scc = exec != 0;' % text.format(self.u(src)))

    op_s_and_saveexec_b32 = op_s_and_saveexec_b64

    def op_s_or_saveexec_b64(self, ops, mods):
        self.op_s_and_saveexec_b64(ops, mods, 'exec | {0}')

    op_s_or_saveexec_b32 = op_s_or_saveexec_b64

    def op_s_andn2_saveexec_b64(self, ops, mods):
        self.op_s_and_saveexec_b64(ops, mods, '{0} & ~exec')

    def op_s_andn2_wrexec_b64(self, ops, mods):
        d = reg_range(ops[0])[1]
        src = 's%d' % reg_range(ops[1])[1]
        self.exec_known = False
        self.emit('exec = (%s & ~exec) & 1; scc = exec != 0;' % self.u(src))
        self.set_s(d, 'exec')
        self.masks.add(d)

    op_s_andn2_wrexec_b32 = op_s_andn2_wrexec_b64

    # SOPK s_addk_i32 / s_mulk_i32: D = D op sext(simm16). LLVM's SIShrinkInstructions forms them from s_add_i32 /
    # s_mul_i32 only when isInt<16> of the constant, so the immediate is signed. s_addk_i32 defines SCC (signed
    # overflow, not modelled: see SCC_UNMODELLED); s_mulk_i32 does not.
    def sopk_arith(self, ops, op):
        d = reg_range(ops[0])
        if not d or d[0] != 's' or d[1] != d[2]:
            raise Unsupported('SOPK destination %s' % ops[0])
        imm = int(ops[1], 0) & 0xffff
        imm = (imm - 0x10000 if imm & 0x8000 else imm) & 0xffffffff
        self.masks.discard(d[1])
        self.set_s(d[1], '%s %s 0x%08xu' % (self.S(d[1]), op, imm))

    # s_bitset0_b32 / s_bitset1_b32: D[S0[4:0]] = 0 / 1, the other bits of D kept (AMD RDNA 2 ISA SOP1; LLVM
    # SOPInstructions.td defines them with D tied as an input and no SCC def). A lane mask or symbolic D is refused.
    def bitset(self, ops, one):
        d = reg_range(ops[0])
        if not d or d[0] != 's' or d[1] != d[2] or d[1] in self.masks:
            raise Unsupported('s_bitset%d_b32 destination %s' % (one, ops[0]))
        bit = '(1u << (%s & 31))' % self.u(ops[1])
        old = self.S(d[1])
        expr = '%s | %s' % (old, bit) if one else '%s & ~%s' % (old, bit)
        a, b = self.is_const(d[1]), self.is_const(reg_range(ops[1])[1]) if reg_range(ops[1]) else None
        if b is None and re.fullmatch(r'(0x[0-9a-f]+|\d+)', ops[1]):
            b = int(ops[1], 0)
        if a is not None and b is not None:  # both known: keep the constant (descriptor words are built this way)
            value = (a | 1 << (b & 31)) if one else (a & ~(1 << (b & 31)))
            self.set_s(d[1], '0x%08xu' % (value & 0xffffffff), ('const', value & 0xffffffff))
            return
        self.set_s(d[1], expr)

    def op_s_bitset0_b32(self, ops, mods):
        self.bitset(ops, 0)

    def op_s_bitset1_b32(self, ops, mods):
        self.bitset(ops, 1)

    def op_s_addk_i32(self, ops, mods):
        self.sopk_arith(ops, '+')

    def op_s_mulk_i32(self, ops, mods):
        self.sopk_arith(ops, '*')

    def op_s_cselect_b32(self, ops, mods):
        self.scalar_select(ops, False)

    def op_s_cselect_b64(self, ops, mods):
        self.scalar_select(ops, True)

    def scalar_select(self, ops, pair):
        low = [('s%d' % reg_range(t)[1]) if reg_range(t) and reg_range(t)[0] == 's' else t for t in ops]
        expr = 'scc ? %s : %s' % (self.u(low[1]), self.u(low[2]))
        if low[0] in ('exec', 'vcc', 'exec_lo', 'vcc_lo'):
            # wave32: vcc_lo / exec_lo are the whole lane mask, as in the generic scalar path
            var = low[0][:4].rstrip('_') if low[0].startswith('exec') else 'vcc'
            if var == 'exec':
                self.exec_known = False
            self.emit('%s = (%s) & 1;' % (var, expr))
            return
        if not reg_range(low[0]) or reg_range(low[0])[0] != 's':
            raise Unsupported('s_cselect into %s' % low[0])
        d = reg_range(low[0])[1]
        self.set_s(d, '(%s) & 1' % expr if pair else expr)
        if pair:
            self.masks.add(d)
            self.set_s(d + 1, '0u')

    def op_s_load_dwordx4(self, ops, mods, count=4):
        dst, base = reg_range(ops[0]), reg_range(ops[1])
        for n in range(dst[1], dst[2] + 1):
            getattr(self, 'masks', set()).discard(n)  # a loaded descriptor or value is not a lane mask (some paths below skip set_s)
        p = self.sym.get(base[1])
        if not p or p[0] != 'ptr':
            raise Unsupported('s_load through s%d holding %s' % (base[1], p))
        if p[1] == ('resheap',) and 'heap' in getattr(self.pipe, 'features', ()) and count in (4, 8) and not mods:
            # A descriptor read from the resource heap at a constant or run-time byte offset (manifest `heap 1`).
            # The title's heap memory holds a view token per 32-byte slot; the HLSL reads the token from heapmem
            # and indexes the desktop view-id region, as the compute path does (draw layout: draw-rt, board msgs
            # 180500). The descriptor words themselves are not read as numbers (S() refuses heapdyn).
            k = len(self.pipe.heapvars)
            self.pipe.heapvars.append(None)
            self.used_h.add(k)
            self.emit('h%d = %s;' % (k, self.u(ops[2]) if reg_range(ops[2]) else '0x%xu' % (int(ops[2], 0) & 0xffffffff)))
            for i in range(count):
                self.sym[dst[1] + i] = ('desc', ('heapdyn', k), 0, i)
            return
        try:
            # 'off' names no offset operand at all, so the byte offset is zero, as for the literal 0.
            off = int(smem_offset(ops[2]), 0)
        except ValueError:
            if p[1] == ('smpheap',) and self.pipe.sampler_heap and count == 4:
                # A sampler picked at run time from the sampler heap: keep the byte offset in the
                # first destination register; image() indexes the heap table with it.
                d = dst[1]
                self.used_s.add(d)
                self.emit('s%d = %s;' % (d, self.u(ops[2])))
                for i in range(count):
                    self.sym[d + i] = ('smpheap', d, i)
                return
            if p[1] == ('smpheap',):
                # Shortcut (default unless X2W_SAMPLER_HEAP=1): a fixed default sampler.
                for i in range(count):
                    self.sym[dst[1] + i] = ('const', 0)
                self.pipe.notes.add('dynamic sampler index replaced by a default sampler')
                return
            raise Unsupported('s_load with a register offset through %s' % (p[1],))
        if p[1][0] == 'root':
            buf = self.pipe.resource((p[1], None), 'buf')
            for i in range(count):
                self.set_s(dst[1] + i, '%s.Load(%d)' % (buf, off + 4 * i))
            return
        for i in range(count):
            self.sym[dst[1] + i] = ('desc', p[1], off, i)

    def op_s_bfm_b32(self, ops, mods):
        """Fold constant bitfield masks: titles build inline sampler words with them."""
        d = reg_range(ops[0])[1]
        vals = []
        for o in ops[1:3]:
            r = reg_range(o)
            if r and r[0] == 's':
                vals.append(self.is_const(r[1]))
            else:
                try:
                    vals.append(int(o, 0))
                except ValueError:
                    vals.append(None)
        if None not in vals:
            value = (((1 << (vals[0] & 31)) - 1) << (vals[1] & 31)) & 0xffffffff
            self.set_s(d, '0x%08xu' % value, ('const', value))
            return None
        self.set_s(d, '(((1u << (%s & 31)) - 1) << (%s & 31))' % (self.u(ops[1]), self.u(ops[2])))
        return None

    def op_s_bfm_b64(self, ops, mods):
        """D[63:0] = ((1 << S0[5:0]) - 1) << S1[5:0]; SCC unchanged. Folded when both counts are constant."""
        d = reg_range(ops[0])[1]
        vals = []
        for o in ops[1:3]:
            r = reg_range(o)
            if r and r[0] == 's':
                vals.append(self.is_const(r[1]))
            else:
                try:
                    vals.append(int(o, 0))
                except ValueError:
                    vals.append(None)
        if None in vals:
            raise Unsupported('s_bfm_b64 with a run-time count')
        value = (((1 << (vals[0] & 63)) - 1) << (vals[1] & 63)) & 0xffffffffffffffff
        self.masks.discard(d)
        self.set_s(d, '0x%08xu' % (value & 0xffffffff), ('const', value & 0xffffffff))
        self.set_s(d + 1, '0x%08xu' % (value >> 32), ('const', value >> 32))
        return None

    def bitcmp(self, ops, want):
        """s_bitcmp{0,1}_b32: SCC = (S0[S1[4:0]] == want); no register is written."""
        self.emit('scc = ((%s >> (%s & 31u)) & 1u) == %du;' % (self.u(ops[0]), self.u(ops[1]), want))

    def op_s_bitcmp0_b32(self, ops, mods):
        self.bitcmp(ops, 0)

    def op_s_bitcmp1_b32(self, ops, mods):
        self.bitcmp(ops, 1)

    def op_s_load_dwordx8(self, ops, mods):
        self.op_s_load_dwordx4(ops, mods, 8)

    def op_s_load_dwordx16(self, ops, mods):
        self.op_s_load_dwordx4(ops, mods, 16)

    def op_s_load_dwordx2(self, ops, mods):
        self.op_s_load_dwordx4(ops, mods, 2)

    def op_s_load_dword(self, ops, mods):
        self.op_s_load_dwordx4(ops, mods, 1)

    def op_s_buffer_load_dword(self, ops, mods, count=1):
        dst = reg_range(ops[0])
        buf = self.pipe.resource(self.descriptor(ops[1]), 'buf')
        token = smem_offset(ops[2])
        offset = self.u(token)
        # An SGPR offset can carry an immediate as well (disasm_rdna2 prints "s4 offset:0x10"): the byte offset is
        # the sum. glc/dlc are cache policy. Anything else is refused.
        extra = 0
        for m in mods:
            if m.startswith('offset:'):
                extra += int(m[7:], 0)
            elif m not in ('glc', 'dlc'):
                raise Unsupported('s_buffer_load modifier %s' % m)
        # Every word's address uses the offset register as it was before the load, so when the destination range
        # holds that register it is written last (s_buffer_load_dwordx2 s[4:5], s[16:19], s4).
        order = list(range(count))
        r = reg_range(token)
        if r and r[0] == 's' and dst[1] <= r[1] < dst[1] + count:
            order.remove(r[1] - dst[1])
            order.append(r[1] - dst[1])
        for i in order:
            self.set_s(dst[1] + i, '%s.Load(%s + %d)' % (buf, offset, extra + 4 * i))
            if not mods and re.fullmatch(r'0x[0-9a-f]+|\d+', token):
                self.sym[dst[1] + i] = ('fixed_buffer_word',)

    def op_s_buffer_load_dwordx2(self, ops, mods):
        self.op_s_buffer_load_dword(ops, mods, 2)

    def op_s_buffer_load_dwordx4(self, ops, mods):
        self.op_s_buffer_load_dword(ops, mods, 4)

    def op_s_buffer_load_dwordx8(self, ops, mods):
        self.op_s_buffer_load_dword(ops, mods, 8)

    def op_s_buffer_load_dwordx16(self, ops, mods):
        self.op_s_buffer_load_dword(ops, mods, 16)

    def buffer_address(self, ops, mods):
        key = self.descriptor(ops[2])
        rw = key in getattr(self.pipe, 'rw_keys', ()) or key in getattr(self.pipe, 'uavs', {})
        if rw:
            # A buffer this program also stores to: read through its UAV (in place, so the program sees its own
            # stores), at the same uavbase-shifted address the stores use. No stride source for a UAV.
            if 'idxen' in mods:
                raise Unsupported('indexed load through a buffer the program stores to')
            buf = self.pipe.uav(key)
        else:
            buf = self.pipe.resource(key, 'buf')
        soff = ops[3]
        if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', soff):
            offset, extra = int(soff, 0), ''
        else:
            offset, extra = 0, '%s + ' % self.u(soff)  # an SGPR soffset adds its value
        for m in mods:
            if m.startswith('offset:'):
                offset += int(m[7:], 0)
        if rw:
            extra = 'ud%d + %s' % (self.pipe.ud(('uavbase', int(buf[1:]))), extra)
        if 'idxen' in mods:
            if key in self.pipe.modified:
                raise Unsupported('indexed load through a modified descriptor')
            if key[0][0] == 'heapdyn':
                # a heap buffer view's stride is vinfo dword 0 of its view id (0 for a raw view)
                address = '%s * x2w_vstride(h%d) + %d' % (self.u(ops[1]), key[0][1], offset)
            else:
                address = '%s * ud%d + %d' % (self.u(ops[1]), self.pipe.ud(('stride',) + (self.pipe.resources[key]['index'],)), offset)
        elif 'offen' in mods:
            address = '%s + %d' % (self.u(ops[1]), offset)
        else:
            address = '%d' % offset
        return buf, extra + address

    def op_buffer_load_dword(self, ops, mods, count=1):
        dst = reg_range(ops[0])
        buf, address = self.buffer_address(ops, mods)
        self.emit('a = %s;' % address)
        for i in range(count):
            self.set_v(dst[1] + i, '%s.Load(a + %d)' % (buf, 4 * i))

    def op_buffer_load_dwordx2(self, ops, mods):
        self.op_buffer_load_dword(ops, mods, 2)

    def op_buffer_load_dwordx3(self, ops, mods):
        self.op_buffer_load_dword(ops, mods, 3)

    def op_buffer_load_dwordx4(self, ops, mods):
        self.op_buffer_load_dword(ops, mods, 4)

    def tbuffer(self, ops, mods, count):
        dst = reg_range(ops[0])
        formats = [m for m in mods if m.startswith('format:')]
        fmt = formats[0][8:-1] if formats else 'BUF_FMT_8_UNORM'  # the disassembler omits the default format
        buf, address = self.buffer_address(ops, [m for m in mods if not m.startswith('format:')])
        if buf.startswith('u'):
            raise Unsupported('typed load through a buffer the program stores to')  # ldu() takes a ByteAddressBuffer
        self.emit('a = %s;' % address)
        table = {'BUF_FMT_8_8_8_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', 'U(((w >> 8) & 255) / 255.0)', 'U(((w >> 16) & 255) / 255.0)', 'U((w >> 24) / 255.0)']),
                 'BUF_FMT_8_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', 'U(((w >> 8) & 255) / 255.0)', '0u', 'U(1.0)']),
                 'BUF_FMT_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', '0u', '0u', 'U(1.0)']),
                 'BUF_FMT_8_UINT': ('{0}.Load(a & ~3u) >> ((a & 3u) * 8u)', ['w & 255u', '0u', '0u', '1u']),
                 # integer fields as unsigned values; a missing component of an integer format reads 1
                 'BUF_FMT_8_8_8_8_UINT': ('ldu({0}, a)', ['w & 255u', '(w >> 8) & 255u', '(w >> 16) & 255u', 'w >> 24']),
                 'BUF_FMT_16_16_UINT': ('ldu({0}, a)', ['w & 65535u', 'w >> 16', '0u', '1u']),
                 # x in bits [9:0], y [19:10], z [29:20], w [31:30] (the R10G10B10A2 layout, x lowest). ASSUMED from
                 # the GCN data-format naming; not checked against a source or a console readback.
                 'BUF_FMT_2_10_10_10_UNORM': ('ldu({0}, a)', ['U((w & 1023) / 1023.0)', 'U(((w >> 10) & 1023) / 1023.0)',
                                                             'U(((w >> 20) & 1023) / 1023.0)', 'U((w >> 30) / 3.0)']),
                 'BUF_FMT_16_16_UNORM': ('ldu({0}, a)', ['U((w & 65535) / 65535.0)', 'U((w >> 16) / 65535.0)', '0u', 'U(1.0)']),
                 # SNORM n: max(c / (2^(n-1) - 1), -1.0), so both -32768 and -32767 read -1.0 (the D3D11.3 functional
                 # spec's SNORM -> FLOAT rule; that the console's buffer fetch clamps the same way is ASSUMED)
                 'BUF_FMT_16_16_SNORM': ('ldu({0}, a)', ['U(max(float(asint(w << 16) >> 16) / 32767.0, -1.0))',
                                                        'U(max(float(asint(w) >> 16) / 32767.0, -1.0))', '0u', 'U(1.0)']),
                 'BUF_FMT_16_16_FLOAT': ('ldu({0}, a)', ['U(f16tof32(w))', 'U(f16tof32(w >> 16))', '0u', 'U(1.0)']),
                 # four binary16 values in eight bytes: x, y in the first dword (low half first), z, w in the second
                 'BUF_FMT_16_16_16_16_FLOAT': ('ldu({0}, a)', ['U(f16tof32(w))', 'U(f16tof32(w >> 16))',
                                                              'U(f16tof32(ldu({0}, a + 4)))', 'U(f16tof32(ldu({0}, a + 4) >> 16))']),
                 'BUF_FMT_32_32_32_32_FLOAT': (None, ['{0}.Load(a)', '{0}.Load(a + 4)', '{0}.Load(a + 8)', '{0}.Load(a + 12)']),
                 'BUF_FMT_32_32_32_FLOAT': (None, ['{0}.Load(a)', '{0}.Load(a + 4)', '{0}.Load(a + 8)', 'U(1.0)']),
                 'BUF_FMT_32_32_FLOAT': (None, ['{0}.Load(a)', '{0}.Load(a + 4)', '0u', 'U(1.0)']),
                 'BUF_FMT_32_FLOAT': (None, ['{0}.Load(a)', '0u', '0u', 'U(1.0)'])}
        if fmt not in table:
            raise Unsupported('buffer format %s' % fmt)
        load, comps = table[fmt]
        if load:
            self.emit('w = %s;' % load.format(buf))
        for i in range(count):
            self.set_v(dst[1] + i, comps[i].format(buf))

    def op_tbuffer_load_format_x(self, ops, mods):
        self.tbuffer(ops, mods, 1)

    def op_tbuffer_load_format_xy(self, ops, mods):
        self.tbuffer(ops, mods, 2)

    def op_tbuffer_load_format_xyz(self, ops, mods):
        self.tbuffer(ops, mods, 3)

    def op_tbuffer_load_format_xyzw(self, ops, mods):
        self.tbuffer(ops, mods, 4)

    def format_load(self, ops, mods, count, name):
        """buffer_load_format_*: the format and swizzle are in the descriptor's word 3, known only at draw time.

        Behind X2W_DRAW_FEATURES=bufword3: the draw runtime passes srv i's word 3 as an entry constant (manifest
        `udsrc <n> bufword3 <i>`: the real word of a title-written descriptor, the synthesized one of a typed
        buffer view, 0 for raw-copy, vertex and root sources) and x2w_fmt_load decodes the element. The idxen
        stride is `udsrc stride` (element size of a typed view, descriptor stride otherwise; d3d12_draw.inl)."""
        if 'bufword3' not in getattr(self.pipe, 'features', ()):
            raise Unsupported(name)
        for m in mods:
            if not (m in ('idxen', 'offen', 'glc', 'slc', 'dlc') or m.startswith('offset:')):
                raise Unsupported('%s modifier %s' % (name, m))
        key = self.descriptor(ops[2])
        if key[0][0] == 'heapdyn' and {'heap', 'heapwords'} <= set(getattr(self.pipe, 'features', ())):
            # A resource-heap buffer view: word 3 is the runtime's synthesized V# word of the view (manifest
            # `heapwords 1`: vdesc, 32 bytes per view id, root SRV t2 space1; mesh_work/RUNTIME_NEEDS.md 6.3, the
            # same contract and helper as mesh2hlsl's format_load). 0 for an unknown slot or id: every channel 0.
            for m in mods:
                if not (m in ('idxen', 'offen', 'glc', 'slc', 'dlc') or m.startswith('offset:')):
                    raise Unsupported('%s modifier %s' % (name, m))
            dst = reg_range(ops[0])
            buf, address = self.buffer_address(ops, mods)
            if buf.startswith('u'):
                raise Unsupported('%s through a buffer the program stores to' % name)
            self.pipe.draw_heapwords = True
            self.emit('a = %s;' % address)
            for i in range(count):
                self.set_v(dst[1] + i, 'x2w_fmt_load(%s, a, x2w_hword(h%d, 3u), %du)' % (buf, key[0][1], i))
            return
        if key[0][0] != 'table':
            # word 3 reaches the shader only for table entries (root and vertex sources give 0)
            raise Unsupported('%s through %s' % (name, key[0][0]))
        dst = reg_range(ops[0])
        buf, address = self.buffer_address(ops, mods)
        if buf.startswith('u'):
            raise Unsupported('%s through a buffer the program stores to' % name)  # bufword3 names an srv
        w3 = self.pipe.ud(('bufword3', self.pipe.resources[key]['index']))
        self.emit('a = %s;' % address)
        for i in range(count):
            self.set_v(dst[1] + i, 'x2w_fmt_load(%s, a, ud%d, %du)' % (buf, w3, i))

    def buffer_store(self, ops, mods, count, name):
        """buffer_store_dword{,x2,x3,x4} to a raw buffer UAV, behind X2W_DRAW_FEATURES=uav.

        The descriptor becomes `uav <i> buf table|root ...` (RWByteAddressBuffer u<i>, draw-rt RUNTIME_DONE.md) and the
        address adds `udsrc <n> uavbase <i>` (the bytes between the 16-aligned view start the runtime binds and the
        descriptor base; without it a misaligned buffer would get a null UAV). Refused: idxen (no stride line for a
        UAV), a descriptor this program also reads as an SRV (D3D12 has no SRV + UAV on one buffer in a draw), and
        any modifier other than offen, offset:N and the cache-policy bits. The lane's store runs only where exec holds.
        """
        if 'uav' not in getattr(self.pipe, 'features', ()):
            raise Unsupported(name)
        for m in mods:
            if not (m in ('offen', 'glc', 'slc', 'dlc') or m.startswith('offset:')):
                raise Unsupported('%s modifier %s' % (name, m))
        key = self.descriptor(ops[2])
        if key[0][0] not in ('table', 'root'):
            raise Unsupported('%s through %s' % (name, key[0][0]))
        if key in self.pipe.modified:
            # A raw store uses only the base address (word 0, word 1 [15:0]); the runtime binds the entry's bytes in
            # place. Accepted edits: words 2 and 3 (record count, format, OOB select: bounds and typing a raw
            # store ignores; assumed in range) and an AND of word 1 that keeps [15:0] (clearing the stride).
            for word, op, other in getattr(self.pipe, 'modified_words', {}).get(key, [('?', '?', ())]):
                keeps_base = word == 1 and op == 's_and_b32' and len(other) == 1 and \
                    re.fullmatch(r'0x[0-9a-f]+', other[0]) and int(other[0], 16) & 0xffff == 0xffff
                if word not in (2, 3) and not keeps_base:
                    raise Unsupported('%s through a modified descriptor' % name)
        u = self.pipe.uav(key)
        base = self.pipe.ud(('uavbase', int(u[1:])))
        offset = sum(int(m[7:], 0) for m in mods if m.startswith('offset:'))
        soff = ops[3]
        if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', soff):
            offset += int(soff, 0)
            parts = []
        else:
            parts = [self.u(soff)]  # an SGPR soffset adds its value
        if 'offen' in mods:
            parts.insert(0, self.u(ops[1]))
        address = ' + '.join(['ud%d' % base] + parts + ['%d' % offset])
        data = reg_range(ops[0])
        if data[2] - data[1] + 1 != count:
            raise Unsupported('%s data %s' % (name, ops[0]))
        values = ', '.join(self.u('v%d' % (data[1] + i)) for i in range(count))
        call = '%s.Store%s(a, %s);' % (u, '' if count == 1 else count, values if count == 1 else 'uint%d(%s)' % (count, values))
        self.emit('a = %s;' % address)
        self.emit(call if self.exec_known else 'if (exec & 1) %s' % call)

    def op_buffer_store_dword(self, ops, mods):
        self.buffer_store(ops, mods, 1, 'buffer_store_dword')

    def op_buffer_store_dwordx2(self, ops, mods):
        self.buffer_store(ops, mods, 2, 'buffer_store_dwordx2')

    def op_buffer_store_dwordx3(self, ops, mods):
        self.buffer_store(ops, mods, 3, 'buffer_store_dwordx3')

    def op_buffer_store_dwordx4(self, ops, mods):
        self.buffer_store(ops, mods, 4, 'buffer_store_dwordx4')

    def op_buffer_load_format_x(self, ops, mods):
        self.format_load(ops, mods, 1, 'buffer_load_format_x')

    def op_buffer_load_format_xy(self, ops, mods):
        self.format_load(ops, mods, 2, 'buffer_load_format_xy')

    def op_buffer_load_format_xyz(self, ops, mods):
        self.format_load(ops, mods, 3, 'buffer_load_format_xyz')

    def op_buffer_load_format_xyzw(self, ops, mods):
        self.format_load(ops, mods, 4, 'buffer_load_format_xyzw')

    def image(self, ops, mods, how):
        dst = reg_range(ops[0])
        dim = [m for m in mods if m.startswith('dim:')][0][4:]
        dmask = int([m for m in mods if m.startswith('dmask:')][0][6:], 0)
        kinds = {'SQ_RSRC_IMG_1D': 'tex1d', 'SQ_RSRC_IMG_2D': 'tex2d', 'SQ_RSRC_IMG_3D': 'tex3d',
                 'SQ_RSRC_IMG_2D_ARRAY': 'tex2da'}
        if dim not in kinds:
            raise Unsupported('image dimension %s' % dim)
        # A 2D array takes the slice as its third address component, where a volume takes z. Sample and
        # Load on a Texture2DArray take it in the same place, so only the declared type differs. A 1D image has
        # the one coordinate (x for a load, u for a sample), bound as a Texture1D (manifest kind tex1d).
        axes = {'SQ_RSRC_IMG_1D': 1, 'SQ_RSRC_IMG_2D': 2}.get(dim, 3)
        coords = ops[1]
        if coords.startswith('['):
            cs = [c.strip() for c in coords[1:-1].split(',')]
        else:
            r = reg_range(coords)
            cs = ['v%d' % n for n in range(r[1], r[2] + 1)]
        # Every form takes only dim, dmask, the cache-policy bits and, for the loads, unorm (a load addresses texels by
        # integer coordinates either way) and a16. a16 packs the 16-bit address components two to a dword, the first
        # in the low half (LLVM SIISelLowering packImage16bitOpsToDwords): unsigned texel coordinates for a load and
        # binary16 coordinates for a sample. d16, tfe, lwe and r128 change the result or resource layout and are
        # refused, as is any modifier not named here.
        # (image_sample_l and image_load_mip check their own modifier lists below.)
        allowed = ('dlc', 'glc', 'slc', 'a16') + (('unorm',) if how == 'load' else ('d16',) if how in ('sample', 'lz') else ())
        for m in mods:
            if how in ('sample', 'lz', 'load', 'b', 'd') and not (m.startswith(('dim:', 'dmask:')) or m in allowed):
                raise Unsupported('image_%s modifier %s' % (how, m))
        a16 = 'a16' in mods
        bias = None
        if how == 'b':
            # image_sample_b: the LOD bias is the first address dword, before the coordinates (LLVM
            # AMDGPUImageDimIntrinsics sample_b: ExtraAddrArgs [bias]). With a16 it would be a 16-bit value packed
            # into its own dword, not modelled here.
            if a16 or dmask == 0 or len(cs) != axes + 1:
                raise Unsupported('image_sample_b form %s %s' % (', '.join(ops), ' '.join(mods)))
            bias, cs = cs[0], cs[1:]
        grads = None
        if how == 'd':
            # image_sample_d: the gradients come first, d<c>dh for each coordinate c (s, t, r) then d<c>dv, and then
            # the coordinates and the slice (LLVM IntrinsicsAMDGPU.td AMDGPUDimProps.GradientArgs, AddrArgs order
            # ExtraAddrArgs, GradientArgs, CoordSliceArgs). h is the horizontal (x) and v the vertical (y) screen
            # direction: SampleGrad's ddx and ddy. A 2D array has 2D gradients. With a16 the gradients would be
            # packed too, not modelled here.
            n = {'SQ_RSRC_IMG_1D': 1, 'SQ_RSRC_IMG_2D': 2, 'SQ_RSRC_IMG_3D': 3, 'SQ_RSRC_IMG_2D_ARRAY': 2}[dim]
            if a16 or dmask == 0 or len(cs) != 2 * n + axes:
                raise Unsupported('image_sample_d form %s %s' % (', '.join(ops), ' '.join(mods)))
            grads, cs = (cs[:n], cs[n:2 * n]), cs[2 * n:]
        if a16:
            if len(cs) < (axes + 1) // 2:
                raise Unsupported('a16 image address with %d registers' % len(cs))
            # component i is the (i % 2) half of address dword i // 2; unpacked to one 32-bit value each
            cs = [('a16', cs[i // 2], i % 2) for i in range(axes)]
        lod = None
        if how == 'l':
            # Address dwords are the coordinates and then the explicit level; offsets, bias, clamp and derivatives
            # would add dwords or modifiers this lowering does not read, so they are refused. Only the cache-policy
            # bits (glc, slc, dlc), which never change the sampled value, are accepted next to dim and dmask;
            # unorm, r128, tfe, lwe, a16, d16 and reserved bits would each change the address, resource or result
            # layout and are refused by name.
            for m in mods:
                if not (m.startswith(('dim:', 'dmask:')) or m in ('dlc', 'glc', 'slc')):
                    raise Unsupported('image_sample_l modifier %s' % m)
            if dmask == 0:
                raise Unsupported('image_sample_l with an empty dmask')
            if len(cs) != axes + 1:
                raise Unsupported('image_sample_l with %d address registers' % len(cs))
            lod, cs = cs[-1], cs[:-1]
        tex = self.pipe.resource(self.descriptor(ops[2]), kinds[dim])
        if how in ('load', 'load_mip'):
            # image_load_mip addresses x, y, [slice or z] and then the mip, so a 2D array is four registers
            # and Texture2DArray.Load takes int4(x, y, slice, mip) like a volume's int4(x, y, z, mip); a Texture1D
            # takes int2(x, mip).
            mip = '0'
            if how == 'load_mip':
                unsupported = [m for m in mods if not m.startswith(('dim:', 'dmask:'))
                               and m not in ('unorm', 'glc', 'slc', 'dlc')]
                if unsupported or len(cs) != axes + 1 or not dmask:
                    raise Unsupported('image_load_mip address or modifiers: %s' % ' '.join(mods))
                mip = self.u(cs[axes])
            if len(cs) < axes:
                raise Unsupported('image_%s with %d address registers' % (how, len(cs)))
            address = [self.a16_u(c) if isinstance(c, tuple) else self.u(c) for c in cs[:axes]] + [mip]
            call = '%s.Load(int%d(%s))' % (tex, axes + 1, ', '.join(address))
        else:
            heap = self.heap_sampler(ops[3])
            words = None if heap else self.const_words(ops[3])
            smp = heap or self.pipe.sampler(tuple(words) if words else self.descriptor(ops[3]))
            if len(cs) < axes:
                raise Unsupported('image_%s with %d address registers' % (how, len(cs)))
            fs = [self.a16_f(c) if isinstance(c, tuple) else self.f(c) for c in cs[:axes]]
            uv = fs[0] if axes == 1 else 'float%d(%s)' % (axes, ', '.join(fs))
            if how == 'l':
                call = '%s.SampleLevel(%s, %s, %s)' % (tex, smp, uv, self.f(lod))
            elif how == 'b':
                # SampleBias takes implicit derivatives like Sample, so it is lowered only where Sample is (below);
                # the level-0 fallback Sample uses elsewhere would ignore the bias.
                if not self.exec_known or self.kind != 'ps' or len(self.blocks) > 0:
                    raise Unsupported('image_sample_b outside straight-line code with every lane active')
                call = '%s.SampleBias(%s, %s, %s)' % (tex, smp, uv, self.f(bias))
            elif how == 'd':
                # explicit gradients: no implicit derivatives, so any control flow and any exec state
                ddx, ddy = (', '.join(self.f(c) for c in g) for g in grads)
                if len(grads[0]) > 1:
                    ddx, ddy = 'float%d(%s)' % (len(grads[0]), ddx), 'float%d(%s)' % (len(grads[0]), ddy)
                call = '%s.SampleGrad(%s, %s, %s, %s)' % (tex, smp, uv, ddx, ddy)
            elif how == 'lz' or not self.exec_known or self.kind != 'ps' or len(self.blocks) > 0:
                call = '%s.SampleLevel(%s, %s, 0)' % (tex, smp, uv)
            else:
                call = '%s.Sample(%s, %s)' % (tex, smp, uv)
        self.emit('t = %s;' % call)
        n = dst[1]
        if 'd16' in mods:
            # Packed D16 (gfx9+, LLVM GCNSubtarget hasPackedD16VMem; SIISelLowering adjustLoadValueType): each enabled
            # channel of the filtered result is converted to binary16 and two go in one VGPR, the first channel in
            # the low half; the high half after an odd channel count is written as 0 here (undefined in hardware).
            # Only the sampling forms take it: their result is a float whatever the texel format.
            comps = ['f32tof16(t.%s)' % c for bit, c in enumerate('xyzw') if dmask & (1 << bit)]
            if not comps or dst[2] - dst[1] + 1 != (len(comps) + 1) // 2:
                raise Unsupported('image_%s d16 with dmask %#x into %s' % (how, dmask, ops[0]))
            for i in range(0, len(comps), 2):
                self.set_v(n, comps[i] if i + 1 == len(comps) else '%s | (%s << 16)' % (comps[i], comps[i + 1]))
                n += 1
            return
        for bit, comp in enumerate('xyzw'):
            if dmask & (1 << bit):
                self.set_v(n, 'U(t.%s)' % comp)
                n += 1

    def a16_u(self, component):
        _, reg, half = component
        return '(%s >> 16)' % self.u(reg) if half else '(%s & 0xffffu)' % self.u(reg)

    def a16_f(self, component):
        _, reg, half = component
        return 'f16tof32(%s)' % ('%s >> 16' % self.u(reg) if half else self.u(reg))

    def image_compare(self, ops, mods, how):
        """image_sample_c and image_sample_c_lz: a 2D or 2D-array depth comparison returning one float.

        The address dwords are the reference, then u and v (and the slice of a 2D array), which is what
        SampleCmp* takes as its third argument and its location. The comparison function and the filter live in the sampler descriptor,
        so they are read from its constant words by the decoder the pixel AOT path already checks against
        D3D12 (pixel_aot.comparison_sampler_state) and bound as a SamplerComparisonState. HLSL has no
        SampleCmpLevel before shader model 6.7 (the stages here compile as 5.1 and 6.0), so
        image_sample_c_l is refused instead of being sampled at level zero.
        """
        base = 'image_sample_' + how
        if how == 'c_l':
            raise Unsupported('image_sample_c_l: SampleCmpLevel needs shader model 6.7; only c and c_lz are lowered')
        for m in mods:
            if not (m.startswith(('dim:', 'dmask:')) or m in ('dlc', 'glc', 'slc')):
                raise Unsupported('%s modifier %s' % (base, m))
        dims = [m[4:] for m in mods if m.startswith('dim:')]
        masks = [m[6:] for m in mods if m.startswith('dmask:')]
        if len(dims) != 1 or len(masks) != 1:
            raise Unsupported('%s without exactly one dim and dmask' % base)
        dim, dmask = dims[0], int(masks[0], 0)
        if dim not in ('SQ_RSRC_IMG_2D', 'SQ_RSRC_IMG_2D_ARRAY'):
            raise Unsupported('%s on %s' % (base, dim))
        array = dim == 'SQ_RSRC_IMG_2D_ARRAY'
        if dmask != 1:
            raise Unsupported('%s with dmask %#x: a comparison returns one channel' % (base, dmask))
        if len(ops) != 4:
            raise Unsupported('%s with %d operands' % (base, len(ops)))
        coords = ops[1]
        if coords.startswith('['):
            cs = [c.strip() for c in coords[1:-1].split(',')]
        else:
            r = reg_range(coords)
            cs = ['v%d' % n for n in range(r[1], r[2] + 1)]
        # The reference, u and v; a 2D array adds the slice as the last address register.
        if len(cs) != (4 if array else 3):
            raise Unsupported('%s with %d address registers' % (base, len(cs)))
        if self.heap_sampler(ops[3]):
            raise Unsupported('%s through a sampler heap' % base)
        words = self.const_words(ops[3])
        if words is None:
            raise Unsupported('%s with a sampler that is not constant' % base)
        import pixel_aot
        try:
            pixel_aot.comparison_sampler_state(tuple(words))
        except ValueError as error:
            raise Unsupported('%s sampler: %s' % (base, error))
        tex = self.pipe.resource(self.descriptor(ops[2]), 'tex2da' if array else 'tex2d')
        smp = self.pipe.sampler(tuple(words), comparison=True)
        # Texture2DArray takes the slice as the third location component, like Sample and Load on the same type.
        location = ('float3(%s, %s, %s)' % (self.f(cs[1]), self.f(cs[2]), self.f(cs[3])) if array
                    else 'float2(%s, %s)' % (self.f(cs[1]), self.f(cs[2])))
        args = '%s, %s, %s' % (smp, location, self.f(cs[0]))
        # MAX_LOD (word 1 bits 23:12) is not carried into the static sampler, and the comparison-state mapping ignores
        # the mip filter and uses level zero. Only a source limited to level zero is therefore exact for the implicit
        # form; any other limit would need mip selection and blending that is not lowered in this slice, so it is refused.
        max_lod = (words[1] >> 12) & 0xfff
        if how == 'c' and max_lod:
            raise Unsupported('%s with implicit mip selection (sampler MAX_LOD %#x)' % (base, max_lod))
        call = '%s.SampleCmpLevelZero(%s)' % (tex, args)
        self.set_v(reg_range(ops[0])[1], 'U(%s)' % call)

    def op_image_gather4_lz(self, ops, mods):
        """image_gather4_lz on a 2D texture: the one channel dmask names, from the 2x2 footprint at level 0.

        The four result dwords are the texels in the order Gather*() returns them, (u0,v1), (u1,v1), (u1,v0),
        (u0,v0): gather4 is the D3D Gather operation, and compilers map Gather's four results straight onto the
        four destination VGPRs. Gather always reads the most detailed level, which is what _lz names. Offsets
        (_o), compare (_c), an explicit or biased level, other dimensions and result-layout modifiers (d16, tfe,
        lwe, a16, unorm, r128) would change the address or the result and are refused by name.
        """
        for m in mods:
            if not (m.startswith(('dim:', 'dmask:')) or m in ('dlc', 'glc', 'slc')):
                raise Unsupported('image_gather4_lz modifier %s' % m)
        dims = [m[4:] for m in mods if m.startswith('dim:')]
        masks = [int(m[6:], 0) for m in mods if m.startswith('dmask:')]
        if dims != ['SQ_RSRC_IMG_2D'] or len(masks) != 1:
            raise Unsupported('image_gather4_lz on %s' % (dims or 'no dim'))
        channel = {1: 'Red', 2: 'Green', 4: 'Blue', 8: 'Alpha'}.get(masks[0])
        if channel is None:
            raise Unsupported('image_gather4_lz dmask %#x: gather reads exactly one channel' % masks[0])
        dst = reg_range(ops[0])
        if len(ops) != 4 or not dst or dst[0] != 'v' or dst[2] - dst[1] != 3:
            raise Unsupported('image_gather4_lz operands %s' % ', '.join(ops))
        coords = ops[1]
        if coords.startswith('['):
            cs = [c.strip() for c in coords[1:-1].split(',')]
        else:
            r = reg_range(coords)
            cs = ['v%d' % n for n in range(r[1], r[2] + 1)]
        if len(cs) != 2:
            raise Unsupported('image_gather4_lz with %d address registers' % len(cs))
        tex = self.pipe.resource(self.descriptor(ops[2]), 'tex2d')
        heap = self.heap_sampler(ops[3])
        words = None if heap else self.const_words(ops[3])
        smp = heap or self.pipe.sampler(tuple(words) if words else self.descriptor(ops[3]))
        self.emit('t = %s.Gather%s(%s, float2(%s, %s));' % (tex, channel, smp, self.f(cs[0]), self.f(cs[1])))
        for i, comp in enumerate('xyzw'):
            self.set_v(dst[1] + i, 'U(t.%s)' % comp)

    def op_image_sample_c(self, ops, mods):
        self.image_compare(ops, mods, 'c')

    def op_image_sample_c_lz(self, ops, mods):
        self.image_compare(ops, mods, 'c_lz')

    def op_image_sample_c_l(self, ops, mods):
        self.image_compare(ops, mods, 'c_l')

    def op_image_sample(self, ops, mods):
        self.image(ops, mods, 'sample')

    def op_image_sample_b(self, ops, mods):
        self.image(ops, mods, 'b')

    def op_image_sample_d(self, ops, mods):
        self.image(ops, mods, 'd')

    def op_image_sample_lz(self, ops, mods):
        self.image(ops, mods, 'lz')

    def op_image_sample_l(self, ops, mods):
        self.image(ops, mods, 'l')

    def op_image_load(self, ops, mods):
        self.image(ops, mods, 'load')

    def op_image_load_mip(self, ops, mods):
        self.image(ops, mods, 'load_mip')

    def op_v_cvt_i32_f32(self, ops, mods):
        if mods:
            raise Unsupported('v_cvt_i32_f32 with modifiers')
        dst = reg_range(ops[0])[1]
        self.set_v(dst, 'uint(int(F(%s)))' % self.u(ops[1]))
        src = reg_range(ops[1])
        if self.exec_known and src and src[0] == 's' and self.sym.get(src[1]) == ('fixed_buffer_word',):
            self.uniform_v.add(dst)

    def op_v_readfirstlane_b32(self, ops, mods):
        # D = S0 of the first active lane (AMD RDNA 2 ISA; LLVM selects it for a VGPR-to-SGPR copy of a value the
        # divergence analysis found uniform, SIFixSGPRCopies). With a lane-invariant source (see set_v) every
        # lane's own value is that value. Branches are allowed: invariance is cleared at every branch target.
        src, dst = reg_range(ops[1]), reg_range(ops[0])
        partial = getattr(self, 'partial_uniform', {}).get(src[1]) if src and src[0] == 'v' else None
        if (not mods and dst and dst[0] == 's' and partial is not None and
                partial[3] == getattr(self, 'exec_writes', 0)):
            # The source was written, in the lanes active then, from a lane-invariant value, and exec has not been
            # written since: the first active lane holds that value. Lanes inactive now kept an older VGPR value,
            # but the SGPR is the wave's, so every lane takes the value itself, from a shadow assigned in all lanes
            # at the write. (With no active lane the hardware reads lane 0; that case is not modelled.)
            lines, at, expr, _ = partial
            n = src[1]
            lines[at] = 'xv%d = %s; if (exec & 1) v%d = xv%d;' % (n, expr, n, n)
            self.shadow_v.add(n)
            self.set_s(dst[1], 'xv%d' % n)
            return
        if (mods or not self.exec_known or not dst or dst[0] != 's' or
                not src or src[0] != 'v' or src[1] not in self.uniform_v):
            raise Unsupported('v_readfirstlane_b32 requires a proven uniform source')
        self.set_s(reg_range(ops[0])[1], self.u(ops[1]))

    def pair(self, tok):
        """(low, high) uint expressions of a 64-bit integer operand: a register pair or an inline integer constant
        (sign-extended to 64 bits). A literal is refused: its 64-bit extension is not pinned down here."""
        r = reg_range(tok)
        if r:
            if r[2] != r[1] + 1:
                raise Unsupported('64-bit operand %s' % tok)
            if r[0] == 'v':
                self.used_v.update((r[1], r[2]))
                return 'v%d' % r[1], 'v%d' % r[2]
            return self.S(r[1]), self.S(r[2])
        try:
            value = int(tok, 0)
        except ValueError:
            raise Unsupported('64-bit operand %r' % tok)
        if tok.startswith('0x') and 0 <= value <= 0x7fffffff:
            # A 32-bit literal in a 64-bit integer operand. Whether hardware zero- or sign-extends it is open (LLVM
            # AMDGPUDisassembler::decodeLiteralConstant zero-extends with a ToDo on signedness; the assembler takes
            # Lo_32), but with bit 31 clear both readings give the high dword 0.
            return '0x%08xu' % value, '0x00000000u'
        if not -16 <= value <= 64 or tok.startswith('0x'):
            raise Unsupported('64-bit literal operand %s' % tok)
        value &= 0xffffffffffffffff
        return '0x%08xu' % (value & 0xffffffff), '0x%08xu' % (value >> 32)

    def shift64(self, ops, mods, helper):
        """v_l{sh,sh}rrev_b64 D, S0, S1: D = S1 shifted by S0[5:0]; the shift count comes first ("rev")."""
        if mods:
            raise Unsupported('64-bit shift with %s' % mods)
        d = reg_range(ops[0])
        if not d or d[0] != 'v' or d[2] != d[1] + 1:
            raise Unsupported('64-bit shift destination %s' % ops[0])
        low, high = self.pair(ops[2])
        count = self.u(ops[1])
        self.uses_q = True
        # Read both source words into q before either destination word is written: D and S1 may overlap.
        self.emit('q = uint2(%s_lo(%s, %s, %s), %s_hi(%s, %s, %s));' % (helper, low, high, count, helper, low, high, count))
        self.set_v(d[1], 'q.x')
        self.set_v(d[1] + 1, 'q.y')

    def carry_op(self, ops, mods, kind, carry_in):
        """v_{add,sub,subrev}_co[_ci]_u32 D, SDST, S0, S1[, CI]: 32-bit add or subtract with a carry or borrow out.

        AMD RDNA 2 ISA: V_ADD_CO_CI_U32 D = S0 + S1 + CI, carry = the 33rd bit; V_SUB_CO_CI_U32 D = S0 - S1 - CI,
        borrow = S1 + CI > S0 (as 33-bit values); SUBREV swaps S0 and S1; the _co forms have no carry-in. LLVM
        VOP2Instructions.td defines V_ADD_CO_CI_U32 as gfx10's name for v_addc_u32 (VOP2b, carry-out an SGPR mask).
        The carry is the lane's bit of SDST, as v_cmp writes it; CI must be a lane mask (vcc, exec or an SGPR the
        translation holds as a mask). Both results go through q, so D or SDST may overlap a source.
        """
        if mods:
            raise Unsupported('v_%s_co%s_u32 with %s' % (kind, '_ci' if carry_in else '', ' '.join(mods)))
        if len(ops) != (5 if carry_in else 4):
            raise Unsupported('v_%s_co%s_u32 with %d operands' % (kind, '_ci' if carry_in else '', len(ops)))
        d = reg_range(ops[0])
        if not d or d[0] != 'v' or d[1] != d[2]:
            raise Unsupported('carry operation destination %s' % ops[0])
        a, b = self.u(ops[2]), self.u(ops[3])
        if kind == 'subrev':
            a, b = b, a
        c = '0u'
        if carry_in:
            r = reg_range(ops[4])
            if not (ops[4] in ('vcc', 'vcc_lo', 'exec', 'exec_lo') or (r and r[0] == 's' and r[1] in self.masks)):
                raise Unsupported('carry-in %s is not a lane mask' % ops[4])
            c = '(%s & 1u)' % self.u(ops[4] if not r else 's%d' % r[1])
        if kind == 'add':
            value, carry = '%s + %s + %s' % (a, b, c), '((%s + %s) < %s || (%s + %s + %s) < (%s + %s))' % (
                a, b, a, a, b, c, a, b)
        else:
            value, carry = '%s - %s - %s' % (a, b, c), '(%s > %s || (%s - %s) < %s)' % (b, a, a, b, c)
        self.uses_q = True
        self.emit('q = uint2(%s, %s ? 1u : 0u);' % (value, carry))
        self.set_v(d[1], 'q.x')
        sdst = ops[1]
        r = reg_range(sdst)
        self.write_compare(False, sdst if not r else 's%d' % r[1], 'q.y')

    def op_v_add_co_ci_u32(self, ops, mods):
        self.carry_op(ops, mods, 'add', True)

    def op_v_sub_co_ci_u32(self, ops, mods):
        self.carry_op(ops, mods, 'sub', True)

    def op_v_subrev_co_ci_u32(self, ops, mods):
        self.carry_op(ops, mods, 'subrev', True)

    def op_v_add_co_u32(self, ops, mods):
        self.carry_op(ops, mods, 'add', False)

    def op_v_sub_co_u32(self, ops, mods):
        self.carry_op(ops, mods, 'sub', False)

    def op_v_subrev_co_u32(self, ops, mods):
        self.carry_op(ops, mods, 'subrev', False)

    def op_v_lshrrev_b64(self, ops, mods):
        self.shift64(ops, mods, 'x2w_lshr64')

    def op_v_lshlrev_b64(self, ops, mods):
        self.shift64(ops, mods, 'x2w_lshl64')

    def half_op(self, ops, mods, fn):
        """v_{min,max}_f16: the binary16 result goes to D[15:0]; gfx10 keeps D[31:16] (LLVM
        GCNSubtarget::zeroesHigh16BitsOfDest: "On gfx10, all 16-bit instructions preserve the high bits")."""
        if mods:
            raise Unsupported('f16 operation with %s' % mods)
        if any(t.startswith(('-', '|')) and not re.match(r'-[\d.]', t) for t in ops[1:3]):
            raise Unsupported('f16 operation with source modifiers')
        d = reg_range(ops[0])[1]
        self.used_v.add(d)
        a, b = ('f16tof32(%s)' % self.h(t) for t in ops[1:3])
        self.set_v(d, '(v%d & 0xffff0000u) | f32tof16(%s(%s, %s))' % (d, fn, a, b))

    def op_v_min_f16(self, ops, mods):
        self.half_op(ops, mods, 'min')

    def op_v_max_f16(self, ops, mods):
        self.half_op(ops, mods, 'max')

    def packed_half(self, ops, mods, fn):
        """v_pk_{min,max}_f16: two binary16 lanes. Result half i reads, from each source, the half picked by
        op_sel (low result) or op_sel_hi (high result); the disassembler prints only non-default selections."""
        sel, sel_hi = [0, 0], [1, 1]
        for m in mods:
            if m.startswith('op_sel:['):
                sel = [int(x) for x in m[8:-1].split(',')]
            elif m.startswith('op_sel_hi:['):
                sel_hi = [int(x) for x in m[11:-1].split(',')]
            else:
                raise Unsupported('packed f16 operation with %s' % m)
        if len(sel) != 2 or len(sel_hi) != 2:
            raise Unsupported('packed f16 selection %s' % mods)
        words = []
        for index, tok in enumerate(ops[1:3]):
            r = reg_range(tok)
            if r:
                words.append(self.u(tok))
            elif re.fullmatch(r'0x[0-9a-f]+', tok) and not sel[index] and not sel_hi[index]:
                words.append(self.u(tok))  # a literal read only through its low half
            elif tok == '0':
                words.append('0x00000000u')  # the inline integer 0: every bit is 0, so each half reads +0.0
            else:
                raise Unsupported('packed f16 operand %s with selection %s/%s' % (tok, sel[index], sel_hi[index]))
        def half(word, high):
            return 'f16tof32(%s >> 16)' % word if high else 'f16tof32(%s)' % word
        low = 'f32tof16(%s(%s, %s))' % (fn, half(words[0], sel[0]), half(words[1], sel[1]))
        high = 'f32tof16(%s(%s, %s))' % (fn, half(words[0], sel_hi[0]), half(words[1], sel_hi[1]))
        self.set_v(reg_range(ops[0])[1], '%s | (%s << 16)' % (low, high))

    def op_v_pk_min_f16(self, ops, mods):
        self.packed_half(ops, mods, 'min')

    def op_v_pk_max_f16(self, ops, mods):
        self.packed_half(ops, mods, 'max')

    def op_v_cvt_f16_f32(self, ops, mods):
        # gfx10 keeps D[31:16] for 16-bit results (see half_op); the SDWA form keeps its own dst_sel handling.
        if mods:
            raise Unsupported('v_cvt_f16_f32 with %s' % mods)
        if len(ops) != 2:  # the VEC_U operand-count rule (#1716); this local handler bypasses the table
            raise Unsupported('v_cvt_f16_f32 with %d operands, expected 2' % len(ops))
        d = reg_range(ops[0])[1]
        self.used_v.add(d)
        self.set_v(d, '(v%d & 0xffff0000u) | f32tof16(F(%s))' % (d, self.u(ops[1])))

    def op_v_mov_b32(self, ops, mods):
        self.set_v(reg_range(ops[0])[1], self.u(ops[1]))

    def op_v_swap_b32(self, ops, mods):
        # RDNA 2 ISA V_SWAP_B32 (VOP1 0x65, LLVM VOP1Instructions.td: outs $vdst, $vdst1 tied to $src0):
        # tmp = D; D = S0; S0 = tmp, in the active lanes. Both operands are single VGPRs; there is no VOP3,
        # DPP or SDWA form (the decoder reports those as unsupported).
        d, s = reg_range(ops[0]) if ops else None, reg_range(ops[1]) if len(ops) == 2 else None
        if mods or len(ops) != 2 or not d or not s or d[0] != 'v' or s[0] != 'v' or d[1] != d[2] or s[1] != s[2]:
            raise Unsupported('v_swap_b32 form %s %s' % (ops, mods))
        if d[1] == s[1]:
            return  # a register swapped with itself keeps its value
        self.uses_swap = True
        self.lines.append('xswap = %s;' % self.u(ops[0]))  # every lane: a temporary read only below
        self.set_v(d[1], self.u(ops[1]))
        self.set_v(s[1], 'xswap')

    def op_v_cndmask_b32(self, ops, mods):
        # The VOP3 form takes neg/abs on its two data operands; the select moves bits, so they are sign-bit
        # operations on the selected word.
        if mods:
            raise Unsupported('v_cndmask_b32 with %s' % mods)
        mask = self.u(ops[3]) if len(ops) > 3 else 'vcc'
        self.set_v(reg_range(ops[0])[1], '(%s & 1) ? %s : %s' % (mask, self.u_mod(ops[2]), self.u_mod(ops[1])))

    def op_v_interp_p1_f32(self, ops, mods):
        self.note_attr(ops[2], 'linear')

    def op_v_interp_p2_f32(self, ops, mods):
        attr, comp = self.note_attr(ops[2], 'linear')
        self.set_v(reg_range(ops[0])[1], 'U(i.param%d.%s)' % (attr, comp))

    def op_v_interp_mov_f32(self, ops, mods):
        # Only p0 is the flat copy. p10 (P1 - P0) and p20 (P2 - P0) need the per-vertex attribute
        # values, which the FXC path does not have, so they are refused rather than read as p0.
        if ops[1] != 'p0':
            raise Unsupported('v_interp_mov_f32 parameter selector %s: only p0 is lowered' % ops[1])
        attr, comp = self.note_attr(ops[2], 'flat')
        self.set_v(reg_range(ops[0])[1], 'i.fparam%d.%s' % (attr, comp))

    def interp_f16_mods(self, name, mods):
        if any(m != 'high' for m in mods):
            raise Unsupported('%s with %s' % (name, ' '.join(mods)))
        return 'hi' if 'high' in mods else 'lo'

    def op_v_interp_p1ll_f16(self, ops, mods):
        # First pass of a binary16 attribute (Mesa ACO emit_interp_instr: p1ll then p2_f16). Like p1_f32 it
        # produces no value of its own here: the interpolated half comes from p2.
        self.interp_f16_mods('v_interp_p1ll_f16', mods)
        if len(ops) != 3:
            raise Unsupported('v_interp_p1ll_f16 with %d operands' % len(ops))
        self.note_attr(ops[2], 'half')

    def op_v_interp_p2_f16(self, ops, mods):
        """The f16 attribute half (`high`: bits 31:16 of the exported word) interpolated, rounded to binary16.

        The vertex/mesh stage exports a second and third interpolated copy of the attribute, each half converted to
        f32 (loparamN / hiparamN, see export); the result is f32tof16 of the interpolant. The hardware
        interpolates in f32 and rounds once, as this does (LLVM SIInstrInfo / ACO: p1ll is f32 out, p2 f16 out).
        A 16-bit write: the other half of the VGPR is kept (Mesa aco_ir.cpp instr_is_16bit lists v_interp_p2_f16
        and v_interp_p2_hi_f16 for GFX9+). op_sel bit 3 (VOP3 word 0 bit 14, which LLVM 15 and the decoder do not
        print) writes the high half: Mesa aco_assembler.cpp emits v_interp_p2_hi_f16 as v_interp_p2_f16 with
        opsel 0x8. Other op_sel bits are refused."""
        half = self.interp_f16_mods('v_interp_p2_f16', mods)
        if len(ops) != 4:
            raise Unsupported('v_interp_p2_f16 with %d operands' % len(ops))
        attr, comp = self.note_attr(ops[2], 'half')
        opsel = struct.unpack_from('<I', self.raw, self.pc)[0] >> 11 & 15
        if opsel not in (0, 8):
            raise Unsupported('v_interp_p2_f16 with op_sel %#x' % opsel)
        d = reg_range(ops[0])[1]
        self.used_v.add(d)
        value = 'f32tof16(i.%sparam%d.%s)' % (half, attr, comp)
        if opsel:
            self.set_v(d, '(v%d & 0xffffu) | (%s << 16)' % (d, value))
        else:
            self.set_v(d, '(v%d & 0xffff0000u) | %s' % (d, value))

    def note_attr(self, tok, how):
        m = re.fullmatch(r'attr(\d+)\.([xyzw])', tok)
        attr = int(m.group(1))
        self.attrs.setdefault(attr, set()).add(how)
        return attr, m.group(2)

    def mix(self, ops, mods, dest):
        """v_fma_mix{_f32,lo_f16,hi_f16}: fma of operands that are f32 or f16 halves.

        LLVM AMDGPUISelDAGToDAG::SelectVOP3PMadMixModsImpl: op_sel_hi[i] makes operand i an f16 converted to f32
        and op_sel[i] then picks its high half; abs and neg act on the converted value (abs first). The result is
        f32 (dest 'f32') or rounds to f16 in the low or high half, the other half kept.
        """
        sel, high = [0, 0, 0], [0, 0, 0]
        for m in mods:
            if m.startswith('op_sel_hi:'):
                sel = [int(x) for x in m[len('op_sel_hi:['):-1].split(',')]
            elif m.startswith('op_sel:'):
                high = [int(x) for x in m[len('op_sel:['):-1].split(',')]
            elif m != 'clamp':
                raise Unsupported('modifier %s on fma_mix' % m)
        if len(ops) != 4 or len(sel) != 3 or len(high) != 3:
            raise Unsupported('fma_mix form %s %s' % (ops, mods))
        def operand(tok, is_half, is_high):
            if not is_half:
                if is_high:
                    raise Unsupported('op_sel on an f32 fma_mix operand')
                if tok.lstrip('-') == '0.15915494':
                    raise Unsupported('fma_mix inline constant 1/(2*pi)')
                # "-0" is the inline 0 with the neg modifier: -0.0 (f() reads it as +0.0)
                return '(-0.0)' if tok == '-0' else self.f(tok)
            if tok in ('0', '-0'):
                return '0.0' if tok == '0' else '(-0.0)'
            if tok in FLOATS and not is_high and tok != '0.15915494':
                # An inline float constant read as an f16 operand is that constant's binary16 form: 0.5 is 0x3800,
                # not the low half of the f32 0x3f000000 (LLVM VOP3PInstructions.td, VOP3P_Mix_Profile: "The inline
                # immediate values are really f16 converted to f32"; its fadd pattern passes FP16_ONE with op_sel_hi).
                # 1/(2*pi) is refused: its f16 and f32 forms differ and only the f16 one is documented here.
                return 'f16tof32(%s)' % self.h(tok)
            neg = tok.startswith('-')
            body = tok[1:] if neg else tok
            absolute = body.startswith('|') and body.endswith('|')
            if absolute:
                body = body[1:-1]
            r = reg_range(body)
            if re.fullmatch(r'0x[0-9a-f]{1,8}', body):
                # A 32-bit literal read as an f16 operand: op_sel picks its half as for a register (LLVM
                # SIFoldOperands::tryFoldImmWithOpSel reads VOP3P literal halves as ImmVal >> (OP_SEL_0 ? 16 : 0)).
                value = int(body, 16)
                text = 'f16tof32(0x%04xu)' % ((value >> 16) if is_high else (value & 0xffff))
            elif not r or r[1] != r[2]:
                raise Unsupported('f16 fma_mix operand %s' % tok)
            else:
                text = 'f16tof32(%s%s)' % (self.u(body), ' >> 16' if is_high else '')
            if absolute:
                text = 'abs(%s)' % text
            return '(-%s)' % text if neg else text
        expr = '%s * %s + %s' % tuple(operand(t, s, h) for t, s, h in zip(ops[1:4], sel, high))
        if 'clamp' in mods:
            expr = 'saturate(%s)' % expr
        d = reg_range(ops[0])[1]
        if dest == 'f32':
            return self.set_v(d, 'U(%s)' % expr)
        self.used_v.add(d)
        if dest == 'hi':
            self.set_v(d, '(v%d & 0xffffu) | (f32tof16(%s) << 16)' % (d, expr))
        else:
            self.set_v(d, '(v%d & 0xffff0000u) | f32tof16(%s)' % (d, expr))

    def op_v_fma_mix_f32(self, ops, mods):
        self.mix(ops, mods, 'f32')

    def op_v_fma_mixlo_f16(self, ops, mods):
        self.mix(ops, mods, 'lo')

    def op_v_fma_mixhi_f16(self, ops, mods):
        self.mix(ops, mods, 'hi')

    def op_v_cvt_pkrtz_f16_f32(self, ops, mods):
        self.set_v(reg_range(ops[0])[1], 'f32tof16(%s) | (f32tof16(%s) << 16)' % (self.f(ops[1]), self.f(ops[2])))

    def pknorm(self, ops, mods, kind):
        # S0 packs into D[15:0] and S1 into D[31:16] (per Mesa ACO selection: pack_[us]norm_2x16 of (x, y) is
        # v_cvt_pknorm_[ui]16_f32 x, y); each half is x2w_pknorm_[ui]16, see HELPERS. Source neg/abs apply first.
        if mods or len(ops) != 3:
            raise Unsupported('v_cvt_pknorm_%s16_f32 with %s' % (kind, mods or ops))
        helper = 'x2w_pknorm_%s16' % kind
        self.set_v(reg_range(ops[0])[1], '%s(%s) | (%s(%s) << 16)' % (helper, self.f(ops[1]), helper, self.f(ops[2])))

    def op_v_cvt_pknorm_u16_f32(self, ops, mods):
        self.pknorm(ops, mods, 'u')

    def op_v_cvt_pknorm_i16_f32(self, ops, mods):
        self.pknorm(ops, mods, 'i')

    def compare(self, base, ops, mods=()):
        m = re.fullmatch(r'v_cmp(x?)_(\w+?)_(f32|u32|i32|f16)(_sdwa)?', base)
        if not m or (m.group(4) and m.group(3) not in ('i32', 'u32') and (m.group(3), m.group(2)) != ('f32', 'class')) \
                or (m.group(3) == 'f16' and m.group(2) != 'class'):
            if m and m.group(4) and m.group(3) == 'f32':
                # The f32 predicates are verified on the plain and e64 forms only (#1920); the SDWA forms
                # stay refused until they are verified too.
                raise Unsupported('%s: f32 SDWA compares are not lowered' % base)
            raise Unsupported(base)
        writes_exec, cond, typ = m.group(1) == 'x', m.group(2), m.group(3)
        if len(ops) == 3:
            dst, a, b = ops
        elif len(ops) == 2:
            dst, (a, b) = ('exec' if writes_exec else 'vcc'), ops
        else:
            raise Unsupported('%s with %d operands' % (base, len(ops)))
        if m.group(4):
            # VOPC SDWA on integers: each source is its selected field (zero- or, with sext(), sign-extended)
            # compared as a 32-bit value; the result goes to vcc, an SGPR mask or (cmpx) exec as for e32.
            fields = dict(mod.partition(':')[::2] for mod in mods)
            if set(fields) != {'src0_sel', 'src1_sel'} or any(v not in SDWA_SEL for v in fields.values()):
                raise Unsupported('%s with %s' % (base, mods))
            if cond == 'class':
                # v_cmp_class_f32 SDWA: S0 is the f32 value (whole dword only; neg()/abs() act on its sign bit as
                # for the e64 form), S1 the integer class mask read through its selection (a compiler uses
                # src1_sel:BYTE_n to test bits of one byte against constant classes such as -0.0).
                if fields['src0_sel'] != 'DWORD':
                    raise Unsupported('%s with a sub-dword f32 value' % base)
                mm = re.fullmatch(r'(neg|abs)\((.*)\)', a)
                value = self.u(mm.group(2) if mm else a)
                if mm:
                    value = '(%s %s)' % (value, '^ 0x80000000u' if mm.group(1) == 'neg' else '& 0x7fffffffu')
                if b.startswith(('sext(', 'neg(', 'abs(')):
                    raise Unsupported('%s with a modified class mask %s' % (base, b))
                expr = 'x2w_class_f32(%s, %s)' % (value, sdwa_source(self.u(b), fields['src1_sel'], False))
                return self.write_compare(writes_exec, dst, expr)
            words = []
            for token, key in ((a, 'src0_sel'), (b, 'src1_sel')):
                sext = token.startswith('sext(') and token.endswith(')')
                words.append(sdwa_source(self.u(token[5:-1] if sext else token), fields[key], sext))
            x, y = words if typ == 'u32' else ['int(%s)' % w for w in words]
            return self.compare_result(base, cond, writes_exec, dst, x, y)
        elif mods:
            raise Unsupported('%s with %s' % (base, mods))
        if cond == 'class':
            # v_cmp_class: S0 is the value, S1 the class mask (llvm.amdgcn.class(x, mask)); neg/abs on S0 act on
            # its sign bit, and an f16 value sits in the low half with inline float constants in half form.
            if typ == 'f16':
                value = self.u_mod(a, 0x8000) if reg_range(a.strip('-|')) else self.h(a)
                expr = 'x2w_class_f16(%s, %s)' % (value, self.u(b))
            else:
                expr = 'x2w_class_f32(%s, %s)' % (self.u_mod(a), self.u(b))
            return self.write_compare(writes_exec, dst, expr)
        if typ == 'f32':
            x, y = self.f(a), self.f(b)
        elif typ == 'i32':
            x, y = 'int(%s)' % self.u(a), 'int(%s)' % self.u(b)
        else:
            x, y = self.u(a), self.u(b)
        ordered = ''
        if typ == 'f32':
            def word(tok):
                # A float source's word before its neg/abs modifiers, as f() reads them; they keep a NaN a NaN.
                if tok.startswith('-') and not re.match(r'-[\d.]', tok):
                    tok = tok[1:]
                if tok.startswith('|') and tok.endswith('|'):
                    tok = tok[1:-1]
                return self.u(tok)
            ordered = ' && '.join('(%s & 0x7fffffffu) <= 0x7f800000u' % word(t) for t in (a, b))
        return self.compare_result(base, cond, writes_exec, dst, x, y, ordered)

    def compare_result(self, base, cond, writes_exec, dst, x, y, ordered=''):
        # f32 lg is ordered and not equal (false when either source is NaN) and nlg its negation (#1920).
        # HLSL != is unordered, so lg reads as less or greater, behind {2}: both source words not NaN,
        # tested on their bits. DXC marks float compares fast unless -Gis, and a driver may then fold
        # less-or-greater into an unordered not-equal.
        # FXC without /Gis compiles !(a < b) to a >= b, false on NaN, and DXC without -Gis folds x != x
        # and x == x to constants (#1936). So nlt, nle, ngt and nge negate an ordered relation behind {2},
        # and o and u read {2} alone; the startup cache compiles with FXC and no flags.
        table = {'lt': '{0} < {1}', 'eq': '{0} == {1}', 'le': '{0} <= {1}', 'gt': '{0} > {1}',
                 'lg': '({2} && ({0} < {1} || {0} > {1}))', 'ne': '{0} != {1}',
                 'ge': '{0} >= {1}', 'neq': '!({0} == {1})', 'nlt': '!({2} && {0} < {1})',
                 'nle': '!({2} && {0} <= {1})', 'ngt': '!({2} && {0} > {1})', 'nge': '!({2} && {0} >= {1})',
                 'nlg': '!({2} && ({0} < {1} || {0} > {1}))', 'o': '({2})', 'u': '!({2})',
                 't': 'true', 'f': 'false'}
        if cond not in table:
            raise Unsupported(base)
        if '{2}' in table[cond] and not ordered:
            # Only the plain and e64 f32 forms supply the source words the ordered test needs.
            raise Unsupported('%s: no f32 source words for the ordered test' % base)
        return self.write_compare(writes_exec, dst, '((%s) ? 1u : 0u)' % table[cond].format(x, y, ordered))

    def write_compare(self, writes_exec, dst, expr):
        """Store a 0/1 compare result as the lane's bit of exec, vcc or an SGPR mask."""
        if not self.exec_known:
            expr = '(exec & %s)' % expr
        if writes_exec or dst in ('exec', 'exec_lo'):
            self.exec_known = False
            self.emit('exec = %s;' % expr)
        elif dst in ('vcc', 'vcc_lo'):
            self.emit('vcc = %s;' % expr)
        else:
            d = reg_range(dst)[1]
            self.set_s(d, expr)
            self.masks.add(d)

    def export(self, asm):
        m = re.fullmatch(r'exp (\w+) (.*?)((?: done| compr| vm)*)', asm)
        if not m:
            # e.g. a compressed export with an unpaired enable mask, which the disassembler prints as en:<mask>
            raise Unsupported('export form %r' % asm)
        target, vals, flags = m.group(1), [v.strip() for v in m.group(2).split(',')], m.group(3).split()
        if 'vm' in flags and not self.exec_known:
            self.emit('if (!(exec & 1)) discard;')
        if target == 'null':
            # The null target carries no data; titles use it to end a pixel stage (done, vm) that exports nothing
            # else, so only the valid-mask discard above applies.
            if any(v != 'off' for v in vals) or self.kind != 'ps':
                raise Unsupported('null export with data: %s' % asm)
            return None
        if target == 'mrtz':
            # Z export: x is depth, y stencil, z the sample mask, w alpha for alpha-to-coverage. Only a depth-only
            # export (SPI_SHADER_32_R) has an HLSL output, SV_Depth; the others are refused by channel.
            if self.kind != 'ps' or 'compr' in flags or vals[0] == 'off' or any(v != 'off' for v in vals[1:]):
                raise Unsupported('mrtz export other than depth alone: %s' % asm)
            self.exports['depth'] = True
            self.emit('o.depth = F(%s);' % self.u(vals[0]))
            return None
        if target == 'pos1' and self.kind == 'vs' and getattr(self.pipe, 'vs_out_cntl', None) is not None \
                and 'compr' not in flags:
            # With VS_OUT_MISC_VEC_ENA clear and VS_OUT_CCDIST0_VEC_ENA set, the export after pos0 is the first
            # clip/cull distance vector (see clip_vector_width); D3D: SV_ClipDistance0.
            n = clip_vector_width(self.pipe.vs_out_cntl, vals)
            self.exports['clip'] = n
            parts = ', '.join('F(%s)' % self.u(v) for v in vals[:n])
            self.emit('o.clip0 = %s;' % (parts if n == 1 else 'float%d(%s)' % (n, parts)))
            return None
        name = {'pos0': 'pos'}.get(target, target)
        if not re.fullmatch(r'pos|param\d+|mrt[0-7]', name):
            raise Unsupported('export target %s' % target)
        kind = self.pipe.mrt_kind(int(name[3:])) if name.startswith('mrt') else 'float'
        if 'compr' in flags:
            if kind in ('uint', 'int'):
                raise Unsupported('packed integer export %s' % name)
            # A compressed export takes x, y from the two halves of its first register and z, w from the second.
            # The enable mask is per pair (0x3 and 0xc), and the disassembler prints each enabled register twice
            # and a disabled pair as "off, off"; a disabled pair reads 0 like an "off" channel of a plain export.
            if vals[0] != vals[1] or vals[2] != vals[3]:
                raise Unsupported('compressed export operands %s' % ', '.join(vals))
            exprs = []
            for v in (vals[0], vals[2]):
                if v == 'off':
                    exprs += ['0.0', '0.0']
                else:
                    word = self.u(v)
                    exprs += ['f16tof32(%s)' % word, 'f16tof32(%s >> 16)' % word]
            words = ['0u' if e == '0.0' else 'U(%s)' % e for e in exprs]
        elif kind in ('uint', 'int'):
            exprs = ['0u' if v == 'off' else self.u(v) for v in vals]
            words = exprs
        elif kind == 'bits':
            ud = self.pipe.ud(('mrtbits',))
            exprs = ['0.0' if v == 'off' else 'F((%s) ^ ud%d)' % (self.u(v), ud) for v in vals]
            words = None
        else:
            exprs = ['0.0' if v == 'off' else 'F(%s)' % self.u(v) for v in vals]
            words = ['0u' if v == 'off' else self.u(v) for v in vals]
        self.exports[name] = True
        expr = '%s(%s)' % ('uint4' if kind in ('uint', 'int') else 'float4', ', '.join(exprs))
        self.emit('o.%s = %s;' % (name, 'asint(%s)' % expr if kind == 'int' else expr))
        if name.startswith('param'):
            # The flat copy read by v_interp_mov_f32 carries the register words. A float copy would let
            # FXC fold asfloat() of a constant and flush integer bit patterns that are binary32
            # denormals (#1259). hlsl_vs() drops the line for attributes the pixel stage reads only
            # interpolated.
            self.emit('o.f%s = uint4(%s);' % (name, ', '.join(words)))
            if 'compr' in flags:
                # The words of a compressed export are not the registers' halves (each half went to its own
                # channel), so a binary16 read of this attribute is refused (Pipeline.check_half_reads).
                self.__dict__.setdefault('compr_params', set()).add(int(name[5:]))
                return None
            # The binary16 halves of each word as f32, interpolated for v_interp_p2_f16 (the hardware interpolates
            # each half of the parameter word). Dropped like the flat copy when the pixel stage does not read them.
            self.emit('o.lo%s = float4(%s);' % (name, ', '.join(
                '0.0' if w == '0u' else 'f16tof32(%s)' % w for w in words)))
            self.emit('o.hi%s = float4(%s);' % (name, ', '.join(
                '0.0' if w == '0u' else 'f16tof32((%s) >> 16)' % w for w in words)))
        return None

    # ---- output ------------------------------------------------------------------------------
    def body(self):
        """Statements of the whole program as structured HLSL."""
        out = []
        offsets = [b[0] for b in self.blocks]
        index = {off: i for i, off in enumerate(offsets)}
        loops = any(t and t[0] in ('jump', 'branch') and t[-1] <= b[0] for b in self.blocks for t in [b[2]])
        if len(self.blocks) == 1:
            return ['    ' + line for line in self.blocks[0][1]]

        def goto(off):
            if off not in index:
                raise Unsupported('branch into the middle of a block (+0x%x)' % off)
            return index[off]
        out.append('    uint pc = 0;')
        if loops:
            out.append('    [loop] for (uint guard = 0; guard < 65536 && pc != 0xffffu; ++guard) {')
        for i, (off, lines, term) in enumerate(self.blocks):
            out.append('    %sif (pc == %d) {' % ('else ' if loops and i else '', i))
            out += ['        ' + line for line in lines]
            nxt = i + 1 if i + 1 < len(self.blocks) else 0xffff
            if not term:
                out.append('        pc = %d;' % nxt)
            elif term[0] == 'end':
                out.append('        pc = 0xffffu;')
            elif term[0] == 'jump':
                out.append('        pc = %d;' % goto(term[1]))
            else:
                out.append('        pc = (%s) ? %d : %d;' % (term[1], goto(term[2]), nxt))
            out.append('    }')
        if loops:
            out.append('    }')
        return out

    def declarations(self):
        lines = []
        if self.used_s:
            lines.append('    uint %s;' % ', '.join('s%d = 0' % n for n in sorted(self.used_s)))
        if self.used_v:
            lines.append('    uint %s;' % ', '.join('v%d = 0' % n for n in sorted(self.used_v)))
        if getattr(self, 'shadow_v', None):
            lines.append('    uint %s;' % ', '.join('xv%d = 0' % n for n in sorted(self.shadow_v)))
        packed = getattr(self.pipe, 'packed_position_vgpr', None)  # the tests' bare pipelines have none
        if self.kind == 'ps' and packed is not None and packed in self.used_v:
            lines.append('    v%d = (uint(i.pos.x) & 0xffffu) | ((uint(i.pos.y) & 0xffffu) << 16);' % packed)
        if self.kind == 'ps':
            for reg, bit in sorted(getattr(self.pipe, 'ps_input_vgprs', {}).items()):
                lines.append('    v%d = %s;  // SPI input %s' % (reg, PS_INPUT_HLSL[bit], PS_INPUTS[bit][0]))
        lines.append('    uint vcc = 0, exec = 1, m0 = 0, a = 0, w = 0; bool scc = false; float4 t = 0;')
        if self.used_h:
            lines.append('    uint %s;' % ', '.join('h%d = 0' % k for k in sorted(self.used_h)))
        if self.uses_q:
            lines.append('    uint2 q = 0;')
        if self.uses_swap:
            lines.append('    uint xswap = 0;')
        return lines


# Identifiers Stage.lane_invariant accepts in an expression besides registers: pure HLSL functions and the
# translation's own pure helpers (x2w_* are accepted by prefix), the buffer Load method and literals.
LANE_INVARIANT_NAMES = frozenset((
    'F', 'U', 'asuint', 'asfloat', 'asint', 'uint', 'int', 'float', 'uint2', 'float2', 'float3', 'float4',
    'min', 'max', 'abs', 'saturate', 'sqrt', 'rsqrt', 'exp2', 'log2', 'floor', 'ceil', 'trunc', 'frac', 'round',
    'sin', 'cos', 'countbits', 'firstbitlow', 'firstbithigh', 'reversebits', 'f16tof32', 'f32tof16',
    'mul_legacy', 'med3', 'clamp', 'Load', 'true', 'false'))
# name -> (source operand count, expression); operands are floats
VEC_F = {
    'v_add_f32': (2, '{0} + {1}'), 'v_sub_f32': (2, '{0} - {1}'), 'v_subrev_f32': (2, '{1} - {0}'),
    'v_mul_f32': (2, '{0} * {1}'), 'v_mul_legacy_f32': (2, 'mul_legacy({0}, {1})'),
    'v_min_f32': (2, 'min({0}, {1})'), 'v_max_f32': (2, 'max({0}, {1})'),
    'v_fmac_f32': (2, '{0} * {1} + {2}'), 'v_mac_f32': (2, '{0} * {1} + {2}'),
    'v_fma_f32': (3, '{0} * {1} + {2}'), 'v_mad_f32': (3, '{0} * {1} + {2}'), 'v_mad_legacy_f32': (3, 'mul_legacy({0}, {1}) + {2}'),
    'v_fmamk_f32': (3, '{0} * {1} + {2}'), 'v_fmaak_f32': (3, '{0} * {1} + {2}'),
    'v_madmk_f32': (3, '{0} * {1} + {2}'), 'v_madak_f32': (3, '{0} * {1} + {2}'),
    'v_rcp_f32': (1, '1.0 / {0}'), 'v_rcp_iflag_f32': (1, '1.0 / {0}'), 'v_rsq_f32': (1, 'rsqrt({0})'), 'v_sqrt_f32': (1, 'sqrt({0})'),
    'v_exp_f32': (1, 'exp2({0})'), 'v_log_f32': (1, 'log2({0})'), 'v_floor_f32': (1, 'floor({0})'), 'v_ceil_f32': (1, 'ceil({0})'),
    'v_trunc_f32': (1, 'trunc({0})'), 'v_fract_f32': (1, 'frac({0})'), 'v_rndne_f32': (1, 'round({0})'),
    'v_sin_f32': (1, 'sin({0} * 6.283185307)'), 'v_cos_f32': (1, 'cos({0} * 6.283185307)'),
    'v_med3_f32': (3, 'med3({0}, {1}, {2})'), 'v_min3_f32': (3, 'min({0}, min({1}, {2}))'), 'v_max3_f32': (3, 'max({0}, max({1}, {2}))'),
    # Cube-map face selection; x2w_cube() returns (face id, sc, tc, 2 * major axis), see HELPERS.
    'v_cubeid_f32': (3, 'x2w_cube({0}, {1}, {2}).x'), 'v_cubesc_f32': (3, 'x2w_cube({0}, {1}, {2}).y'),
    'v_cubetc_f32': (3, 'x2w_cube({0}, {1}, {2}).z'), 'v_cubema_f32': (3, 'x2w_cube({0}, {1}, {2}).w'),
}
# Conversions whose result is binary32. The expression is the float value, so an output modifier
# (clamp, mul:2, mul:4, div:2) can wrap it the way it wraps any other float result. The uint-result
# conversions stay in VEC_U.
VEC_U_TO_F = {
    'v_cvt_f32_u32': (1, 'float({0})'), 'v_cvt_f32_i32': (1, 'float(int({0}))'),
    'v_cvt_f32_ubyte0': (1, 'float({0} & 255)'), 'v_cvt_f32_ubyte1': (1, 'float(({0} >> 8) & 255)'),
    'v_cvt_f32_ubyte2': (1, 'float(({0} >> 16) & 255)'), 'v_cvt_f32_ubyte3': (1, 'float({0} >> 24)'),
    'v_cvt_f32_f16': (1, 'f16tof32({0})'),
}
# operands are uints; the result is a uint expression
VEC_U = {
    'v_add_nc_u32': (2, '{0} + {1}'), 'v_sub_nc_u32': (2, '{0} - {1}'), 'v_subrev_nc_u32': (2, '{1} - {0}'),
    'v_mul_lo_u32': (2, '{0} * {1}'), 'v_and_b32': (2, '{0} & {1}'), 'v_or_b32': (2, '{0} | {1}'), 'v_xor_b32': (2, '{0} ^ {1}'),
    'v_not_b32': (1, '~{0}'), 'v_lshlrev_b32': (2, '{1} << ({0} & 31)'), 'v_lshrrev_b32': (2, '{1} >> ({0} & 31)'),
    'v_ashrrev_i32': (2, 'uint(int({1}) >> ({0} & 31))'), 'v_min_u32': (2, 'min({0}, {1})'), 'v_max_u32': (2, 'max({0}, {1})'),
    'v_min_i32': (2, 'uint(min(int({0}), int({1})))'), 'v_max_i32': (2, 'uint(max(int({0}), int({1})))'),
    # Three-operand integer forms (RDNA2 ISA: V_MIN3/V_MAX3/V_MED3 _I32/_U32; median = max(min(a, b), min(max(a, b), c)),
    # the identity the f32 med3 helper uses).
    'v_min3_u32': (3, 'min({0}, min({1}, {2}))'), 'v_max3_u32': (3, 'max({0}, max({1}, {2}))'),
    'v_min3_i32': (3, 'uint(min(int({0}), min(int({1}), int({2}))))'),
    'v_max3_i32': (3, 'uint(max(int({0}), max(int({1}), int({2}))))'),
    'v_med3_u32': (3, 'max(min({0}, {1}), min(max({0}, {1}), {2}))'),
    'v_med3_i32': (3, 'uint(max(min(int({0}), int({1})), min(max(int({0}), int({1})), int({2}))))'),
    'v_cvt_u32_f32': (1, 'uint(max(F({0}), 0.0))'),
    'v_cvt_f16_f32': (1, 'f32tof16(F({0}))'),
    'v_bfe_u32': (3, '({0} >> ({1} & 31)) & ((1u << ({2} & 31)) - 1)'), 'v_mad_u32_u24': (3, '({0} & 0xffffffu) * ({1} & 0xffffffu) + {2}'),
    'v_lshl_add_u32': (3, '({0} << ({1} & 31)) + {2}'), 'v_add_lshl_u32': (3, '({0} + {1}) << ({2} & 31)'), 'v_add3_u32': (3, '{0} + {1} + {2}'),
    'v_or3_b32': (3, '{0} | {1} | {2}'), 'v_xor3_b32': (3, '{0} ^ {1} ^ {2}'),'v_and_or_b32': (3, '({0} & {1}) | {2}'), 'v_lshl_or_b32': (3, '({0} << ({1} & 31)) | {2}'),
    'v_xad_u32': (3, '({0} ^ {1}) + {2}'), 'v_bfi_b32': (3, '({0} & {1}) | (~{0} & {2})'),
    'v_mul_u32_u24': (2, '({0} & 0xffffffu) * ({1} & 0xffffffu)'),
    # Per-lane bit operations (AMD RDNA 2 ISA reference). The bitscan results count from the most significant
    # bit for the "h" forms and read 0xffffffff when no bit qualifies; firstbitlow(0) is already 0xffffffff.
    # v_bfe_i32 extracts a field at S1[4:0] of width S2[4:0] (0 reads 0) and sign-extends it from its top bit,
    # taking the bits above bit 31 as copies of bit 31; v_bcnt_u32_b32 adds S1 to the population count of S0.
    'v_bfe_i32': (3, '(({2} & 31u) == 0u ? 0u : asuint(asint({0} << (32u - min(({1} & 31u) + ({2} & 31u), 32u))) '
                     '>> (32u - min(({1} & 31u) + ({2} & 31u), 32u) + ({1} & 31u))))'),
    'v_bcnt_u32_b32': (2, 'countbits({0}) + {1}'), 'v_ffbl_b32': (1, 'firstbitlow({0})'),
    'v_ffbh_u32': (1, '({0} == 0u ? 0xffffffffu : 31u - firstbithigh({0}))'),
    'v_ffbh_i32': (1, '({0} == 0u || {0} == 0xffffffffu ? 0xffffffffu : 31u - firstbithigh(asint({0}) < 0 ? ~{0} : {0}))'),
    'v_bfrev_b32': (1, 'reversebits({0})'),
    # The high word of the unsigned 64-bit product; shader model 5.1 has no 64-bit integers (see HELPERS).
    'v_mul_hi_u32': (2, 'x2w_mul_hi_u32({0}, {1})'),
    # D = S0[15:0] * S1[15:0] + S2 (op_sel, which picks other halves, arrives as a modifier and is refused).
    'v_mad_u32_u16': (3, '({0} & 0xffffu) * ({1} & 0xffffu) + {2}'),
}
# HLSL helpers, each emitted only by pipelines whose code calls it. The integer helpers are single expressions so
# tests/startup_gfx2hlsl_coverage_check.py can evaluate their text against plain integer references.
# x2w_mul_hi_u32: bits 63:32 of the unsigned product, from four 16-bit partial products; the carry into bit 32
# comes from the high half of the low product plus the low halves of the two cross products (each sum < 2^18).
MUL_HI_U32 = ('(a >> 16) * (b >> 16) + (((a & 0xffffu) * (b >> 16)) >> 16) + (((a >> 16) * (b & 0xffffu)) >> 16) + '
              '(((((a & 0xffffu) * (b & 0xffffu)) >> 16) + (((a & 0xffffu) * (b >> 16)) & 0xffffu) + '
              '(((a >> 16) * (b & 0xffffu)) & 0xffffu)) >> 16)')
# v_cmp_class: the bit index of the IEEE class of x in the S1 mask (LLVM SIDefines.h ClassFlags): 0 signaling NaN,
# 1 quiet NaN, 2 -inf, 3 -normal, 4 -subnormal, 5 -0, 6 +0, 7 +subnormal, 8 +normal, 9 +inf. Classified on the bits,
# so no float compare or denormal flush can change the answer.
CLASS_F32 = ('(((x >> 23) & 255u) == 255u ? ((x & 0x7fffffu) != 0u ? ((x >> 22) & 1u) : ((x >> 31) != 0u ? 2u : 9u)) : '
             '((x >> 23) & 255u) != 0u ? ((x >> 31) != 0u ? 3u : 8u) : '
             '(x & 0x7fffffu) != 0u ? ((x >> 31) != 0u ? 4u : 7u) : ((x >> 31) != 0u ? 5u : 6u))')
CLASS_F16 = ('(((x >> 10) & 31u) == 31u ? ((x & 0x3ffu) != 0u ? ((x >> 9) & 1u) : (((x >> 15) & 1u) != 0u ? 2u : 9u)) : '
             '((x >> 10) & 31u) != 0u ? (((x >> 15) & 1u) != 0u ? 3u : 8u) : '
             '(x & 0x3ffu) != 0u ? (((x >> 15) & 1u) != 0u ? 4u : 7u) : (((x >> 15) & 1u) != 0u ? 5u : 6u))')
LSHR64_LO = ('((n & 63u) == 0u ? l : (n & 63u) < 32u ? (l >> (n & 63u)) | (h << (32u - (n & 63u))) : '
             'h >> ((n & 63u) - 32u))')
LSHR64_HI = '((n & 63u) < 32u ? h >> (n & 63u) : 0u)'
LSHL64_LO = '((n & 63u) < 32u ? l << (n & 63u) : 0u)'
LSHL64_HI = ('((n & 63u) == 0u ? h : (n & 63u) < 32u ? (h << (n & 63u)) | (l >> (32u - (n & 63u))) : '
             'l << ((n & 63u) - 32u))')
HELPERS = {
    'x2w_mul_hi_u32': 'uint x2w_mul_hi_u32(uint a, uint b) { return %s; }' % MUL_HI_U32,
    'x2w_class_f32': 'uint x2w_class_f32(uint x, uint mask) { return (mask >> %s) & 1u; }' % CLASS_F32,
    'x2w_class_f16': 'uint x2w_class_f16(uint x, uint mask) { return (mask >> %s) & 1u; }' % CLASS_F16,
    # The words of a 64-bit shift of (l, h) by n[5:0]. HLSL masks shift counts to five bits, so a count of 0
    # (where 32 - n would wrap to a shift by 0) and counts of 32..63 are separate cases.
    'x2w_lshr64_lo': 'uint x2w_lshr64_lo(uint l, uint h, uint n) { return %s; }' % LSHR64_LO,
    'x2w_lshr64_hi': 'uint x2w_lshr64_hi(uint l, uint h, uint n) { return %s; }' % LSHR64_HI,
    'x2w_lshl64_lo': 'uint x2w_lshl64_lo(uint l, uint h, uint n) { return %s; }' % LSHL64_LO,
    'x2w_lshl64_hi': 'uint x2w_lshl64_hi(uint l, uint h, uint n) { return %s; }' % LSHL64_HI,
    # v_cube{id,sc,tc,ma}_f32 as LLVM's constant folder defines them (ConstantFoldAMDGCNCubeIntrinsic): the
    # major axis is z when |z| >= |x| and |z| >= |y|, else y when |y| >= |x|, else x; a negative major axis is one
    # that is below zero (not -0, not NaN). ma is twice the major-axis component.
    'x2w_cube': ('float4 x2w_cube(float a, float b, float c) { '
                 'if (abs(c) >= abs(a) && abs(c) >= abs(b)) return float4(c < 0.0 ? 5.0 : 4.0, c < 0.0 ? -a : a, -b, c + c); '
                 'if (abs(b) >= abs(a)) return float4(b < 0.0 ? 3.0 : 2.0, a, b < 0.0 ? -c : c, b + b); '
                 'return float4(a < 0.0 ? 1.0 : 0.0, a < 0.0 ? c : -c, -b, a + a); }'),
    # One 16-bit half of v_cvt_pknorm_{u16,i16}_f32, per Mesa ACO selection (Mesa main 13ebbc00,
    # src/amd/compiler/instruction_selection/aco_select_nir_alu.cpp selects nir pack_[us]norm_2x16 to these
    # opcodes) with the NIR definition (src/compiler/nir/nir_constant_expressions.py pack_[us]norm_1x16):
    # (uint16_t)(int)roundeven(CLAMP(x, lo, 1) * 65535 or 32767). Mesa's CLAMP (src/util/macros.h) is
    # "X > MIN ? (X > MAX ? MAX : X) : MIN", so NaN gives MIN: unorm 0, snorm -32767 (0x8001). The NaN case is
    # decided on the bits so no compiler min/max rewrite can change it; HLSL round() rounds half to even.
    'x2w_pknorm_u16': ('uint x2w_pknorm_u16(float x) { if ((asuint(x) & 0x7fffffffu) > 0x7f800000u) return 0u; '
                       'return uint(round(clamp(x, 0.0, 1.0) * 65535.0)); }'),
    'x2w_pknorm_i16': ('uint x2w_pknorm_i16(float x) { if ((asuint(x) & 0x7fffffffu) > 0x7f800000u) return 0x8001u; '
                       'return uint(int(round(clamp(x, -1.0, 1.0) * 32767.0))) & 0xffffu; }'),
}

# gfx10 buffer FORMAT codes (V# word 3 [18:12]) the draw runtime can synthesize for a typed buffer view
# (gfx10_view_words.h gfx10_buffer_words; raw and structured views get 22, 32_FLOAT) and their channels, x first
# from bit 0: (bit widths, numeric kind). Mixed widths list x, y, z, w as the bits rise: 10_11_11_FLOAT is x 11, y
# 11, z 10; 2_10_10_10 is x 10, y 10, z 10, w 2. Codes from Mesa's gfx10 format table, as in gfx10_view_words.h.
BUF_FORMATS = {
    1: ((8,), 'unorm'), 2: ((8,), 'snorm'), 5: ((8,), 'uint'), 6: ((8,), 'sint'),
    7: ((16,), 'unorm'), 8: ((16,), 'snorm'), 11: ((16,), 'uint'), 12: ((16,), 'sint'), 13: ((16,), 'float'),
    14: ((8, 8), 'unorm'), 15: ((8, 8), 'snorm'), 18: ((8, 8), 'uint'), 19: ((8, 8), 'sint'),
    20: ((32,), 'uint'), 21: ((32,), 'sint'), 22: ((32,), 'float'),
    23: ((16, 16), 'unorm'), 24: ((16, 16), 'snorm'), 27: ((16, 16), 'uint'), 28: ((16, 16), 'sint'),
    29: ((16, 16), 'float'),
    36: ((11, 11, 10), 'float'),
    50: ((10, 10, 10, 2), 'unorm'), 54: ((10, 10, 10, 2), 'uint'),
    56: ((8, 8, 8, 8), 'unorm'), 57: ((8, 8, 8, 8), 'snorm'), 60: ((8, 8, 8, 8), 'uint'), 61: ((8, 8, 8, 8), 'sint'),
    62: ((32, 32), 'uint'), 63: ((32, 32), 'sint'), 64: ((32, 32), 'float'),
    65: ((16,) * 4, 'unorm'), 66: ((16,) * 4, 'snorm'), 69: ((16,) * 4, 'uint'), 70: ((16,) * 4, 'sint'),
    71: ((16,) * 4, 'float'),
    72: ((32,) * 3, 'uint'), 73: ((32,) * 3, 'sint'), 74: ((32,) * 3, 'float'),
    75: ((32,) * 4, 'uint'), 76: ((32,) * 4, 'sint'), 77: ((32,) * 4, 'float'),
}


def buf_format_channel(widths, kind, k):
    """HLSL for channel k (a Python int) of an element at byte address a of ByteAddressBuffer b, as a uint."""
    pos, n = sum(widths[:k]), widths[k]
    field = 'b.Load(a + %du)' % (pos // 8) if n == 32 else \
        '((ldu(b, a + %du) >> %du) & 0x%xu)' % (4 * (pos // 32), pos % 32, (1 << n) - 1)
    if kind == 'uint' or (kind == 'float' and n == 32):
        return field
    if kind == 'sint':
        return field if n == 32 else 'uint(int(%s << %du) >> %du)' % (field, 32 - n, 32 - n)
    if kind == 'unorm':
        return 'U(float(%s) / %d.0)' % (field, (1 << n) - 1)
    if kind == 'snorm':
        return 'U(max(float(int(%s << %du) >> %du) / %d.0, -1.0))' % (field, 32 - n, 32 - n, (1 << (n - 1)) - 1)
    # binary16, and the unsigned 11-bit (5-bit exponent, 6-bit mantissa) and 10-bit (5, 5) floats, which are the
    # upper bits of a binary16 without its sign
    return 'U(f16tof32(%s))' % (field if n == 16 else '(%s << %du)' % (field, 15 - n))


def buf_format_helper():
    """x2w_fmt_load(b, a, w3, c): channel c of a buffer_load_format_* result, from V# word 3 w3 (udsrc bufword3).

    DST_SEL_X/Y/Z/W ([2:0] [5:3] [8:6] [11:9]): 0 -> 0, 1 -> 1 (1.0 for a float format, 1 for an integer one),
    4..7 -> element channel x..w. A channel the format lacks reads 0 (w reads 1, as the fill 0001); a FORMAT code
    not in BUF_FORMATS (0 = invalid, which the runtime gives for raw-copy, vertex and root sources) reads 0 in
    every channel, as do selectors 2 and 3. These fills are assumptions, not measured."""
    lines = ['uint x2w_fmt_chan(ByteAddressBuffer b, uint a, uint f, uint k, uint one) {']
    for code, (widths, kind) in sorted(BUF_FORMATS.items()):
        chans = ' '.join('if (k == %du) return %s;' % (k, buf_format_channel(widths, kind, k)) for k in range(len(widths)))
        lines.append('    if (f == %du) { %s return k == 3u ? one : 0u; }' % (code, chans))
    lines.append('    return 0u; }')
    known = ' || '.join('f == %du' % c for c in sorted(BUF_FORMATS))
    ints = ' || '.join('f == %du' % c for c, (_, kind) in sorted(BUF_FORMATS.items()) if kind in ('uint', 'sint'))
    lines.append('uint x2w_fmt_load(ByteAddressBuffer b, uint a, uint w3, uint c) {')
    lines.append('    uint f = (w3 >> 12) & 127u, s = (w3 >> (3u * c)) & 7u;')
    lines.append('    if (!(%s) || s == 0u || s == 2u || s == 3u) return 0u;' % known)
    lines.append('    uint one = (%s) ? 1u : 0x3f800000u;' % ints)
    lines.append('    return s == 1u ? one : x2w_fmt_chan(b, a, f, s - 4u, one); }')
    return '\n'.join(lines)


HELPERS['x2w_fmt_load'] = buf_format_helper()
# VEC_U conversions whose result is a binary32 value, so the VOP3 output modifiers apply to it.
FLOAT_RESULT_U = frozenset(('v_cvt_f32_u32', 'v_cvt_f32_i32', 'v_cvt_f32_ubyte0', 'v_cvt_f32_ubyte1',
                            'v_cvt_f32_ubyte2', 'v_cvt_f32_ubyte3', 'v_cvt_f32_f16'))
# scalar ops: (expression over uint sources, SCC rule with $ for the result or None)
SCA = {
    's_mov_b32': ('{0}', None), 's_mov_b64': ('{0}', None), 's_movk_i32': ('{0}', None),
    's_and_b32': ('{0} & {1}', '$ != 0'), 's_or_b32': ('{0} | {1}', '$ != 0'), 's_xor_b32': ('{0} ^ {1}', '$ != 0'),
    's_andn2_b32': ('{0} & ~{1}', '$ != 0'), 's_orn2_b32': ('{0} | ~{1}', '$ != 0'), 's_not_b32': ('~{0}', '$ != 0'),
    's_nand_b32': ('~({0} & {1})', '$ != 0'), 's_nor_b32': ('~({0} | {1})', '$ != 0'), 's_xnor_b32': ('~({0} ^ {1})', '$ != 0'),
    's_and_b64': ('{0} & {1}', '$ != 0'), 's_or_b64': ('{0} | {1}', '$ != 0'), 's_xor_b64': ('{0} ^ {1}', '$ != 0'),
    's_andn2_b64': ('{0} & ~{1}', '$ != 0'), 's_orn2_b64': ('{0} | ~{1}', '$ != 0'), 's_not_b64': ('~{0}', '$ != 0'),
    's_nand_b64': ('~({0} & {1})', '$ != 0'), 's_nor_b64': ('~({0} | {1})', '$ != 0'), 's_xnor_b64': ('~({0} ^ {1})', '$ != 0'),
    's_lshl_b32': ('{0} << ({1} & 31)', '$ != 0'), 's_lshr_b32': ('{0} >> ({1} & 31)', '$ != 0'),
    's_ashr_i32': ('uint(int({0}) >> ({1} & 31))', '$ != 0'),
    's_add_i32': ('{0} + {1}', None), 's_add_u32': ('{0} + {1}', None), 's_sub_i32': ('{0} - {1}', None), 's_sub_u32': ('{0} - {1}', None),
    's_mul_i32': ('{0} * {1}', None), 's_min_u32': ('min({0}, {1})', None), 's_max_u32': ('max({0}, {1})', None),
    's_min_i32': ('uint(min(int({0}), int({1})))', None), 's_max_i32': ('uint(max(int({0}), int({1})))', None),
    's_bfe_u32': ('({0} >> ({1} & 31)) & ((1u << (({1} >> 16) & 127)) - 1)', '$ != 0'),
    's_brev_b32': ('reversebits({0})', None),
    # s_bcnt0/1_i32_b32: the count of zero / one bits of S0; SCC = (D != 0) (AMD RDNA 2 ISA SOP1, LLVM
    # SOPInstructions.td: Defs = [SCC]).
    's_bcnt1_i32_b32': ('countbits({0})', '$ != 0'), 's_bcnt0_i32_b32': ('32u - countbits({0})', '$ != 0'),
    # s_bfm_b32: a mask of S0[4:0] ones shifted left by S1[4:0]; SCC unchanged.
    's_bfm_b32': ('(((1u << ({0} & 31)) - 1) << ({1} & 31))', None),
    's_mul_hi_u32': ('x2w_mul_hi_u32({0}, {1})', None),
    # LLVM SOPInstructions.td: s_lshlN_add_u32 selects shlN_add, (S0 << N) + S1; it defines SCC, not modelled.
    's_lshl1_add_u32': ('({0} << 1) + {1}', None), 's_lshl2_add_u32': ('({0} << 2) + {1}', None),
    's_lshl3_add_u32': ('({0} << 3) + {1}', None), 's_lshl4_add_u32': ('({0} << 4) + {1}', None),
    # s_abs_i32 is what LLVM selects for smax(x, -x) (so abs(INT_MIN) stays INT_MIN); it defines SCC, not modelled.
    's_abs_i32': ('uint(max(int({0}), -int({0})))', None),
    # s_pack_ll_b32_b16 selects build_vector(src0, src1): S0[15:0] in the low half, S1[15:0] in the high half.
    # It does not define SCC.
    's_pack_ll_b32_b16': ('({0} & 0xffffu) | ({1} << 16)', None),
    # s_pack_hh_b32_b16 selects build_vector(trunc(src0 >> 16), trunc(src1 >> 16)) (LLVM SIInstructions.td):
    # S0[31:16] in the low half, S1[31:16] in the high half. No SCC (SOP2_32 outside the Defs = [SCC] block).
    's_pack_hh_b32_b16': ('({0} >> 16) | ({1} & 0xffff0000u)', None),
}
# Scalar ops whose SCC result the translation does not compute (LLVM SOPInstructions.td lists SCC in their Defs).
# Stage.check_scc refuses a program only when that SCC can reach a reader.
SCC_UNMODELLED = frozenset(('s_add_u32', 's_add_i32', 's_sub_u32', 's_sub_i32', 's_min_u32', 's_max_u32', 's_min_i32',
                            's_max_i32', 's_abs_i32', 's_addk_i32', 's_lshl1_add_u32', 's_lshl2_add_u32',
                            's_lshl3_add_u32', 's_lshl4_add_u32', 's_lshl_b64', 's_lshr_b64', 's_ashr_i64'))
SCC_READERS = ('s_cbranch_scc0', 's_cbranch_scc1', 's_cselect_b32', 's_cselect_b64', 's_cmov_b32', 's_cmov_b64',
               's_cmovk_i32', 's_addc_u32', 's_subb_u32')


def reads_scc(name, asm):
    return re.sub(r'_e(32|64)$', '', name) in SCC_READERS or 'src_scc' in parse(asm)[1]
for _name, _op in (('eq', '=='), ('lg', '!='), ('gt', '>'), ('ge', '>='), ('lt', '<'), ('le', '<=')):
    for _typ, _cast in (('i32', 'int({0}) %s int({1})'), ('u32', '{0} %s {1}')):
        def _make(op=_op, cast=_cast):
            def handler(self, ops, mods):
                self.emit('scc = %s;' % (cast % op).format(self.u(ops[0]), self.u(ops[1])))
            return handler
        setattr(Stage, 'op_s_cmp_%s_%s' % (_name, _typ), _make())
        setattr(Stage, 'op_s_cmpk_%s_%s' % (_name, _typ), _make())


def parse_root(blob):
    if not blob or blob[:4] != b'DXBC':
        raise Unsupported('root signature blob missing')

    def read(fmt, data, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise Unsupported('root signature blob truncated')
        return struct.unpack_from(fmt, data, offset)

    n, = read('<I', blob, 28)
    for o in read('<%dI' % n, blob, 32):
        if blob[o:o + 4] == b'RTS0':
            d = blob[o + 8:]
            ver, nparam, poff, nsamp, soff, flags = read('<6I', d, 0)
            params = []
            for i in range(nparam):
                ptype, vis, off = read('<3I', d, poff + 12 * i)
                p = dict(type=ptype, vis=vis)
                if ptype == 1:
                    p['reg'], p['space'], p['count'] = read('<3I', d, off)
                params.append(p)
            return dict(flags=flags, params=params)
    raise Unsupported('no RTS0 part')


# Run-time sampler heap indexing: an s_load from the sampler heap with a register offset becomes a
# read of a bounded table, SamplerState qh[SAMPLER_HEAP_SIZE] : register(s0, space1), the form a
# desktop SM 5.1 / 6.0 shader can index at run time. Opt-in with X2W_SAMPLER_HEAP=1 until the
# stand-in binds that table; otherwise such a load keeps the default-sampler shortcut.
#
# Contract at the manifest boundary ('smpheap <count> <space> fallback <entry>'):
# - entries 0 .. SAMPLER_HEAP_FALLBACK - 1 hold the title's sampler-heap entries, entry n at byte
#   offset 16 * n;
# - entry SAMPLER_HEAP_FALLBACK is reserved: the host fills it with a fallback sampler;
# - the shader computes the entry itself (x2w_sampler_entry): a byte offset that is not a multiple
#   of 16, or that lies at or past 16 * SAMPLER_HEAP_FALLBACK, reads the fallback entry. The host
#   cannot check the offset, which is computed in the shader, so the guard is in the shader.
SAMPLER_HEAP_SIZE = 2048  # D3D12's limit for a shader-visible sampler heap
SAMPLER_HEAP_SPACE = 1
SAMPLER_HEAP_FALLBACK = SAMPLER_HEAP_SIZE - 1


# Draw lowerings that need the draw runtime to parse a manifest line, named in X2W_DRAW_FEATURES (comma list):
# heap (`heap 1`), bufword3 (`udsrc <n> bufword3 <srv>` for buffer_load_format_*), uav (`uav <i> buf table ...`
# with `udsrc <n> uavbase <i>` for buffer stores), heapwords (`heapwords 1`: a resource-heap view's synthesized
# descriptor words from vdesc, for buffer_load_format_* through a heap descriptor; needs heap and bufword3).
# Off by default until a build with the runtime side is announced.
KNOWN_DRAW_FEATURES = frozenset(('heap', 'bufword3', 'uav', 'heapwords'))


def draw_features():
    named = frozenset(f for f in os.environ.get('X2W_DRAW_FEATURES', '').replace(' ', '').split(',') if f)
    unknown = named - KNOWN_DRAW_FEATURES
    if unknown:
        raise SystemExit('X2W_DRAW_FEATURES: unknown %s' % ', '.join(sorted(unknown)))
    return named


# Resource-heap views of a draw (manifest `heap 1`, layout agreed with draw-rt): the title heap memory heapmem
# (t0 space1, a view token per 32-byte slot: HEAP_TOKEN then the view id), vinfo (t1 space1, 16 bytes per view id,
# dword 0 the buffer stride), and one array per kind over the view-id region, index 8 + 8 * id + kind for ids below
# HEAP_IDS and kind alone (an empty view) otherwise. Bounded arrays, so FXC needs no unbounded-table flag.
HEAP_TOKEN = 0x56573258
HEAP_IDS = 90000
HEAP_KINDS = {'tex2d': ('heaptex2d', 0, 'Texture2D<float4>'), 'tex2da': ('heaptex2da', 1, 'Texture2DArray<float4>'),
              'tex3d': ('heaptex3d', 2, 'Texture3D<float4>'), 'tex1d': ('heaptex1d', 3, 'Texture1D<float4>'),
              'buf': ('heapbuf', 4, 'ByteAddressBuffer')}


# DXGI_FORMAT values for integer RTVs and the binary32 float targets.
UINT_RTVS = frozenset((3, 7, 12, 17, 25, 30, 36, 42, 50, 57, 62))
SINT_RTVS = frozenset((4, 8, 14, 18, 32, 38, 43, 52, 59, 64))
FLOAT32_RTVS = frozenset((2, 6, 16, 41))


# SPI pixel-stage inputs in SPI_PS_INPUT_ENA/ADDR bit order and the VGPRs each takes (Mesa
# src/amd/common/ac_shader_util.c ac_get_fs_input_vgpr_cnt): the IJ pairs, the pull model's three, then
# one VGPR each for line stipple, POS_X/Y/Z/W_FLOAT, FRONT_FACE, ANCILLARY, SAMPLE_COVERAGE, POS_FIXED_PT.
PS_INPUTS = (('persp_sample', 2), ('persp_center', 2), ('persp_centroid', 2), ('persp_pull_model', 3),
             ('linear_sample', 2), ('linear_center', 2), ('linear_centroid', 2), ('line_stipple', 1),
             ('pos_x_float', 1), ('pos_y_float', 1), ('pos_z_float', 1), ('pos_w_float', 1),
             ('front_face', 1), ('ancillary', 1), ('sample_coverage', 1), ('pos_fixed_pt', 1))
POS_FIXED_PT = 15
# SPI inputs lowered besides POS_FIXED_PT (bit -> HLSL uint expression over PSIn i). Mesa
# ac_nir_lower_intrinsics_to_args.c reads frag_coord x/y/z from POS_X/Y/Z_FLOAT and frag_coord_w_rcp from
# POS_W_FLOAT; nir_builder.c nir_build_frag_coord makes gl_FragCoord.w = frcp(frag_coord_w_rcp), and D3D
# SV_Position.w is 1 / gl_FragCoord.w (vkd3d-shader spirv.c frag_coord_fixup), so POS_W_FLOAT is SV_Position.w.
PS_INPUT_HLSL = {8: 'U(i.pos.x)', 9: 'U(i.pos.y)', 10: 'U(i.pos.z)', 11: 'U(i.pos.w)'}
# The *_sample barycentrics: the stage runs per sample and the position inputs are at the sample, which a plain
# SV_Position input is not.
PS_SAMPLE_RATE = (1 << 0) | (1 << 4)


def ps_input_layout(ena, addr):
    """VGPR -> SPI input bit for the given SPI_PS_INPUT_ENA/ADDR, or Unsupported.

    The VGPRs follow ADDR, the inputs the code was compiled for; ENA names the ones the hardware initializes
    and must lie inside ADDR (Mesa ac_print_spi_ps_input_vgpr_list walks the ADDR bits to number v0, v1, ... and
    marks an input "initialized" only when its ENA bit is also set). Both are 16-bit masks.
    """
    if not 0 <= ena <= 0xffff or not 0 <= addr <= 0xffff or ena & ~addr:
        raise Unsupported('unverified pixel input enable/address combination %#x/%#x' % (ena, addr))
    layout, vgpr = {}, 0
    for bit, (_, width) in enumerate(PS_INPUTS):
        if addr >> bit & 1:
            for _ in range(width):
                layout[vgpr] = bit
                vgpr += 1
    return layout


class Pipeline:
    def __init__(self, vs_code, ps_code, root_blob, sampler_heap=None, rtv_formats=(), ps_hw_inputs=None,
                 features=None, vs_out_cntl=None, rw_keys=()):
        self.root = parse_root(root_blob)
        self.rw_keys = frozenset(rw_keys)  # buffers the program stores to and reads: every access through the UAV
        # PA_CL_VS_OUT_CNTL of the vertex stage (request .vsout), or None: says what a pos1 export carries.
        self.vs_out_cntl = vs_out_cntl
        # Lowerings whose manifest lines need draw-runtime support (DRAW_FEATURES); off unless named, so a
        # --requests pass never publishes a result the runtime cannot bind yet.
        self.features = draw_features() if features is None else frozenset(features)
        self.heapvars = []    # heap descriptor k -> resource kind it is used as (None until used)
        self.draw_heapwords = False  # x2w_hword read (X2W_DRAW_FEATURES heapwords, manifest `heapwords 1`)
        self.spi_inputs(ps_hw_inputs)
        self.rtv_formats = rtv_formats
        self.sampler_heap = os.environ.get('X2W_SAMPLER_HEAP') == '1' if sampler_heap is None else sampler_heap
        self.uses_sampler_heap = False
        self.resources = {}   # key -> dict(index, kind)
        self.samplers = {}    # key -> index
        self.uds = []         # entry-constant sources
        self.notes = set()    # shortcuts taken (written to the manifest as comments)
        self.modified = set()
        self.helper_text = None  # both stage bodies, read once by common() to pick the HELPERS to emit
        self.rect = self.ud(('rect',))
        self.vs = Stage(self, 'vs', vs_code)
        self.ps = Stage(self, 'ps', ps_code)
        self.spi_reads()
        self.params = sorted(int(k[5:]) for k in self.vs.exports if k.startswith('param'))
        self.flat = sorted(a for a, how in self.ps.attrs.items() if 'flat' in how)
        self.half = sorted(a for a, how in self.ps.attrs.items() if 'half' in how)
        self.check_half_reads()
        missing = [a for a in self.ps.attrs if a not in self.params]
        if missing:
            raise Unsupported('pixel stage reads attributes %s the vertex stage does not export' % missing)
        if 'pos' not in self.vs.exports:
            raise Unsupported('vertex stage exports no position')

    def spi_inputs(self, ps_hw_inputs):
        """Before the pixel stage is built: the SPI input layout from its SPI_PS_INPUT_ENA/ADDR, or none.

        Shared by the draw and mesh pipelines. With (ENA, ADDR), entry VGPRs map to SPI inputs. Only POS_FIXED_PT
        is lowered: the integer pixel X in [15:0] and Y in [31:16] (Mesa nir_intrinsics.py pixel_coord, "16-bit
        integer vec2 of the pixel X/Y in the framebuffer", which ac_nir_lower_intrinsics_to_args.c builds as
        unpack_32_2x16 of the pos_fixed_pt argument). D3D SV_Position.xy is the pixel centre, so its integer part
        is that pixel."""
        self.packed_position_vgpr = None
        self.ps_input_layout = None
        if ps_hw_inputs is not None:
            ena, addr = ps_hw_inputs
            self.ps_input_layout = layout = ps_input_layout(ena, addr)
            self.ps_input_ena = ena
            if ena >> POS_FIXED_PT & 1:
                self.packed_position_vgpr = [v for v, bit in layout.items() if bit == POS_FIXED_PT][0]

    def spi_reads(self):
        """After the pixel stage is built: check its entry-VGPR reads against the SPI input layout.

        A pixel stage starts with its SPI inputs in the VGPRs (barycentrics, position, face, ancillary data). The
        interpolation instructions name them without reading them (they take the attribute); any other read before
        a write sees 0 in the HLSL instead of the hardware input. Without the layout this is reported, not refused
        (existing fixtures read entry VGPRs as zero): ps_entry_reads, VGPR -> offset of the first read; the
        translate_to callers refuse a pipeline that has any. With the layout, a read of an input that is not
        lowered is kept in ps_input_refusal and raised by hlsl_ps(), so a pipeline can still be built to ask for its
        layout (packed_position_vgpr), but no pixel HLSL is ever written for it."""
        self.ps_entry_reads = dict(self.ps.entry_reads)
        self.ps_input_refusal = None
        self.ps_input_vgprs = {}  # entry VGPR -> PS_INPUT_HLSL bit, initialized by Stage.declarations()
        if self.ps_input_layout is None:
            return
        for reg, off in sorted(self.ps_entry_reads.items(), key=lambda item: item[1]):
            bit = self.ps_input_layout.get(reg)
            if bit is None:
                self.ps_input_refusal = ('pixel stage reads entry VGPR v%d at %#x, past the SPI inputs (ADDR allocates %d)'
                                         % (reg, off, len(self.ps_input_layout)))
            elif not self.ps_input_ena >> bit & 1:
                self.ps_input_refusal = ('pixel stage reads SPI input %s (v%d) at %#x, allocated but not enabled'
                                         % (PS_INPUTS[bit][0], reg, off))
            elif bit in PS_INPUT_HLSL and self.ps_input_ena & PS_SAMPLE_RATE:
                self.ps_input_refusal = ('pixel stage reads SPI input %s (v%d) at %#x with per-sample barycentrics '
                                         'enabled (%#x)' % (PS_INPUTS[bit][0], reg, off, self.ps_input_ena))
            elif bit in PS_INPUT_HLSL:
                self.ps_input_vgprs[reg] = bit
            elif bit != POS_FIXED_PT:
                self.ps_input_refusal = ('pixel stage reads SPI input %s (v%d) at %#x: not lowered'
                                         % (PS_INPUTS[bit][0], reg, off))
            if self.ps_input_refusal:
                break
        self.ps_entry_reads = {}  # every entry read is a lowered input (or refused above)

    def check_half_reads(self):
        """Refuse a binary16 read (v_interp_p2_f16) of an attribute the vertex/mesh stage exports with compr: its
        loparam/hiparam copies are not written (Stage.export)."""
        compr = sorted(set(self.half) & set(getattr(self.vs, 'compr_params', ())))
        if compr:
            raise Unsupported('binary16 interpolation of attribute %s exported with compr' % compr)

    def mrt_kind(self, slot):
        if slot >= len(self.rtv_formats):
            return 'float'
        fmt = self.rtv_formats[slot]
        if fmt in UINT_RTVS:
            if fmt != 42:
                raise Unsupported('unverified UINT RTV format %d' % fmt)
            return 'uint'
        if fmt in SINT_RTVS:
            if fmt != 43:
                raise Unsupported('unverified SINT RTV format %d' % fmt)
            return 'int'
        return 'bits' if fmt in FLOAT32_RTVS else 'float'

    def ud(self, source):
        if source not in self.uds:
            self.uds.append(source)
        return self.uds.index(source)

    def resource(self, key, kind):
        if key[0][0] == 'heapdyn':
            k = key[0][1]
            if key[1] not in (0, None):
                raise Unsupported('heap descriptor at word offset %s' % key[1])
            if self.heapvars[k] not in (None, kind):
                raise Unsupported('heap descriptor used as %s and %s' % (self.heapvars[k], kind))
            if kind not in HEAP_KINDS:
                raise Unsupported('heap %s' % kind)
            self.heapvars[k] = kind
            return '%s[NonUniformResourceIndex(x2w_hix(h%d, %du))]' % (HEAP_KINDS[kind][0], k, HEAP_KINDS[kind][1])
        if key in getattr(self, 'uavs', {}) or key in getattr(self, 'rw_keys', ()):
            raise Unsupported('descriptor %s read as an SRV and stored to as a UAV' % (key,))
        r = self.resources.setdefault(key, dict(index=len(self.resources), kind=kind))
        if r['kind'] != kind:
            raise Unsupported('resource %s used as %s and %s' % (key, r['kind'], kind))
        return 'r%d' % r['index']

    def uav(self, key):
        """u<i> for a raw buffer UAV (manifest `uav <i> buf ...`); indices count from 0 apart from the SRVs."""
        if key in self.resources:
            # translate_to builds the pipeline again with the key in rw_keys, so its vector loads go through the UAV
            raise StoredSrv(key, 'descriptor %s read as an SRV and stored to as a UAV' % (key,))
        uavs = self.__dict__.setdefault('uavs', {})
        if key not in uavs:
            if len(uavs) == 8:
                raise Unsupported('more than 8 UAVs')
            uavs[key] = len(uavs)
        return 'u%d' % uavs[key]

    def sampler(self, key, comparison=False):
        if not isinstance(key[0], int):
            raise Unsupported('sampler from a descriptor')
        # A comparison sampler is its own binding even when its words equal an ordinary sampler's.
        key = ('cmp',) + tuple(key) if comparison else key
        return 'q%d' % self.samplers.setdefault(key, len(self.samplers))

    def common(self):
        out = ['#define F(x) asfloat(x)', '#define U(x) asuint(x)',
               'float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }',
               'float med3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }',
               'uint ldu(ByteAddressBuffer b, uint a) { uint k = (a & 3) * 8, lo = b.Load(a & ~3u); if (k == 0) return lo; '
               'return (lo >> k) | (b.Load((a & ~3u) + 4) << (32 - k)); }']
        if self.helper_text is None:
            self.helper_text = '\n'.join(self.vs.body() + self.ps.body())
        out += [text for name, text in HELPERS.items() if re.search(r'\b%s\(' % name, self.helper_text)]
        out += ['cbuffer entry : register(b0) { %s };' % ' '.join('uint ud%d;' % i for i in range(len(self.uds)))]
        for key, r in sorted(self.resources.items(), key=lambda kv: kv[1]['index']):
            out.append('%s r%d : register(t%d);' % ({'buf': 'ByteAddressBuffer', 'tex1d': 'Texture1D<float4>', 'tex2d': 'Texture2D<float4>', 'tex2da': 'Texture2DArray<float4>', 'tex3d': 'Texture3D<float4>'}[r['kind']], r['index'], r['index']))
        for key, i in getattr(self, 'uavs', {}).items():
            out.append('RWByteAddressBuffer u%d : register(u%d);' % (i, i))
        for key, i in self.samplers.items():
            out.append('%s q%d : register(s%d);' % ('SamplerComparisonState' if key[0] == 'cmp' else 'SamplerState', i, i))
        if any(getattr(self, 'heapvars', ())):  # tests build bare pipelines without __init__
            out += ['ByteAddressBuffer heapmem : register(t0, space1);', 'ByteAddressBuffer vinfo : register(t1, space1);',
                    'uint x2w_vid(uint h) { return heapmem.Load(h) == 0x%08xu ? heapmem.Load(h + 4) : 0xffffffffu; }'
                    % HEAP_TOKEN,
                    'uint x2w_hix(uint h, uint k) { uint i = x2w_vid(h); return i < %du ? 8u + 8u * i + k : k; }' % HEAP_IDS,
                    'uint x2w_vstride(uint h) { uint i = x2w_vid(h); return i < %du ? vinfo.Load(i * 16u) : 0u; }' % HEAP_IDS]
            for kind in sorted(set(k for k in self.heapvars if k), key=lambda k: HEAP_KINDS[k][1]):
                name, slot, hlsl_type = HEAP_KINDS[kind]
                out.append('%s %s[%d] : register(t0, space%d);' % (hlsl_type, name, 8 + 8 * HEAP_IDS, 2 + slot))
            if getattr(self, 'draw_heapwords', False):
                # vdesc: the gfx10 descriptor words per view id (RUNTIME_NEEDS.md 6.3.1; text as mesh2hlsl's x2w_hword)
                out += ['ByteAddressBuffer x2w_vdesc : register(t2, space1);',
                        'uint x2w_hword(uint h, uint i) { uint id = x2w_vid(h); return id < %du ? '
                        'x2w_vdesc.Load(id * 32u + 4u * i) : 0u; }' % HEAP_IDS]
        if self.uses_sampler_heap:
            out.append('SamplerState qh[%d] : register(s0, space%d);' % (SAMPLER_HEAP_SIZE, SAMPLER_HEAP_SPACE))
            out.append('uint x2w_sampler_entry(uint offset) { return (offset & 15u) != 0 || offset >= %du ? %du : offset >> 4; }'
                       % (16 * SAMPLER_HEAP_FALLBACK, SAMPLER_HEAP_FALLBACK))
        return out

    def interface(self, for_pixel):
        fields = []
        for p in range(max(self.params) + 1 if self.params else 0):
            fields.append('    float4 param%d : PARAM%d;' % (p, p))
        # Attributes read without interpolation get a second, flat copy of the vertex output.
        for p in self.flat:
            fields.append('    nointerpolation uint4 fparam%d : FPARAM%d;' % (p, p))
        # Attributes read as binary16 halves (v_interp_p2_f16) get the two halves as interpolated f32 copies.
        for p in getattr(self, 'half', ()):
            fields.append('    float4 loparam%d : LOPARAM%d;' % (p, p))
            fields.append('    float4 hiparam%d : HIPARAM%d;' % (p, p))
        fields.append('    float4 pos : SV_Position;')
        clip = self.vs.exports.get('clip')
        if clip:
            fields.append('    float%s clip0 : SV_ClipDistance0;' % ('' if clip == 1 else clip))
        return fields

    def hlsl_vs(self):
        s = self.vs
        out = self.common() + ['struct VSOut {'] + self.interface(False) + ['};', 'VSOut body(uint vid) {', '    VSOut o = (VSOut)0;']
        s.used_v.add(0)
        flat = re.compile(r'\s*o\.(f|lo|hi)param(\d+) = ')

        def unread(line):
            m = flat.match(line)
            return m and int(m.group(2)) not in (self.flat if m.group(1) == 'f' else getattr(self, 'half', ()))
        body = [line for line in s.body() if not unread(line)]
        out += s.declarations() + ['    v0 = vid;'] + body + ['    return o;', '}',
            '// Rectangle lists arrive as six vertices per rectangle: corners 0 1 2, 0 2 and the fourth corner 0 + 2 - 1.',
            'VSOut main(uint vid : SV_VertexID) {',
            '    if (ud%d == 0) return body(vid);' % self.rect,
            '    uint first = (vid / 6) * 3, corner = vid % 6;',
            '    if (corner < 3) return body(first + corner);',
            '    if (corner == 3) return body(first);',
            '    if (corner == 4) return body(first + 2);',
            '    VSOut p = body(first), q = body(first + 1), r = body(first + 2), o;']
        for p in range(max(self.params) + 1 if self.params else 0):
            out.append('    o.param%d = p.param%d + r.param%d - q.param%d;' % (p, p, p, p))
        out += ['    o.fparam%d = p.fparam%d;' % (p, p) for p in self.flat]
        for p in getattr(self, 'half', ()):
            out += ['    o.%sparam%d = p.%sparam%d + r.%sparam%d - q.%sparam%d;' % ((h, p) * 4) for h in ('lo', 'hi')]
        if self.vs.exports.get('clip'):
            out.append('    o.clip0 = p.clip0 + r.clip0 - q.clip0;')  # linear in the corner, like the params
        out += ['    o.pos = p.pos + r.pos - q.pos;', '    return o;', '}']
        return '\n'.join(out) + '\n'

    def hlsl_ps(self):
        refusal = getattr(self, 'ps_input_refusal', None)  # the tests' bare pipelines have none
        if refusal:
            raise Unsupported(refusal)
        s = self.ps
        targets = sorted(int(k[3:]) for k in s.exports if k.startswith('mrt'))
        out = self.common() + ['struct PSIn {'] + self.interface(True) + ['};']
        depth = 'depth' in s.exports
        if not targets and not depth:  # depth-only: no output struct, so the pipeline declares no SV_Target
            out += ['void main(PSIn i) {'] + s.declarations() + s.body() + ['}']
            return '\n'.join(out) + '\n'
        out.append('struct PSOut {')
        for t in targets:
            kind = self.mrt_kind(t)
            out.append('    %s mrt%d : SV_Target%d;' % ({'int': 'int4', 'uint': 'uint4'}.get(kind, 'float4'), t, t))
        if depth:
            out.append('    float depth : SV_Depth;')
        out += ['};', 'PSOut main(PSIn i) {', '    PSOut o = (PSOut)0;']
        out += s.declarations() + s.body() + ['    return o;', '}']
        return '\n'.join(out) + '\n'

    def manifest(self):
        lines = ['x2w-manifest 1', 'ud %d' % len(self.uds)]
        for i, source in enumerate(self.uds):
            lines.append('udsrc %d %s' % (i, ' '.join(str(x) for x in source)))
        for key, r in sorted(self.resources.items(), key=lambda kv: kv[1]['index']):
            source, off = key
            where = {'table': 'table %d %d' % (source[1] if len(source) > 1 else 0, off or 0), 'vbtab': 'vb %d' % ((off or 0) // 16),
                     'root': 'root %d' % (source[1] if len(source) > 1 else 0)}.get(source[0])
            if where is None:
                raise Unsupported('resource through %s' % (source,))
            lines.append('srv %d %s %s' % (r['index'], r['kind'], where))
        for (source, off), i in sorted(getattr(self, 'uavs', {}).items(), key=lambda kv: kv[1]):
            if source[0] == 'table':
                lines.append('uav %d buf table %d %d' % (i, source[1] if len(source) > 1 else 0, off or 0))
            elif source[0] == 'root' and not off:
                lines.append('uav %d buf root %d 0' % (i, source[1] if len(source) > 1 else 0))
            else:
                raise Unsupported('uav through %s at %s' % (source, off))
        if any(getattr(self, 'heapvars', ())):
            lines.append('heap 1')
            if getattr(self, 'draw_heapwords', False):
                lines.append('heapwords 1')
        for key, i in self.samplers.items():
            comparison = key[0] == 'cmp'
            lines.append('%s %d %s' % ('smpc' if comparison else 'smp', i, ' '.join('0x%x' % w for w in key[comparison:])))
        if self.uses_sampler_heap:
            lines.append('smpheap %d %d fallback %d' % (SAMPLER_HEAP_SIZE, SAMPLER_HEAP_SPACE, SAMPLER_HEAP_FALLBACK))
        lines.append('params %d' % (max(self.params) + 1 if self.params else 0))
        lines.append('targets %s' % ' '.join(k[3:] for k in sorted(self.ps.exports) if k.startswith('mrt')))
        return '\n'.join(lines) + '\n'


def fnv(data, h=1469598103934665603):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h


def pipeline_key(vs_code, ps_code, root_blob):
    """Hash the startup translation revision and the three blobs in runtime order."""
    return fnv(root_blob, fnv(ps_code, fnv(vs_code, fnv(b'x2w-startup-shader-3'))))


DEFAULT_FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
KITS_BIN = r'C:\Program Files (x86)\Windows Kits\10\bin'


def find_fxc():
    """The fxc.exe to compile with, '' when X2W_FXC is the empty string (manifest only), or None when none exists.

    Order: X2W_FXC, the 10.0.26100.0 SDK, the newest KITS_BIN/<version>/x64, then PATH."""
    configured = os.environ.get('X2W_FXC')
    if configured == '':
        return ''
    if configured and os.path.isfile(configured):
        return configured
    if os.path.isfile(DEFAULT_FXC):
        return DEFAULT_FXC
    versions = []
    try:
        for entry in os.listdir(KITS_BIN):
            if re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', entry):
                versions.append((tuple(map(int, entry.split('.'))), entry))
    except OSError:
        pass
    for _, entry in sorted(versions, reverse=True):
        candidate = os.path.join(KITS_BIN, entry, 'x64', 'fxc.exe')
        if os.path.isfile(candidate):
            return candidate
    import shutil
    return shutil.which('fxc.exe')


def translate_to(out_dir, key, vs_code, ps_code, root_blob, check=True, rtv_formats=(), rtv_request='',
                 ps_hw_inputs=None, vs_out_cntl=None):
    """Writes <key>.vs.hlsl, <key>.ps.hlsl and <key>.txt (last), or <key>.fail with the reason.

    With check, the pair is compiled to .vs.cso and .ps.cso first and a missing fxc is a failure.
    check=False, or X2W_FXC set to the empty string, publishes the manifest without bytecode."""
    name = os.path.join(out_dir, '%016x' % key)
    try:
        rw_keys = set()
        while True:
            try:
                pipe = Pipeline(vs_code, ps_code, root_blob, rtv_formats=rtv_formats, ps_hw_inputs=ps_hw_inputs,
                                vs_out_cntl=vs_out_cntl, rw_keys=rw_keys)
                break
            except StoredSrv as conflict:
                # read before the first store: build again with every access to that buffer through its UAV
                if conflict.key in rw_keys or len(rw_keys) == 8:
                    raise
                rw_keys.add(conflict.key)
        if pipe.ps_entry_reads:
            # A request does not carry the SPI input layout, so the HLSL would read that input as 0 (for example
            # the packed pixel position a dither or a full-screen copy addresses with). Not written as a result.
            reg, off = min(pipe.ps_entry_reads.items(), key=lambda item: item[1])
            raise Unsupported('pixel stage reads entry VGPR v%d (an SPI input; layout not in the request) at %#x'
                              % (reg, off))
        vs, ps, manifest = pipe.hlsl_vs(), pipe.hlsl_ps(), pipe.manifest()
        if rtv_request:
            manifest += rtv_request + '\n'
    except Unsupported as reason:
        open(name + '.fail', 'w').write(str(reason) + '\n')
        return 'unsupported: %s' % reason
    open(name + '.vs.hlsl', 'w').write(vs)
    open(name + '.ps.hlsl', 'w').write(ps)
    fxc = find_fxc() if check else ''
    if fxc is None:
        message = 'fxc.exe not found (X2W_FXC=%s): set X2W_FXC to its path or install the Windows SDK' % os.environ.get('X2W_FXC', '<unset>')
        open(name + '.fail', 'w').write(message + '\n')
        return message
    if fxc:
        import subprocess
        for stage, profile in (('vs', 'vs_5_1'), ('ps', 'ps_5_1')):
            r = subprocess.run([fxc, '/nologo', '/T', profile, '/E', 'main', '/Fo', name + '.%s.cso' % stage, name + '.%s.hlsl' % stage],
                               capture_output=True, text=True)
            if r.returncode:
                open(name + '.fail', 'w').write('fxc %s: %s\n' % (stage, r.stderr[-2000:]))
                return 'fxc %s failed: %s' % (stage, r.stderr.strip().splitlines()[-1] if r.stderr.strip() else '?')
    temporary = '%s.txt.%d.tmp' % (name, os.getpid())
    with open(temporary, 'w') as result:
        result.write(manifest)
    os.replace(temporary, name + '.txt')
    return 'ok'


def classify(blob):
    text = ' '.join(x[3] for x in decode(strip_header(blob)) if x[2] == 'exp')
    # No export: a depth-only pixel shader. Only a pixel stage exports to the null target (a stage that ends with
    # "exp null ... done vm" and writes no colour, e.g. for its discards or depth).
    return 'vs' if 'pos' in text else 'ps' if 'mrt' in text or 'null' in text or not text else 'other'


def translate_request(out_dir, key):
    """Translate a base shader request or an RTV variant referencing its source blobs."""
    name = os.path.join(out_dir, 'req_%016x' % key)
    request = ''
    formats = ()
    try:
        base = key
        if os.path.isfile(name + '.rtv'):
            with open(name + '.rtv', encoding='ascii') as file:
                request = file.read(256)
            if not re.fullmatch(r'rtv1 [0-9]{1,20}(?: [0-9]{1,10}){1,8}', request) or fnv(request.encode('ascii')) != key:
                raise Unsupported('invalid RTV variant request')
            base, *formats = map(int, request.split()[1:])
            if base > 0xffffffffffffffff or any(fmt > 0xffffffff for fmt in formats):
                raise Unsupported('invalid RTV variant values')
            name = os.path.join(out_dir, 'req_%016x' % base)
        a, b, root = (open(name + ext, 'rb').read() for ext in ('.a', '.b', '.root'))
        if request and pipeline_key(a, b, root) != base:
            raise Unsupported('RTV variant source key mismatch')
        ps_hw_inputs = read_ps_inputs(name + '.psin')
        vs_out_cntl = read_vs_out(name + '.vsout')
        dumped = dumped_state(name) if ps_hw_inputs is None or vs_out_cntl is None else (None, None)
        if ps_hw_inputs is None and dumped[0] is not None:
            ps_hw_inputs = (dumped[0], dumped[0])  # ADDR assumed equal to ENA (no ADDR field located)
        if vs_out_cntl is None:
            vs_out_cntl = dumped[1]
        if a.startswith(b'DXBC') or b.startswith(b'DXBC'):
            # Slot 86 records complete containers in canonical (VS, PS) order.
            # Keep the container bytes in the cache key; translate only the bounded native code.
            import zlib
            from pixel_aot import container_code
            from resource_inventory import parts
            try:
                a, b = (container_code(parts(blob)[b'DXIL'], stage=stage)
                        for blob, stage in ((a, 1), (b, 0)))
            except (KeyError, ValueError, struct.error, zlib.error) as error:
                raise Unsupported('invalid stream shader: %s' % error) from error
            if classify(a) != 'vs' or classify(b) != 'ps':
                raise Unsupported('stream stage and shader export disagree')
            kinds = {'vs': a, 'ps': b}
        else:
            kinds = {classify(a): a, classify(b): b}
    except (Unsupported, OSError, UnicodeError) as reason:
        open(os.path.join(out_dir, '%016x.fail' % key), 'w').write(str(reason) + '\n')
        return 'unsupported: %s' % reason
    if set(kinds) != {'vs', 'ps'}:
        open(os.path.join(out_dir, '%016x.fail' % key), 'w').write('stages %s\n' % sorted(kinds))
        return 'stages %s' % sorted(kinds)
    return translate_to(out_dir, key, kinds['vs'], kinds['ps'], root, rtv_formats=formats, rtv_request=request,
                        ps_hw_inputs=ps_hw_inputs, vs_out_cntl=vs_out_cntl)


# Fields of the pipeline's state block A ([1]/[2] of the slot-57 record, dumped by the runtime as
# req_<key>.vsregs), located from run-84 dumps: SPI_PS_INPUT_ENA = u16 at +0x58 (1530 of 1547 dumped pixel
# requests consistent with their own VGPR use), PA_CL_VS_OUT_CNTL = u32 at +0x98 (0x01400303 on every request
# exporting pos1, low byte 0 on every other). Measured offsets, not documented ones.
VSREGS_PS_INPUT_ENA = 0x58
VSREGS_VS_OUT_CNTL = 0x98


def dumped_state(name):
    """(ENA or None, PA_CL_VS_OUT_CNTL or None) from <name>.vsregs, else from the per-state dumps
    <name>.<8 hex>.vsregs when they all agree. (None, None) when nothing is dumped."""
    paths = [name + '.vsregs'] if os.path.isfile(name + '.vsregs') else []
    if not paths:
        folder, base = os.path.split(name)
        try:
            paths = [os.path.join(folder, n) for n in os.listdir(folder or '.')
                     if re.fullmatch(re.escape(os.path.basename(base)) + r'\.[0-9a-f]{8}\.vsregs', n)]
        except OSError:
            paths = []
    enas, cntls = set(), set()
    for path in paths:
        with open(path, 'rb') as file:
            blob = file.read(0x400)
        enas.add(struct.unpack_from('<H', blob, VSREGS_PS_INPUT_ENA)[0] if len(blob) >= VSREGS_PS_INPUT_ENA + 2 else None)
        cntls.add(struct.unpack_from('<I', blob, VSREGS_VS_OUT_CNTL)[0] if len(blob) >= VSREGS_VS_OUT_CNTL + 4 else None)
    return (enas.pop() if len(enas) == 1 else None), (cntls.pop() if len(cntls) == 1 else None)


def read_vs_out(path):
    """PA_CL_VS_OUT_CNTL from a request's optional .vsout file, 'pa_cl_vs_out_cntl 1 <hex8>', or None."""
    if not os.path.isfile(path):
        return None
    with open(path, encoding='ascii') as file:
        text = file.read(64)
    m = re.fullmatch(r'pa_cl_vs_out_cntl 1 ([0-9a-f]{8})\n', text)
    if not m:
        raise Unsupported('invalid vertex output control file')
    return int(m.group(1), 16)


# PA_CL_VS_OUT_CNTL fields (Mesa src/amd/registers/gfx10.json): CLIP_DIST_ENA_0..7 [7:0], CULL_DIST_ENA_0..7 [15:8],
# USE_VTX_POINT_SIZE 16, _EDGE_FLAG 17, _RENDER_TARGET_INDX 18, _VIEWPORT_INDX 19, _KILL_FLAG 20,
# VS_OUT_MISC_VEC_ENA 21, VS_OUT_CCDIST0_VEC_ENA 22, VS_OUT_CCDIST1_VEC_ENA 23, VS_OUT_MISC_SIDE_BUS_ENA 24,
# USE_VTX_GS_CUT_FLAG 25, USE_VTX_SHD_OBJPRIM_ID 26, USE_VTX_LINE_WIDTH 27.
VS_OUT_CCDIST0 = 1 << 22
VS_OUT_SIDE_BUS = 1 << 24


def clip_vector_width(cntl, vals):
    """Channels of a pos1 export that are clip distances 0..n-1; refuses every other PA_CL_VS_OUT_CNTL use.

    Accepted only when pos1 is the first clip/cull distance vector (no misc vector, CCDIST0 on, CCDIST1 off,
    no per-vertex misc values), the written channels are x.. contiguously, each is a clip distance, and the
    cull enables are either none or the same channels. A distance that is both clip and cull distance is
    lowered as SV_ClipDistance alone: the cull removes a primitive only when every vertex is below 0, which the
    clip at 0 removes already. VS_OUT_MISC_SIDE_BUS_ENA (a transport choice) is allowed.
    """
    written = [i for i, v in enumerate(vals) if v != 'off']
    n = len(written)
    clip, cull = cntl & 0xff, cntl >> 8 & 0xff
    if (cntl & ~(0xffff | VS_OUT_CCDIST0 | VS_OUT_SIDE_BUS) or not cntl & VS_OUT_CCDIST0 or not n
            or written != list(range(n)) or clip != (1 << n) - 1 or cull not in (0, clip)):
        raise Unsupported('pos1 export with PA_CL_VS_OUT_CNTL %#010x (channels %s)' % (cntl, written))
    return n


def read_ps_inputs(path):
    """(ENA, ADDR) from a request's optional .psin file, 'spi_ps_input 1 <ENA hex8> <ADDR hex8>', or None.

    The runtime writes it beside the request (it is not part of the key); without one the pixel stage's SPI
    input layout is unknown and a read of an input VGPR is refused.
    """
    if not os.path.isfile(path):
        return None
    with open(path, encoding='ascii') as file:
        text = file.read(256)
    m = re.fullmatch(r'spi_ps_input 1 ([0-9a-f]{8}) ([0-9a-f]{8})\n(?:vs_input 1 [0-3] [0-9a-f]{8}\n)?', text)
    if not m:
        raise Unsupported('invalid SPI input file')
    return int(m.group(1), 16), int(m.group(2), 16)


def main():
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    if sys.argv[1] == '--capture':
        # --capture <dir> <capture.bin>: write request files for every pipeline the capture binds
        import capture
        cap = capture.Capture(sys.argv[3])
        done = set()
        for c in cap.calls:
            w = c['args'][0]
            if c['slot'] != 25 or w in done:
                continue
            done.add(w)
            p = cap.psos.get(w)
            seen = {w}
            while p and p['path'] == 55 and p['base']:
                if p['base'] in seen:
                    print('pso %x: wrapper chain loops back to %x' % (w & 0xffffff, p['base'] & 0xffffff))
                    p = None
                    break
                seen.add(p['base'])
                p = cap.psos.get(p['base'])
            if not p or p['path'] != 57:
                continue
            if len(p['desc']) < 96:
                print('pso %x: pipeline desc is %d bytes, need 96' % (w & 0xffffff, len(p['desc'])))
                continue
            q = struct.unpack_from('<12Q', p['desc'], 0)
            a, b = cap.memory.read(q[5], q[6]), cap.memory.read(q[9], q[10])
            root = cap.roots.get(q[0], b'')
            if not a or not b or len(a) != q[6] or len(b) != q[10]:
                print('pso %x: code not captured' % (w & 0xffffff))
                continue
            key = pipeline_key(a, b, root)
            for ext, blob in (('.a', a), ('.b', b), ('.root', root)):
                open(os.path.join(out_dir, 'req_%016x%s' % (key, ext)), 'wb').write(blob)
            print('pso %x -> request %016x' % (w & 0xffffff, key))
    # translate every request that has no result yet
    for name in sorted(os.listdir(out_dir)):
        m = re.fullmatch(r'req_([0-9a-f]{16})\.(?:a|rtv)', name)
        if not m:
            continue
        key = int(m.group(1), 16)
        if os.path.exists(os.path.join(out_dir, '%016x.txt' % key)):
            continue
        print('%016x: %s' % (key, translate_request(out_dir, key)))


if __name__ == '__main__':
    main()
