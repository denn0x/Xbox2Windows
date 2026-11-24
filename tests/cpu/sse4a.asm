; coverage: record cpu_aot_sse4a
; Source-built MASM fixture exercising all 6 SSE4a forms (extrq imm/reg,
; insertq imm/reg, movntsd, movntss) from docs/coverage/x64_zen2.tsv group
; SSE3-4, along with Win64 MASM baseline ref_* exports so lifted verification
; always runs on both Intel and AMD hosts.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes, 16-byte aligned)
;   RDX = const void* input_b (32 bytes, 16-byte aligned)
;   R8  = void*       output  (32 bytes, 16-byte aligned)

; Helper macro: rax = extrq(rax, r9[13:0]) with width=0 -> 64. Clobbers rcx, r10.
REF_EXTRQ MACRO
    LOCAL done_mask
    mov ecx, r9d
    and ecx, 3Fh
    mov r10, -1
    jz done_mask
    mov r10, 1
    shl r10, cl
    dec r10
done_mask:
    mov ecx, r9d
    shr ecx, 8
    and ecx, 3Fh
    shr rax, cl
    and rax, r10
ENDM

; Helper macro: rax = insertq(rax, r11, r9[13:0]) with width=0 -> 64. Clobbers rcx, r10, r11.
REF_INSERTQ MACRO
    LOCAL done_mask
    mov ecx, r9d
    and ecx, 3Fh
    mov r10, -1
    jz done_mask
    mov r10, 1
    shl r10, cl
    dec r10
done_mask:
    and r11, r10
    mov ecx, r9d
    shr ecx, 8
    and ecx, 3Fh
    shl r10, cl
    shl r11, cl
    not r10
    and rax, r10
    or rax, r11
ENDM

.code

; --- extrq immediate form (66 0F 78 /0 ib ib) ---

op_extrq_imm_basic PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    extrq xmm0, 12, 24
ELSE
    extrq xmm0, 12, 20
ENDIF
    movntsd QWORD PTR [r8], xmm0
    ret
op_extrq_imm_basic ENDP

op_extrq_imm_zero_len PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    extrq xmm0, 63, 0
ELSE
    extrq xmm0, 0, 0
ENDIF
    movntsd QWORD PTR [r8], xmm0
    ret
op_extrq_imm_zero_len ENDP

op_extrq_imm_high_bit PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    extrq xmm0, 1, 63
    movntsd QWORD PTR [r8], xmm0
    ret
op_extrq_imm_high_bit ENDP

op_extrq_imm_mid PROC FRAME
    .endprolog
    movaps xmm1, XMMWORD PTR [rcx]
    extrq xmm1, 16, 48
    movntsd QWORD PTR [r8], xmm1
    ret
op_extrq_imm_mid ENDP

op_extrq_imm_bits76 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    extrq xmm0, 48h, 05h
    movntsd QWORD PTR [r8], xmm0
    ret
op_extrq_imm_bits76 ENDP

; --- extrq register form (66 0F 79 /r) ---

op_extrq_reg_basic PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    extrq xmm0, xmm1
    movntsd QWORD PTR [r8], xmm0
    ret
op_extrq_reg_basic ENDP

op_extrq_reg_self PROC FRAME
    .endprolog
    movaps xmm2, XMMWORD PTR [rcx]
    extrq xmm2, xmm2
    movntsd QWORD PTR [r8], xmm2
    ret
op_extrq_reg_self ENDP

op_extrq_reg_xmm4_xmm5 PROC FRAME
    .endprolog
    movdqa xmm4, XMMWORD PTR [rcx+16]
    movdqu xmm5, XMMWORD PTR [rdx+1]
    extrq xmm4, xmm5
    movntsd QWORD PTR [r8], xmm4
    ret
op_extrq_reg_xmm4_xmm5 ENDP

; --- insertq immediate form (F2 0F 78 /r ib ib) ---

op_insertq_imm_basic PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    insertq xmm0, xmm1, 16, 20
ELSE
    insertq xmm0, xmm1, 16, 16
ENDIF
    movntsd QWORD PTR [r8], xmm0
    ret
op_insertq_imm_basic ENDP

op_insertq_imm_zero_len PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    insertq xmm0, xmm1, 63, 0
ELSE
    insertq xmm0, xmm1, 0, 0
ENDIF
    movntsd QWORD PTR [r8], xmm0
    ret
op_insertq_imm_zero_len ENDP

op_insertq_imm_high_bit PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    insertq xmm0, xmm1, 1, 63
    movntsd QWORD PTR [r8], xmm0
    ret
op_insertq_imm_high_bit ENDP

op_insertq_imm_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    insertq xmm0, xmm0, 12, 36
    movntsd QWORD PTR [r8], xmm0
    ret
op_insertq_imm_self ENDP

; --- insertq register form (F2 0F 79 /r) ---

op_insertq_reg_basic PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    insertq xmm0, xmm1
    movntsd QWORD PTR [r8], xmm0
    ret
op_insertq_reg_basic ENDP

op_insertq_reg_self PROC FRAME
    .endprolog
    movaps xmm3, XMMWORD PTR [rcx]
    insertq xmm3, xmm3
    movntsd QWORD PTR [r8], xmm3
    ret
op_insertq_reg_self ENDP

op_insertq_reg_xmm4_xmm5 PROC FRAME
    .endprolog
    movdqa xmm4, XMMWORD PTR [rcx+16]
    movdqu xmm5, XMMWORD PTR [rdx+1]
    insertq xmm4, xmm5
    movntsd QWORD PTR [r8], xmm4
    ret
op_insertq_reg_xmm4_xmm5 ENDP

; --- movntsd (F2 0F 2B /r) and movntss (F3 0F 2B /r) ---

op_movntsd_aligned PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movntsd QWORD PTR [r8], xmm0
    ret
op_movntsd_aligned ENDP

op_movntsd_disp8 PROC FRAME
    .endprolog
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    movntsd QWORD PTR [r8+16], xmm1
ELSE
    movntsd QWORD PTR [r8+8], xmm1
ENDIF
    ret
op_movntsd_disp8 ENDP

op_movntsd_unaligned PROC FRAME
    .endprolog
    movdqu xmm0, XMMWORD PTR [rcx+3]
    movntsd QWORD PTR [r8+1], xmm0
    ret
op_movntsd_unaligned ENDP

op_movntss_aligned PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movntss DWORD PTR [r8], xmm0
    ret
op_movntss_aligned ENDP

op_movntss_disp12 PROC FRAME
    .endprolog
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    movntss DWORD PTR [r8+16], xmm1
ELSE
    movntss DWORD PTR [r8+12], xmm1
ENDIF
    ret
op_movntss_disp12 ENDP

op_movntss_unaligned PROC FRAME
    .endprolog
    movdqu xmm0, XMMWORD PTR [rcx+5]
    movntss DWORD PTR [r8+3], xmm0
    ret
op_movntss_unaligned ENDP

; --- Chained pipeline and discarded-read fault ordering ---

op_pipeline PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    movdqa xmm2, XMMWORD PTR [rcx+16]
    movdqu xmm3, XMMWORD PTR [rdx+16]
    extrq xmm0, 16, 8
    insertq xmm2, xmm0, 16, 24
    insertq xmm2, xmm1
    extrq xmm3, xmm1
    movntsd QWORD PTR [r8], xmm2
    movntsd QWORD PTR [r8+8], xmm3
    movntss DWORD PTR [r8+16], xmm0
    movntss DWORD PTR [r8+21], xmm2
    ret
op_pipeline ENDP

; Universal native+lifted dead-read ordering control (runs on Intel & AMD).
op_discarded_read PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps XMMWORD PTR [r8], xmm0
    movaps xmm5, XMMWORD PTR [rdx]
    movaps XMMWORD PTR [r8+16], xmm0
    ret
op_discarded_read ENDP

; SSE4a dead-read ordering control (lifted always; native gated on CPUID SSE4a).
op_sse4a_discarded_read PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movntsd QWORD PTR [r8], xmm0
    movdqu xmm5, XMMWORD PTR [rdx]
    extrq xmm5, 8, 8
    movntss DWORD PTR [r8+8], xmm0
    ret
op_sse4a_discarded_read ENDP

; --- Win64 MASM baseline reference exports (never translated; run on Intel & AMD) ---

ref_extrq_imm_basic PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9d, (20 SHL 8) OR 12
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_imm_basic ENDP

ref_extrq_imm_zero_len PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9d, 0
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_imm_zero_len ENDP

ref_extrq_imm_high_bit PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9d, (63 SHL 8) OR 1
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_imm_high_bit ENDP

ref_extrq_imm_mid PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9d, (48 SHL 8) OR 16
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_imm_mid ENDP

ref_extrq_imm_bits76 PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9d, (05h SHL 8) OR 48h
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_imm_bits76 ENDP

ref_extrq_reg_basic PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_reg_basic ENDP

ref_extrq_reg_self PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, rax
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_reg_self ENDP

ref_extrq_reg_xmm4_xmm5 PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx+16]
    mov r9, QWORD PTR [rdx+1]
    REF_EXTRQ
    mov QWORD PTR [r8], rax
    ret
ref_extrq_reg_xmm4_xmm5 ENDP

ref_insertq_imm_basic PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, QWORD PTR [rdx]
    mov r9d, (16 SHL 8) OR 16
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_imm_basic ENDP

ref_insertq_imm_zero_len PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, QWORD PTR [rdx]
    mov r9d, 0
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_imm_zero_len ENDP

ref_insertq_imm_high_bit PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, QWORD PTR [rdx]
    mov r9d, (63 SHL 8) OR 1
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_imm_high_bit ENDP

ref_insertq_imm_self PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, rax
    mov r9d, (36 SHL 8) OR 12
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_imm_self ENDP

ref_insertq_reg_basic PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, QWORD PTR [rdx]
    mov r9, QWORD PTR [rdx+8]
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_reg_basic ENDP

ref_insertq_reg_self PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r11, rax
    mov r9, QWORD PTR [rcx+8]
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_reg_self ENDP

ref_insertq_reg_xmm4_xmm5 PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx+16]
    mov r11, QWORD PTR [rdx+1]
    mov r9, QWORD PTR [rdx+9]
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    ret
ref_insertq_reg_xmm4_xmm5 ENDP

ref_movntsd_aligned PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov QWORD PTR [r8], rax
    ret
ref_movntsd_aligned ENDP

ref_movntsd_disp8 PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rdx]
    mov QWORD PTR [r8+8], rax
    ret
ref_movntsd_disp8 ENDP

ref_movntsd_unaligned PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx+3]
    mov QWORD PTR [r8+1], rax
    ret
ref_movntsd_unaligned ENDP

ref_movntss_aligned PROC FRAME
    .endprolog
    mov eax, DWORD PTR [rcx]
    mov DWORD PTR [r8], eax
    ret
ref_movntss_aligned ENDP

ref_movntss_disp12 PROC FRAME
    .endprolog
    mov eax, DWORD PTR [rdx]
    mov DWORD PTR [r8+12], eax
    ret
ref_movntss_disp12 ENDP

ref_movntss_unaligned PROC FRAME
    .endprolog
    mov eax, DWORD PTR [rcx+5]
    mov DWORD PTR [r8+3], eax
    ret
ref_movntss_unaligned ENDP

ref_pipeline PROC FRAME
    .endprolog
    movq xmm0, QWORD PTR [rcx+16] ; save a_hi before REF_EXTRQ clobbers rcx
    ; Step 1: xmm0 = extrq(a_lo, 16, 8) -> low 32 bits go to [r8+16]
    mov rax, QWORD PTR [rcx]
    mov r9d, (8 SHL 8) OR 16
    REF_EXTRQ
    mov DWORD PTR [r8+16], eax
    mov r11, rax
    ; Step 2: xmm2 = insertq(a_hi, xmm0, 16, 24)
    movq rax, xmm0
    mov r9d, (24 SHL 8) OR 16
    REF_INSERTQ
    ; Step 3: xmm2 = insertq(xmm2, xmm1)
    mov r11, QWORD PTR [rdx]
    mov r9, QWORD PTR [rdx+8]
    REF_INSERTQ
    mov QWORD PTR [r8], rax
    mov r11, rax               ; keep xmm2[31:0] for [r8+21]
    ; Step 4: xmm3 = extrq(b_hi, xmm1)
    mov rax, QWORD PTR [rdx+16]
    mov r9, QWORD PTR [rdx]
    REF_EXTRQ
    mov QWORD PTR [r8+8], rax
    ; Step 5: movntss [r8+21], xmm2
    mov DWORD PTR [r8+21], r11d
    ret
ref_pipeline ENDP

END
