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

    def __init__(self, pipe, kind, code):
        self.pipe, self.kind = pipe, kind
        self.ins = decode(strip_header(code))
        self.lines = []           # body statements (per block)
        self.sym = {}             # scalar register -> symbolic value
        self.used_s, self.used_v = set(), set()
        self.exec_known = True    # exec still equals the entry mask (the lane is active)
        self.exports = {}         # 'pos0' / 'paramN' / 'mrtN' -> [4 expressions]
        self.attrs = {}           # pixel: attr index -> 'linear' | 'flat'
        self.uses_pos = False
        self.masks = set()        # scalar registers currently holding lane masks
        self.uses_q = False       # the uint2 temporary of the 64-bit operations
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
        self.lines.append(text)

    def set_v(self, n, expr):
        if hasattr(self, 'written_v'):
            self.written_v.add(n)
        self.uniform_v.discard(n)
        self.used_v.add(n)
        self.emit('v%d = %s;' % (n, expr) if self.exec_known else 'if (exec & 1) v%d = %s;' % (n, expr))

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
                pass
            else:
                raise Unsupported('modifier %s' % m)
        if scale != 1.0:
            factor = ('%d.0' % int(scale)) if scale == int(scale) else ('%g' % scale)
            expr = '(%s) * %s' % (expr, factor)
        if clamp:
            expr = 'saturate(%s)' % expr
        self.set_v(n, 'U(%s)' % expr)

    def set_s(self, n, expr, sym=None, scc=None):
        """Assign scalar register n at run time and/or symbolically."""
        if sym is not None:
            self.sym[n] = sym
            if sym[0] != 'const':
                return
        else:
            self.sym.pop(n, None)
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
        return dict(self.sym), set(self.masks), self.exec_known

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

    def translate(self):
        targets, sources = set(), {}
        for off, n, name, asm in self.ins:
            if name.startswith('s_cbranch') or name == 's_branch':
                target = int(parse(asm)[1][0].split('_')[1], 16)
                targets.add(target)
                sources.setdefault(target, []).append(off)
        self.blocks = []          # (offset, [lines], terminator)
        self.uniform_v = set()
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

    # VOP1/VOP2 with sub-dword addressing: select the source fields, run the same operation, then place
    # the low bits of the result in the destination field. The integer forms reuse VEC_U unchanged;
    # float forms are not lowered yet, and dst_unused UNUSED_SEXT has no lowering.
    def sdwa(self, base, ops, mods):
        fields = {}
        for m in mods:
            key, _, value = m.partition(':')
            if key in ('src0_sel', 'src1_sel', 'src2_sel', 'dst_sel', 'dst_unused') and value:
                fields[key] = value
            else:
                raise Unsupported('%s with SDWA modifier %s' % (base, m))
        if base == 'v_cndmask_b32':
            # VOP2 SDWA select: the mask is always vcc; the selected source fields are moved as bits.
            if len(ops) != 4 or ops[3] not in ('vcc', 'vcc_lo'):
                raise Unsupported('SDWA v_cndmask_b32 form %s' % ops)
            arity, text = 2, '(vcc & 1) ? {1} : {0}'
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
        # The operation's HLSL carries its own operators, so it has to be grouped before the
        # destination field masks it: "v7 | v8 & 0xffff" would otherwise bind as "v7 | (v8 & 0xffff)".
        if SDWA_SEL[select][1] < 32:
            result = '(%s)' % result
        destination = reg_range(ops[0])[1]
        self.set_v(destination, sdwa_dest(result, select, fields.get('dst_unused', 'UNUSED_PAD'),
                                          'v%d' % destination))

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
        if dname == 'm0':
            return None
        syms = [self.sym.get(reg_range(t)[1]) if reg_range(t) and reg_range(t)[0] == 's' else None for t in srcs]
        if any(s and s[0] in ('desc', 'descnum', 'ptr', 'ptrhi') for s in syms):
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
            else:
                self.sym[d] = ('descnum',)
            return None
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
            self.masks.add(d)
        else:
            self.masks.discard(d)
        self.set_s(d, expr, scc=scc)
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
        if low[0] in ('exec', 'vcc'):
            if low[0] == 'exec':
                self.exec_known = False
            self.emit('%s = (%s) & 1;' % (low[0], expr))
            return
        d = reg_range(low[0])[1]
        self.set_s(d, '(%s) & 1' % expr if pair else expr)
        if pair:
            self.masks.add(d)
            self.set_s(d + 1, '0u')

    def op_s_load_dwordx4(self, ops, mods, count=4):
        dst, base = reg_range(ops[0]), reg_range(ops[1])
        p = self.sym.get(base[1])
        if not p or p[0] != 'ptr':
            raise Unsupported('s_load through s%d holding %s' % (base[1], p))
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
        for i in range(count):
            self.set_s(dst[1] + i, '%s.Load(%s + %d)' % (buf, offset, 4 * i))
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
        buf = self.pipe.resource(key, 'buf')
        offset = int(ops[3], 0)
        for m in mods:
            if m.startswith('offset:'):
                offset += int(m[7:], 0)
        if 'idxen' in mods:
            if key in self.pipe.modified:
                raise Unsupported('indexed load through a modified descriptor')
            address = '%s * ud%d + %d' % (self.u(ops[1]), self.pipe.ud(('stride',) + (self.pipe.resources[key]['index'],)), offset)
        elif 'offen' in mods:
            address = '%s + %d' % (self.u(ops[1]), offset)
        else:
            address = '%d' % offset
        return buf, address

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
        self.emit('a = %s;' % address)
        table = {'BUF_FMT_8_8_8_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', 'U(((w >> 8) & 255) / 255.0)', 'U(((w >> 16) & 255) / 255.0)', 'U((w >> 24) / 255.0)']),
                 'BUF_FMT_8_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', 'U(((w >> 8) & 255) / 255.0)', '0u', 'U(1.0)']),
                 'BUF_FMT_8_UNORM': ('ldu({0}, a)', ['U((w & 255) / 255.0)', '0u', '0u', 'U(1.0)']),
                 'BUF_FMT_8_UINT': ('{0}.Load(a & ~3u) >> ((a & 3u) * 8u)', ['w & 255u', '0u', '0u', '1u']),
                 'BUF_FMT_16_16_UNORM': ('ldu({0}, a)', ['U((w & 65535) / 65535.0)', 'U((w >> 16) / 65535.0)', '0u', 'U(1.0)']),
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
        allowed = ('dlc', 'glc', 'slc', 'a16') + (('unorm',) if how == 'load' else ())
        for m in mods:
            if how in ('sample', 'lz', 'load') and not (m.startswith(('dim:', 'dmask:')) or m in allowed):
                raise Unsupported('image_%s modifier %s' % (how, m))
        a16 = 'a16' in mods
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
            elif how == 'lz' or not self.exec_known or self.kind != 'ps' or len(self.blocks) > 0:
                call = '%s.SampleLevel(%s, %s, 0)' % (tex, smp, uv)
            else:
                call = '%s.Sample(%s, %s)' % (tex, smp, uv)
        self.emit('t = %s;' % call)
        n = dst[1]
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
        """image_sample_c and image_sample_c_lz: a 2D depth comparison returning one float.

        The address dwords are the reference, then u and v, which is what SampleCmp* takes as its third
        argument and its location. The comparison function and the filter live in the sampler descriptor,
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
        if dim != 'SQ_RSRC_IMG_2D':
            raise Unsupported('%s on %s' % (base, dim))
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
        if len(cs) != 3:
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
        tex = self.pipe.resource(self.descriptor(ops[2]), 'tex2d')
        smp = self.pipe.sampler(tuple(words), comparison=True)
        args = '%s, float2(%s, %s), %s' % (smp, self.f(cs[1]), self.f(cs[2]), self.f(cs[0]))
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
        src, dst = reg_range(ops[1]), reg_range(ops[0])
        if (mods or self.has_branches or not self.exec_known or not dst or dst[0] != 's' or
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
        attr, comp = self.note_attr(ops[2], 'flat')
        self.set_v(reg_range(ops[0])[1], 'i.fparam%d.%s' % (attr, comp))

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
            if not r or r[1] != r[2]:
                raise Unsupported('f16 fma_mix operand %s' % tok)
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
        if not m or (m.group(4) and m.group(3) not in ('i32', 'u32')) or (m.group(3) == 'f16' and m.group(2) != 'class'):
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
        return self.compare_result(base, cond, writes_exec, dst, x, y)

    def compare_result(self, base, cond, writes_exec, dst, x, y):
        table = {'lt': '{0} < {1}', 'eq': '{0} == {1}', 'le': '{0} <= {1}', 'gt': '{0} > {1}', 'lg': '{0} != {1}', 'ne': '{0} != {1}',
                 'ge': '{0} >= {1}', 'neq': '!({0} == {1})', 'nlt': '!({0} < {1})', 'nle': '!({0} <= {1})', 'ngt': '!({0} > {1})',
                 'nge': '!({0} >= {1})', 'nlg': '!({0} != {1})', 'o': '({0} == {0} && {1} == {1})', 'u': '({0} != {0} || {1} != {1})',
                 't': 'true', 'f': 'false'}
        if cond not in table:
            raise Unsupported(base)
        return self.write_compare(writes_exec, dst, '((%s) ? 1u : 0u)' % table[cond].format(x, y))

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
        packed = self.pipe.packed_position_vgpr
        if self.kind == 'ps' and packed in self.used_v:
            lines.append('    v%d = (uint(i.pos.x) & 0xffffu) | ((uint(i.pos.y) & 0xffffu) << 16);' % packed)
        lines.append('    uint vcc = 0, exec = 1, m0 = 0, a = 0, w = 0; bool scc = false; float4 t = 0;')
        if self.uses_q:
            lines.append('    uint2 q = 0;')
        return lines


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
    'v_cvt_u32_f32': (1, 'uint(max(F({0}), 0.0))'),
    'v_cvt_f16_f32': (1, 'f32tof16(F({0}))'),
    'v_bfe_u32': (3, '({0} >> ({1} & 31)) & ((1u << ({2} & 31)) - 1)'), 'v_mad_u32_u24': (3, '({0} & 0xffffffu) * ({1} & 0xffffffu) + {2}'),
    'v_lshl_add_u32': (3, '({0} << ({1} & 31)) + {2}'), 'v_add_lshl_u32': (3, '({0} + {1}) << ({2} & 31)'), 'v_add3_u32': (3, '{0} + {1} + {2}'),
    'v_or3_b32': (3, '{0} | {1} | {2}'), 'v_and_or_b32': (3, '({0} & {1}) | {2}'), 'v_lshl_or_b32': (3, '({0} << ({1} & 31)) | {2}'),
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
# VEC_U conversions whose result is a binary32 value, so the VOP3 output modifiers apply to it.
FLOAT_RESULT_U = frozenset(('v_cvt_f32_u32', 'v_cvt_f32_i32', 'v_cvt_f32_ubyte0', 'v_cvt_f32_ubyte1',
                            'v_cvt_f32_ubyte2', 'v_cvt_f32_ubyte3', 'v_cvt_f32_f16'))
# scalar ops: (expression over uint sources, SCC rule with $ for the result or None)
SCA = {
    's_mov_b32': ('{0}', None), 's_mov_b64': ('{0}', None), 's_movk_i32': ('{0}', None),
    's_and_b32': ('{0} & {1}', '$ != 0'), 's_or_b32': ('{0} | {1}', '$ != 0'), 's_xor_b32': ('{0} ^ {1}', '$ != 0'),
    's_andn2_b32': ('{0} & ~{1}', '$ != 0'), 's_orn2_b32': ('{0} | ~{1}', '$ != 0'), 's_not_b32': ('~{0}', '$ != 0'),
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


# DXGI_FORMAT values for integer RTVs and the binary32 float targets.
UINT_RTVS = frozenset((3, 7, 12, 17, 25, 30, 36, 42, 50, 57, 62))
SINT_RTVS = frozenset((4, 8, 14, 18, 32, 38, 43, 52, 59, 64))
FLOAT32_RTVS = frozenset((2, 6, 16, 41))


class Pipeline:
    def __init__(self, vs_code, ps_code, root_blob, sampler_heap=None, rtv_formats=(), ps_hw_inputs=None):
        self.root = parse_root(root_blob)
        self.packed_position_vgpr = None
        if ps_hw_inputs is not None:
            ena, addr = ps_hw_inputs
            if ena != addr or not 0 <= ena <= 0xffff:
                raise Unsupported('unverified pixel input enable/address combination')
            if ena & (1 << 15):
                # SPI PS inputs 0..6 supply IJ pairs (the pull model supplies three);
                # line stipple through sample coverage each occupy one VGPR.
                widths = (2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1)
                self.packed_position_vgpr = sum(w for bit, w in enumerate(widths) if addr & (1 << bit))
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
        # A pixel stage starts with its SPI inputs in the VGPRs (barycentrics, position, face, ancillary data). The
        # interpolation instructions name them without reading them (they take the attribute); any other read before
        # a write sees 0 in the HLSL instead of the hardware input. Which input sits in which VGPR depends on the
        # SPI_PS_INPUT_ENA/ADDR state, which a request does not carry, so this is reported, not refused (existing
        # fixtures read entry VGPRs as zero): VGPR -> offset of the first read.
        self.ps_entry_reads = dict(self.ps.entry_reads)
        self.params = sorted(int(k[5:]) for k in self.vs.exports if k.startswith('param'))
        self.flat = sorted(a for a, how in self.ps.attrs.items() if 'flat' in how)
        missing = [a for a in self.ps.attrs if a not in self.params]
        if missing:
            raise Unsupported('pixel stage reads attributes %s the vertex stage does not export' % missing)
        if 'pos' not in self.vs.exports:
            raise Unsupported('vertex stage exports no position')

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
        r = self.resources.setdefault(key, dict(index=len(self.resources), kind=kind))
        if r['kind'] != kind:
            raise Unsupported('resource %s used as %s and %s' % (key, r['kind'], kind))
        return 'r%d' % r['index']

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
        for key, i in self.samplers.items():
            out.append('%s q%d : register(s%d);' % ('SamplerComparisonState' if key[0] == 'cmp' else 'SamplerState', i, i))
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
        fields.append('    float4 pos : SV_Position;')
        return fields

    def hlsl_vs(self):
        s = self.vs
        out = self.common() + ['struct VSOut {'] + self.interface(False) + ['};', 'VSOut body(uint vid) {', '    VSOut o = (VSOut)0;']
        s.used_v.add(0)
        flat = re.compile(r'\s*o\.fparam(\d+) = ')
        body = [line for line in s.body() if not (flat.match(line) and int(flat.match(line).group(1)) not in self.flat)]
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
        out += ['    o.pos = p.pos + r.pos - q.pos;', '    return o;', '}']
        return '\n'.join(out) + '\n'

    def hlsl_ps(self):
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


def translate_to(out_dir, key, vs_code, ps_code, root_blob, check=True, rtv_formats=(), rtv_request=''):
    """Writes <key>.vs.hlsl, <key>.ps.hlsl and <key>.txt (last), or <key>.fail with the reason.

    With check, the pair is compiled to .vs.cso and .ps.cso first and a missing fxc is a failure.
    check=False, or X2W_FXC set to the empty string, publishes the manifest without bytecode."""
    name = os.path.join(out_dir, '%016x' % key)
    try:
        pipe = Pipeline(vs_code, ps_code, root_blob, rtv_formats=rtv_formats)
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
    return translate_to(out_dir, key, kinds['vs'], kinds['ps'], root, rtv_formats=formats, rtv_request=request)


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
