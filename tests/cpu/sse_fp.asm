; coverage: source cpu_aot_sse_fp
; Source-built MASM fixture exercising 12 legacy SSE forms from docs/coverage/x64_zen2.tsv
; group SSE: addps, subps, mulps, divps, minps, maxps, sqrtps, cmpps, cmpss, movlps,
; movhps and movntps, along with aligned and unaligned transport moves (movaps,
; movdqa, movdqu).
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes, 16-byte aligned)
;   RDX = const void* input_b (32 bytes, 16-byte aligned; second vector at +16)
;   R8  = void*       output  (32 bytes, 16-byte aligned)
.code

; --- cmpps: all eight predicates on register and aligned m128 sources ---

op_cmpps_0 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpps xmm0, xmm1, 0
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_0 ENDP

op_cmpps_1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpps xmm0, xmm1, 5
ELSE
    cmpps xmm0, xmm1, 1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_1 ENDP

op_cmpps_2 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpps xmm0, xmm1, 2
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_2 ENDP

op_cmpps_3 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpps xmm0, xmm1, 7
ELSE
    cmpps xmm0, xmm1, 3
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_3 ENDP

op_cmpps_4 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpps xmm0, xmm1, 4
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_4 ENDP

op_cmpps_5 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpps xmm0, xmm1, 5
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_5 ENDP

op_cmpps_6 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpps xmm0, xmm1, 2
ELSE
    cmpps xmm0, xmm1, 6
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_6 ENDP

op_cmpps_7 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpps xmm0, xmm1, 7
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_7 ENDP

op_cmpps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpps xmm0, XMMWORD PTR [rdx], 5
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_mem ENDP

op_cmpps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpps xmm0, XMMWORD PTR [rdx+16], 2
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_mem_disp ENDP

op_cmpps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpps xmm0, xmm0, 0
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpps_self ENDP

; --- cmpss: all eight predicates, register/m32/unaligned +1 sources, bits 127:32 preserved ---

op_cmpss_0 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpss xmm0, xmm1, 0
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_0 ENDP

op_cmpss_1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpss xmm0, xmm1, 5
ELSE
    cmpss xmm0, xmm1, 1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_1 ENDP

op_cmpss_2 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpss xmm0, xmm1, 2
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_2 ENDP

op_cmpss_3 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpss xmm0, xmm1, 7
ELSE
    cmpss xmm0, xmm1, 3
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_3 ENDP

op_cmpss_4 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpss xmm0, xmm1, 4
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_4 ENDP

op_cmpss_5 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpss xmm0, xmm1, 5
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_5 ENDP

op_cmpss_6 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    cmpss xmm0, xmm1, 2
ELSE
    cmpss xmm0, xmm1, 6
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_6 ENDP

op_cmpss_7 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    cmpss xmm0, xmm1, 7
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_7 ENDP

op_cmpss_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpss xmm0, DWORD PTR [rdx], 5
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_mem ENDP

op_cmpss_mem_plus1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpss xmm0, DWORD PTR [rdx+1], 2
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_mem_plus1 ENDP

op_cmpss_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    cmpss xmm0, xmm0, 0
    movaps XMMWORD PTR [r8], xmm0
    ret
op_cmpss_self ENDP

; --- movlps / movhps: m64 load-merge (preserving other 64 bits) and m64 store ---

op_movlps_load PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movlps xmm0, QWORD PTR [rdx]
    movaps XMMWORD PTR [r8], xmm0
    ret
op_movlps_load ENDP

op_movlps_store PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movlps QWORD PTR [r8+8], xmm0
    ret
op_movlps_store ENDP

op_movhps_load PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movhps xmm0, QWORD PTR [rdx]
    movaps XMMWORD PTR [r8], xmm0
    ret
op_movhps_load ENDP

op_movhps_store PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movhps QWORD PTR [r8+8], xmm0
    ret
op_movhps_store ENDP

op_movlps_load_plus1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movlps xmm0, QWORD PTR [rdx+1]
    movaps XMMWORD PTR [r8], xmm0
    ret
op_movlps_load_plus1 ENDP

op_movhps_load_plus1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movhps xmm0, QWORD PTR [rdx+1]
    movaps XMMWORD PTR [r8], xmm0
    ret
op_movhps_load_plus1 ENDP

op_movlps_store_plus1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movlps QWORD PTR [r8+9], xmm0
    ret
op_movlps_store_plus1 ENDP

op_movhps_store_plus1 PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movhps QWORD PTR [r8+9], xmm0
    ret
op_movhps_store_plus1 ENDP

; --- movntps: 16-byte aligned non-temporal store ---

op_movntps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movntps XMMWORD PTR [r8], xmm0
    ret
op_movntps ENDP

op_movntps_disp16 PROC FRAME
    .endprolog
    movdqu xmm0, XMMWORD PTR [rcx+1]
    movntps XMMWORD PTR [r8+16], xmm0
    ret
op_movntps_disp16 ENDP

; --- Transport helpers and chained pipeline ---

op_movaps_reg PROC FRAME
    .endprolog
    movdqa xmm1, XMMWORD PTR [rcx]
    movaps xmm0, xmm1
    movaps XMMWORD PTR [r8], xmm0
    ret
op_movaps_reg ENDP

; The MOVAPS source must be read even though xmm5 is dead after this instruction.
op_discarded_read PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps XMMWORD PTR [r8], xmm0
    movaps xmm5, XMMWORD PTR [rdx]
    movaps XMMWORD PTR [r8+16], xmm0
    ret
op_discarded_read ENDP

op_pipeline PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movdqu xmm1, XMMWORD PTR [rdx]
    movdqa xmm2, XMMWORD PTR [rdx+16]
    movlps xmm0, QWORD PTR [rdx+8]
    movhps xmm2, QWORD PTR [rcx+24]
    cmpps xmm0, xmm1, 2
    cmpss xmm2, DWORD PTR [rdx+4], 4
    movntps XMMWORD PTR [r8], xmm0
    movhps QWORD PTR [r8+16], xmm2
    movlps QWORD PTR [r8+24], xmm2
    ret
op_pipeline ENDP

; --- Packed float arithmetic: reg, aligned m128, m128 at +16 and self sources ---
; ALTER_IMM swaps each register-form operation (add/sub, mul/div, min/max; sqrt becomes
; add), moves each memory source by 16 bytes and replaces each self source by xmm1, so
; every lowering has a build that must differ on known words.

op_addps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    subps xmm0, xmm1
ELSE
    addps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_addps ENDP

op_addps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    addps xmm0, XMMWORD PTR [rdx+16]
ELSE
    addps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_addps_mem ENDP

op_addps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    addps xmm0, XMMWORD PTR [rdx]
ELSE
    addps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_addps_mem_disp ENDP

op_addps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    addps xmm0, xmm1
ELSE
    addps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_addps_self ENDP

op_subps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    addps xmm0, xmm1
ELSE
    subps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_subps ENDP

op_subps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    subps xmm0, XMMWORD PTR [rdx+16]
ELSE
    subps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_subps_mem ENDP

op_subps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    subps xmm0, XMMWORD PTR [rdx]
ELSE
    subps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_subps_mem_disp ENDP

op_subps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    subps xmm0, xmm1
ELSE
    subps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_subps_self ENDP

op_mulps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    divps xmm0, xmm1
ELSE
    mulps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_mulps ENDP

op_mulps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    mulps xmm0, XMMWORD PTR [rdx+16]
ELSE
    mulps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_mulps_mem ENDP

op_mulps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    mulps xmm0, XMMWORD PTR [rdx]
ELSE
    mulps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_mulps_mem_disp ENDP

op_mulps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    mulps xmm0, xmm1
ELSE
    mulps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_mulps_self ENDP

op_divps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    mulps xmm0, xmm1
ELSE
    divps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_divps ENDP

op_divps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    divps xmm0, XMMWORD PTR [rdx+16]
ELSE
    divps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_divps_mem ENDP

op_divps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    divps xmm0, XMMWORD PTR [rdx]
ELSE
    divps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_divps_mem_disp ENDP

op_divps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    divps xmm0, xmm1
ELSE
    divps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_divps_self ENDP

op_minps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    maxps xmm0, xmm1
ELSE
    minps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_minps ENDP

op_minps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    minps xmm0, XMMWORD PTR [rdx+16]
ELSE
    minps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_minps_mem ENDP

op_minps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    minps xmm0, XMMWORD PTR [rdx]
ELSE
    minps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_minps_mem_disp ENDP

op_minps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    minps xmm0, xmm1
ELSE
    minps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_minps_self ENDP

op_maxps PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    minps xmm0, xmm1
ELSE
    maxps xmm0, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_maxps ENDP

op_maxps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    maxps xmm0, XMMWORD PTR [rdx+16]
ELSE
    maxps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_maxps_mem ENDP

op_maxps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    maxps xmm0, XMMWORD PTR [rdx]
ELSE
    maxps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_maxps_mem_disp ENDP

op_maxps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    maxps xmm0, xmm1
ELSE
    maxps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_maxps_self ENDP

op_sqrtps PROC FRAME
    .endprolog
    movaps xmm1, XMMWORD PTR [rdx]
    movaps xmm2, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    addps xmm2, xmm1
ELSE
    sqrtps xmm2, xmm1
ENDIF
    movaps XMMWORD PTR [r8], xmm2
    ret
op_sqrtps ENDP

op_sqrtps_mem PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    sqrtps xmm0, XMMWORD PTR [rdx+16]
ELSE
    sqrtps xmm0, XMMWORD PTR [rdx]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_sqrtps_mem ENDP

op_sqrtps_mem_disp PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
IFDEF ALTER_IMM
    sqrtps xmm0, XMMWORD PTR [rdx]
ELSE
    sqrtps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_sqrtps_mem_disp ENDP

op_sqrtps_self PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_IMM
    addps xmm0, xmm1
ELSE
    sqrtps xmm0, xmm0
ENDIF
    movaps XMMWORD PTR [r8], xmm0
    ret
op_sqrtps_self ENDP

; Chained arithmetic: every operation feeds the next through registers, with a memory
; source at +16 and a destination reused across instructions.
op_arith_pipeline PROC FRAME
    .endprolog
    movaps xmm0, XMMWORD PTR [rcx]
    movaps xmm1, XMMWORD PTR [rdx]
    addps xmm0, xmm1
IFDEF ALTER_IMM
    divps xmm0, XMMWORD PTR [rdx+16]
ELSE
    mulps xmm0, XMMWORD PTR [rdx+16]
ENDIF
    subps xmm0, xmm1
    movaps xmm2, XMMWORD PTR [rcx+16]
    divps xmm2, xmm0
    minps xmm2, xmm1
    maxps xmm0, xmm2
    sqrtps xmm3, xmm0
    movaps XMMWORD PTR [r8], xmm3
    movaps XMMWORD PTR [r8+16], xmm2
    ret
op_arith_pipeline ENDP

END
