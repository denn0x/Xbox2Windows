; coverage: source cpu_aot_switch_bit_scans
; Source-owned differential fixture and negative controls for BSF and BSR in tools/cpu_switch_aot.py.
; Every differential export takes RCX = x (the destination's old value), RDX = y (the source) and
; R8 = address of three writable QWORDs. SETE and SETNE bytes go to [r8] and [r8+1] after the scan;
; the other bytes of the first two QWORDs are left as the harness set them. A zero source leaves the
; destination unchanged, so x is observable in the result exactly then.
; A build that defines SWAP_SCANS exchanges BSF and BSR in bsf32_reg, bsf64_mem and bsr16_reg.
.code
bsf16_reg PROC FRAME
    .endprolog
    mov r9, rcx
    bsf r9w, dx
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsf16_reg ENDP

bsf16_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
    bsf ax, WORD PTR [r8+16]
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsf16_mem ENDP

bsf32_reg PROC FRAME
    .endprolog
    mov r9, rcx
IFDEF SWAP_SCANS
    bsr r9d, edx
ELSE
    bsf r9d, edx
ENDIF
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsf32_reg ENDP

bsf32_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
    bsf eax, DWORD PTR [r8+16]
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsf32_mem ENDP

bsf64_reg PROC FRAME
    .endprolog
    mov r9, rcx
    bsf r9, rdx
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsf64_reg ENDP

bsf64_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
IFDEF SWAP_SCANS
    bsr rax, QWORD PTR [r8+16]
ELSE
    bsf rax, QWORD PTR [r8+16]
ENDIF
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsf64_mem ENDP

bsr16_reg PROC FRAME
    .endprolog
    mov r9, rcx
IFDEF SWAP_SCANS
    bsf r9w, dx
ELSE
    bsr r9w, dx
ENDIF
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsr16_reg ENDP

bsr16_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
    bsr ax, WORD PTR [r8+16]
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsr16_mem ENDP

bsr32_reg PROC FRAME
    .endprolog
    mov r9, rcx
    bsr r9d, edx
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsr32_reg ENDP

bsr32_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
    bsr eax, DWORD PTR [r8+16]
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsr32_mem ENDP

bsr64_reg PROC FRAME
    .endprolog
    mov r9, rcx
    bsr r9, rdx
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], r9
    mov rax, r9
    ret
bsr64_reg ENDP

bsr64_mem PROC FRAME
    .endprolog
    mov QWORD PTR [r8+16], rdx
    mov rax, rcx
    bsr rax, QWORD PTR [r8+16]
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    ret
bsr64_mem ENDP

bsr32_self PROC FRAME
    .endprolog
    mov rax, rdx
    bsr eax, eax
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], rax
    ret
bsr32_self ENDP

bsf16_self PROC FRAME
    .endprolog
    mov rax, rdx
    bsf ax, ax
    sete BYTE PTR [r8]
    setne BYTE PTR [r8+1]
    mov QWORD PTR [r8+16], rax
    ret
bsf16_self ENDP

jcc_bsf_e PROC FRAME
    .endprolog
    mov rax, rcx
    bsf rax, rdx
    je jcc_bsf_e_taken
    ret
jcc_bsf_e_taken:
    not rax
    ret
jcc_bsf_e ENDP

jcc_bsr_ne PROC FRAME
    .endprolog
    mov rax, rcx
    bsr eax, edx
    jne jcc_bsr_ne_taken
    ret
jcc_bsr_ne_taken:
    not rax
    ret
jcc_bsr_ne ENDP

cmov_scans PROC FRAME
    .endprolog
    mov rax, rcx
    mov r9, rdx
    bsr r9, rcx
    mov r10, rdx
    cmove rax, r10
    mov QWORD PTR [r8], rax
    mov r10, rdx
    bsf r10d, ecx
    cmovne r10d, r9d
    mov QWORD PTR [r8+8], r10
    mov QWORD PTR [r8+16], r9
    mov rax, rdx
    ret
cmov_scans ENDP

; --- Negative controls (untranslated by CMake; tested directly for rejection) ---

neg_bsf_setb PROC FRAME
    .endprolog
    mov rax, rdx
    bsf rax, rcx
    setb al
    ret
neg_bsf_setb ENDP

neg_bsr_seta PROC FRAME
    .endprolog
    mov rax, rdx
    bsr rax, rcx
    seta al
    ret
neg_bsr_seta ENDP

neg_bsf_cmovae PROC FRAME
    .endprolog
    mov rax, rdx
    mov r9, rcx
    bsf r9, rdx
    cmovae rax, r9
    ret
neg_bsf_cmovae ENDP

neg_bsr_sets PROC FRAME
    .endprolog
    mov eax, edx
    bsr rax, rcx
    sets al
    ret
neg_bsr_sets ENDP

neg_bsf_setl PROC FRAME
    .endprolog
    mov eax, edx
    bsf rax, rcx
    setl al
    ret
neg_bsf_setl ENDP

neg_bsr_seto PROC FRAME
    .endprolog
    mov eax, edx
    bsr eax, ecx
    seto al
    ret
neg_bsr_seto ENDP

neg_bsf_setp PROC FRAME
    .endprolog
    mov eax, edx
    bsf ax, cx
    setp al
    ret
neg_bsf_setp ENDP

neg_bsr_cmovg PROC FRAME
    .endprolog
    mov rax, rdx
    mov r9, rcx
    bsr r9, rdx
    cmovg rax, r9
    ret
neg_bsr_cmovg ENDP

neg_bsf_undef_dest PROC FRAME
    .endprolog
    bsf eax, ecx
    ret
neg_bsf_undef_dest ENDP

neg_bsf_partial_dest PROC FRAME
    .endprolog
    mov ax, cx
    bsf eax, edx
    ret
neg_bsf_partial_dest ENDP

neg_bsr_undef_src PROC FRAME
    .endprolog
    mov rax, rcx
    bsr rax, r10
    ret
neg_bsr_undef_src ENDP

END
