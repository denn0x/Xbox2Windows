; Native-only harness for tests/cpu/mxcsr.py and tests/cpu/mxcsr_denormals.py; it is never translated. get_mxcsr
; returns the calling thread's MXCSR and set_mxcsr(word) loads it, both through the caller's home space.
; mxcsr_mask_of(area) runs FXSAVE into the 16-byte-aligned 512-byte area and returns its MXCSR_MASK field.
; has_avx_os reports whether the host can execute VEX instructions natively (tests/cpu/vmxcsr.py).
.code

get_mxcsr PROC FRAME
    .endprolog
    stmxcsr dword ptr [rsp+8]
    mov eax, dword ptr [rsp+8]
    ret
get_mxcsr ENDP

set_mxcsr PROC FRAME
    .endprolog
    mov dword ptr [rsp+8], ecx
    ldmxcsr dword ptr [rsp+8]
    ret
set_mxcsr ENDP

mxcsr_mask_of PROC FRAME
    .endprolog
    fxsave64 [rcx]
    mov eax, dword ptr [rcx+28]
    ret
mxcsr_mask_of ENDP

; has_avx_os returns 1 when CPUID.01H:ECX reports AVX (bit 28) and OSXSAVE (bit 27) and XCR0 enables SSE and AVX
; state (bits 1 and 2), the precondition for executing a VEX instruction natively.
has_avx_os PROC FRAME
    push rbx
    .pushreg rbx
    .endprolog
    mov eax, 1
    xor ecx, ecx
    cpuid
    mov eax, ecx
    and eax, 18000000h
    cmp eax, 18000000h
    jne short no_avx
    xor ecx, ecx
    xgetbv
    and eax, 6
    cmp eax, 6
    jne short no_avx
    mov eax, 1
    pop rbx
    ret
no_avx:
    xor eax, eax
    pop rbx
    ret
has_avx_os ENDP

END
