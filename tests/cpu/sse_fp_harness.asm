; Native-only harness for tests/cpu/sse_fp_check.py; it is never translated.
; op_call_with_mxcsr runs fn(a, b, out) with MXCSR set to the fifth argument and restores the caller's
; MXCSR before returning, so the host control word never leaks into the checker.
; RCX = fn, RDX = a, R8 = b, R9 = out, [stack] = MXCSR value.
.code

op_call_with_mxcsr PROC FRAME
    sub rsp, 56
    .allocstack 56
    .endprolog
    mov r10, rcx
    stmxcsr DWORD PTR [rsp+36]
    mov eax, DWORD PTR [rsp+96]
    mov DWORD PTR [rsp+40], eax
    ldmxcsr DWORD PTR [rsp+40]
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call r10
    ldmxcsr DWORD PTR [rsp+36]
    add rsp, 56
    ret
op_call_with_mxcsr ENDP

END
