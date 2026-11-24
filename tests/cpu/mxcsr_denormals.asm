; coverage: source cpu_aot_mxcsr_denormals
; Source-owned differential fixture for FTZ and DAZ in the host-MXCSR profile's denormals opt-in. Each export takes
; (a, b, c, state) in RCX, RDX, R8 and R9; b is the requested MXCSR word. State layout (bytes):
;   00 caller word (STMXCSR at entry, LDMXCSR at exit)   04 guard
;   08 requested word                                     0C guard
;   10, 20 the FTZ case: normal operands with tiny signed sums     30, 40 the DAZ case: subnormal signed operands
;   50 the FTZ sums                                       60 the DAZ sums
;   70 STMXCSR read-back, 74 guard                        78 the marker den_after writes, 80-FF guards
; All operands are written by the test at run time. den_add restores the caller's word itself; den_after and
; den_fault do not, and the test calls them translated only.
.code

den_add PROC FRAME
    .endprolog
    stmxcsr dword ptr [r9]
    mov dword ptr [r9+8], edx
    ldmxcsr dword ptr [r9+8]
    movups xmm0, xmmword ptr [r9+10h]
    movups xmm1, xmmword ptr [r9+20h]
    addps xmm0, xmm1
    movups xmmword ptr [r9+50h], xmm0
    movups xmm0, xmmword ptr [r9+30h]
    movups xmm1, xmmword ptr [r9+40h]
    addps xmm0, xmm1
    movups xmmword ptr [r9+60h], xmm0
    stmxcsr dword ptr [r9+70h]
    ldmxcsr dword ptr [r9]
    mov eax, 7
    ret
den_add ENDP

; Guest work after the load: the marker store must not run when the word is refused.
den_after PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    ldmxcsr dword ptr [r9+8]
    mov qword ptr [r9+78h], 1
    mov eax, 7
    ret
den_after ENDP

; A guest load through RCX after a valid word was loaded; the test points RCX at an inaccessible page.
den_fault PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    ldmxcsr dword ptr [r9+8]
    mov eax, dword ptr [rcx]
    ret
den_fault ENDP

END
