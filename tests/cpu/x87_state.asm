; Native-only measurement harness for tests/cpu/x87_data.py. fpu_state returns the x87 control word in
; bits 0 to 15, the status word in bits 16 to 31 and the tag word in bits 32 to 47, as a native export
; called through ctypes sees them. FNSTENV masks all exceptions, so FLDCW restores the control word.
.code

fpu_state PROC FRAME
    sub rsp, 40
    .allocstack 40
    .endprolog
    fnstenv [rsp]
    movzx eax, word ptr [rsp]
    movzx edx, word ptr [rsp+4]
    shl edx, 16
    or eax, edx
    movzx edx, word ptr [rsp+8]
    shl rdx, 32
    or rax, rdx
    fldcw word ptr [rsp]
    add rsp, 40
    ret
fpu_state ENDP

END
