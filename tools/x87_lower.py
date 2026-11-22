"""Lower x87 instructions for ffx_version_query.lower() as calls into tools/x87_soft.h.

The generated function declares one `X87 fpu;` in the initial state that x87_soft.h documents. This
module covers the register stack and data movement: loads, stores, integer conversions, constants,
exchanges and sign changes. Instructions that read or write the status word, the control word or the
environment are not lowered, so ffx_version_query.lower() rejects them like any other unsupported operation.
"""
from pathlib import Path

from iced_x86 import MemorySize as S, Mnemonic as M, OpKind as K, Register as R

X87_HEADER = Path(__file__).with_name("x87_soft.h").read_text(encoding="ascii")

CONSTANTS = {M.FLD1: "x87_one", M.FLDZ: "x87_zero", M.FLDPI: "x87_pi", M.FLDL2E: "x87_l2e",
             M.FLDL2T: "x87_l2t", M.FLDLG2: "x87_lg2", M.FLDLN2: "x87_ln2"}
INTEGERS = {S.INT16: (16, "uint16_t", "int16_t"), S.INT32: (32, "uint32_t", "int32_t"), S.INT64: (64, "uint64_t", "int64_t")}
X87_OPS = frozenset({M.FLD, M.FST, M.FSTP, M.FILD, M.FIST, M.FISTP, M.FISTTP, M.FXCH, M.FCHS, M.FABS,
                     M.FNOP, M.WAIT, M.FFREE, M.FFREEP, M.FINCSTP, M.FDECSTP, *CONSTANTS})


def _st(register):
    return register - R.ST0


def _store(size, a):
    """Store ST(0) to memory at a in the format of size."""
    if size == S.FLOAT32:
        return f"store<uint32_t>({a}, x87_to_f32(fpu, x87_st(fpu, 0)));"
    if size == S.FLOAT64:
        return f"store<uint64_t>({a}, x87_to_f64(fpu, x87_st(fpu, 0)));"
    if size == S.FLOAT80:
        return f"{{ const X87Value v = x87_st(fpu, 0); store<uint64_t>({a}, v.sig); store<uint16_t>({a} + 8, v.se); }}"
    raise ValueError(f"unsupported x87 store size {size}")


def statement(i, address):
    """C++ for one x87 instruction. address() returns the flat address expression of the memory operand."""
    op = i.mnemonic
    memory = i.op_count >= 1 and i.op0_kind == K.MEMORY
    if memory:
        size = i.memory_size
        body = None
        if op == M.FLD:
            value = {S.FLOAT32: "x87_from_f32(load<uint32_t>(a))", S.FLOAT64: "x87_from_f64(load<uint64_t>(a))",
                     S.FLOAT80: "x87_from_m80(load<uint64_t>(a), load<uint16_t>(a + 8))"}.get(size)
            body = value and f"x87_push(fpu, {value});"
        elif op in (M.FST, M.FSTP) and (size != S.FLOAT80 or op == M.FSTP):
            body = _store(size, "a") + (" x87_pop(fpu);" if op == M.FSTP else "")
        elif op == M.FILD and size in INTEGERS:
            _, unsigned, signed = INTEGERS[size]
            body = f"x87_push(fpu, x87_from_int(static_cast<{signed}>(load<{unsigned}>(a))));"
        elif op in (M.FIST, M.FISTP, M.FISTTP) and size in INTEGERS and (op != M.FIST or size != S.INT64):
            bits, unsigned, _ = INTEGERS[size]
            rc = "3u" if op == M.FISTTP else "x87_rc(fpu)"
            body = (f"store<{unsigned}>(a, static_cast<{unsigned}>(x87_to_int(x87_st(fpu, 0), {rc}, {bits})));"
                    + (" x87_pop(fpu);" if op != M.FIST else ""))
        if body is None:
            raise ValueError(f"unsupported x87 operand at {i.ip:#x}: {i}")
        return f"{{ const word a = {address()}; {body} }}"
    if op in CONSTANTS:
        return f"x87_push(fpu, x87_constant(fpu, {CONSTANTS[op]}));"
    if op == M.FLD:
        return f"x87_push(fpu, x87_st(fpu, {_st(i.op0_register)}));"
    if op in (M.FST, M.FSTP):
        return f"x87_set(fpu, {_st(i.op0_register)}, x87_st(fpu, 0));" + (" x87_pop(fpu);" if op == M.FSTP else "")
    if op == M.FXCH:
        return f"x87_xchg(fpu, {_st(i.op1_register)});"
    if op in (M.FCHS, M.FABS):
        change = "v.se ^= 0x8000;" if op == M.FCHS else "v.se &= 0x7fff;"
        return f"if (x87_empty(fpu, 0)) x87_set(fpu, 0, x87_indefinite); else {{ X87Value v = x87_st(fpu, 0); {change} x87_set(fpu, 0, v); }}"
    if op in (M.FNOP, M.WAIT):  # with every exception masked, WAIT has nothing to report
        return ";"
    if op == M.FFREE:
        return f"x87_free(fpu, {_st(i.op0_register)});"
    if op == M.FFREEP:
        return f"x87_free(fpu, {_st(i.op0_register)}); x87_pop(fpu);"
    if op == M.FINCSTP:
        return "fpu.top = (fpu.top + 1) & 7u;"
    if op == M.FDECSTP:
        return "fpu.top = (fpu.top - 1) & 7u;"
    raise ValueError(f"unsupported x87 operation at {i.ip:#x}: {i}")
