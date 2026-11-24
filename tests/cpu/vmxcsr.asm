; coverage: source cpu_aot_vmxcsr
; VEX copies of mxcsr.asm for VLDMXCSR and VSTMXCSR in the opt-in host-MXCSR profile, with the same export names,
; state layout and ABI (see mxcsr.asm), plus two exports that mix the legacy and VEX forms: mix_ld_vst loads with
; LDMXCSR and reads back with VSTMXCSR, mix_vld_st the reverse. Neither restores the caller's word; the translated
; entry's boundary does, and the native comparison restores it from the harness.
.code

mxcsr_round PROC FRAME
    .endprolog
    vstmxcsr dword ptr [r9]
    mov dword ptr [r9+8], edx
    vldmxcsr dword ptr [r9+8]
    movups xmm0, xmmword ptr [r9+10h]
    movups xmm1, xmmword ptr [r9+20h]
    addps xmm0, xmm1
    movups xmmword ptr [r9+50h], xmm0
    movups xmm2, xmmword ptr [r9+30h]
    cvtss2si eax, xmm2
    mov dword ptr [r9+60h], eax
    movups xmm2, xmmword ptr [r9+40h]
    cvtss2si eax, xmm2
    mov dword ptr [r9+68h], eax
    vstmxcsr dword ptr [r9+70h]
    vldmxcsr dword ptr [r9]
    mov eax, 7
    ret
mxcsr_round ENDP

; Loads the requested word and returns without restoring the caller's.
mxcsr_leak PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    vldmxcsr dword ptr [r9+8]
    mov eax, 7
    ret
mxcsr_leak ENDP

; Guest work after the load: the marker store must not run when the word is refused.
mxcsr_after PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    vldmxcsr dword ptr [r9+8]
    mov qword ptr [r9+78h], 1
    mov eax, 7
    ret
mxcsr_after ENDP

; LDMXCSR of an operand the test places on an inaccessible page: the guest read faults before any load.
mxcsr_badload PROC FRAME
    .endprolog
    vldmxcsr dword ptr [rcx]
    mov eax, 7
    ret
mxcsr_badload ENDP

; A guest load through RCX after a valid word was loaded; the test points RCX at an inaccessible page.
mxcsr_fault PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    vldmxcsr dword ptr [r9+8]
    mov eax, dword ptr [rcx]
    ret
mxcsr_fault ENDP


mix_ld_vst PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    ldmxcsr dword ptr [r9+8]
    vstmxcsr dword ptr [r9+70h]
    mov eax, 7
    ret
mix_ld_vst ENDP

mix_vld_st PROC FRAME
    .endprolog
    mov dword ptr [r9+8], edx
    vldmxcsr dword ptr [r9+8]
    stmxcsr dword ptr [r9+70h]
    mov eax, 7
    ret
mix_vld_st ENDP

END
