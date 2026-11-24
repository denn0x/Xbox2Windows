; coverage: source cpu_aot_sse_mmx
; Source-built MASM fixture exercising 7 legacy SSE/SSE2/MMX forms from
; docs/coverage/x64_zen2.tsv (SSE: cvtpi2ps, cvtps2pi, cvttps2pi, pshufw,
; movntq, maskmovq; SSE2: maskmovdqu), along with transport/ABI glue (movq,
; 64-bit mov for rdi save/restore, movaps, movdqa, movdqu, emms).
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes, 16-byte aligned)
;   RDX = const void* input_b (32 bytes, 16-byte aligned)
;   R8  = void*       output  (32 bytes, 16-byte aligned)
.code

; --- cvtpi2ps: int32x2 -> float32x2 preserving xmm[127:64] ---

op_cvtpi2ps_reg PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movq mm0, QWORD PTR [rdx]
    cvtpi2ps xmm0, mm0
    movaps XMMWORD PTR [r8], xmm0
    emms
    ret
op_cvtpi2ps_reg ENDP

op_cvtpi2ps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cvtpi2ps xmm0, QWORD PTR [rdx]
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cvtpi2ps_mem ENDP

op_cvtpi2ps_mem_plus1 PROC FRAME
    .endprolog
    movdqa xmm2, XMMWORD PTR [rcx+16]
    cvtpi2ps xmm2, QWORD PTR [rdx+1]
    movdqa XMMWORD PTR [r8], xmm2
    ret
op_cvtpi2ps_mem_plus1 ENDP

; --- cvtps2pi: float32x2 -> int32x2 (round-to-nearest-even) ---

op_cvtps2pi_reg PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    cvttps2pi mm0, xmm0
ELSE
    cvtps2pi mm0, xmm0
ENDIF
    movq QWORD PTR [r8], mm0
    emms
    ret
op_cvtps2pi_reg ENDP

op_cvtps2pi_mem PROC FRAME
    .endprolog
    cvtps2pi mm1, QWORD PTR [rcx]
    movq QWORD PTR [r8], mm1
    emms
    ret
op_cvtps2pi_mem ENDP

op_cvtps2pi_mem_plus1 PROC FRAME
    .endprolog
    cvtps2pi mm2, QWORD PTR [rdx+1]
    movq QWORD PTR [r8+8], mm2
    emms
    ret
op_cvtps2pi_mem_plus1 ENDP

; --- cvttps2pi: float32x2 -> int32x2 (truncate toward zero) ---

op_cvttps2pi_reg PROC FRAME
    .endprolog
    movaps xmm1, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    cvtps2pi mm0, xmm1
ELSE
    cvttps2pi mm0, xmm1
ENDIF
    movq QWORD PTR [r8], mm0
    emms
    ret
op_cvttps2pi_reg ENDP

op_cvttps2pi_mem PROC FRAME
    .endprolog
    cvttps2pi mm3, QWORD PTR [rcx+8]
    movq QWORD PTR [r8], mm3
    emms
    ret
op_cvttps2pi_mem ENDP

; --- pshufw: 16-bit word permutation by imm8 ---

op_pshufw_reg_e4 PROC FRAME
    .endprolog
    movq mm0, QWORD PTR [rcx]
IFDEF ALTER_IMM
    pshufw mm1, mm0, 01Bh
ELSE
    pshufw mm1, mm0, 0E4h
ENDIF
    movq QWORD PTR [r8], mm1
    emms
    ret
op_pshufw_reg_e4 ENDP

op_pshufw_reg_1b PROC FRAME
    .endprolog
    movq mm2, QWORD PTR [rcx]
    pshufw mm3, mm2, 01Bh
    movq QWORD PTR [r8], mm3
    emms
    ret
op_pshufw_reg_1b ENDP

op_pshufw_reg_00 PROC FRAME
    .endprolog
    movq mm4, QWORD PTR [rcx]
    pshufw mm4, mm4, 000h
    movq QWORD PTR [r8], mm4
    emms
    ret
op_pshufw_reg_00 ENDP

op_pshufw_reg_ff PROC FRAME
    .endprolog
    movq mm5, QWORD PTR [rcx]
    pshufw mm6, mm5, 0FFh
    movq QWORD PTR [r8], mm6
    emms
    ret
op_pshufw_reg_ff ENDP

op_pshufw_mem_4e PROC FRAME
    .endprolog
IFDEF ALTER_IMM
    pshufw mm7, QWORD PTR [rdx+1], 0B1h
ELSE
    pshufw mm7, QWORD PTR [rdx+1], 04Eh
ENDIF
    movq QWORD PTR [r8], mm7
    emms
    ret
op_pshufw_mem_4e ENDP

op_pshufw_mem_39 PROC FRAME
    .endprolog
    pshufw mm0, QWORD PTR [rdx+8], 039h
    movq QWORD PTR [r8], mm0
    emms
    ret
op_pshufw_mem_39 ENDP

; --- movntq, GPR bridge, discarded read and chained pipeline ---

op_movntq PROC FRAME
    .endprolog
    movq mm0, QWORD PTR [rcx]
    movntq QWORD PTR [r8], mm0
    emms
    ret
op_movntq ENDP

op_movntq_disp PROC FRAME
    .endprolog
    movq mm1, QWORD PTR [rcx+1]
    movntq QWORD PTR [r8+9], mm1
    emms
    ret
op_movntq_disp ENDP

op_movq_gpr_bridge PROC FRAME
    .endprolog
    movq mm0, QWORD PTR [rcx]
    movq rax, mm0
    movq mm7, rax
    movntq QWORD PTR [r8], mm7
    emms
    ret
op_movq_gpr_bridge ENDP

; The CVTPS2PI source must be read even though mm5 is dead after this instruction.
op_discarded_read PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps XMMWORD PTR [r8], xmm0
    cvtps2pi mm5, QWORD PTR [rdx]
    emms
    movaps XMMWORD PTR [r8+16], xmm0
    ret
op_discarded_read ENDP

op_pipeline PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movdqu xmm1, XMMWORD PTR [rdx+1]
    cvtps2pi mm0, xmm0
    cvttps2pi mm1, xmm1
    pshufw mm2, mm0, 04Eh
    cvtpi2ps xmm0, mm2
    movntq QWORD PTR [r8], mm1
    movaps XMMWORD PTR [r8+16], xmm0
    emms
    movq mm3, QWORD PTR [rdx+16]
    pshufw mm4, mm3, 0B1h
    movntq QWORD PTR [r8+8], mm4
    emms
    ret
op_pipeline ENDP

; --- maskmovq / maskmovdqu: conditional byte store to [rdi] with rdi save/restore ---

op_maskmovq PROC FRAME
    .endprolog
    movq mm0, QWORD PTR [rcx]
    movq mm1, QWORD PTR [rdx]
    mov r10, rdi
    mov rdi, r8
IFDEF ALTER_IMM
    maskmovq mm1, mm0
ELSE
    maskmovq mm0, mm1
ENDIF
    mov rdi, r10
    emms
    ret
op_maskmovq ENDP

op_maskmovdqu PROC FRAME
    .endprolog
    movdqu xmm0, XMMWORD PTR [rcx]
    movdqu xmm1, XMMWORD PTR [rdx]
    mov r11, rdi
    mov rdi, r8
IFDEF ALTER_IMM
    maskmovdqu xmm1, xmm0
ELSE
    maskmovdqu xmm0, xmm1
ENDIF
    mov rdi, r11
    ret
op_maskmovdqu ENDP

op_maskmov_pipeline PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movdqu xmm1, XMMWORD PTR [rdx+1]
    movdqu XMMWORD PTR [r8], xmm0
    mov r9, rdi
    mov rdi, r8
    maskmovdqu xmm1, xmm0
    mov rdi, r9
    cvtps2pi mm0, xmm0
    cvttps2pi mm1, xmm1
    movq QWORD PTR [r8+16], mm0
    mov rax, rdi
    mov rdi, r8
    maskmovq mm1, mm0
    mov rdi, rax
    emms
    ret
op_maskmov_pipeline ENDP

END
