; coverage: source cpu_aot_avx_fp_move
; Source-built MASM fixture exercising 4 AVX forms from docs/coverage/x64_zen2.tsv
; group AVX: vmovq, vmaskmovdqu, vmovupd and vmovapd, along with VEX transport glue
; (vmovups, vmovaps, vmovdqu, vmovdqa, vzeroupper, 64-bit mov).
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes, 32-byte aligned)
;   RDX = const void* input_b (32 bytes, 32-byte aligned)
;   R8  = void*       output  (32 bytes, 32-byte aligned)
.code

; --- vmovq: XMM<-XMM, XMM<-m64, m64<-XMM, XMM<-r64, r64<-XMM (zeroing [255:64]) ---

op_vmovq_xmm_xmm PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups xmm1, XMMWORD PTR [rdx]
IFDEF ALTER_OP
    vmovq xmm0, xmm0
ELSE
    vmovq xmm0, xmm1
ENDIF
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovq_xmm_xmm ENDP

op_vmovq_xmm_m64 PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
IFDEF ALTER_OP
    vmovq xmm0, QWORD PTR [rdx+9]
ELSE
    vmovq xmm0, QWORD PTR [rdx+1]
ENDIF
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovq_xmm_m64 ENDP

op_vmovq_m64_xmm PROC FRAME
    .endprolog
    vmovups xmm0, XMMWORD PTR [rcx]
    vmovq QWORD PTR [r8+9], xmm0
    ret
op_vmovq_m64_xmm ENDP

op_vmovq_xmm_r64 PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    mov rax, QWORD PTR [rdx]
    vmovq xmm0, rax
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovq_xmm_r64 ENDP

op_vmovq_r64_xmm PROC FRAME
    .endprolog
    vmovups xmm0, XMMWORD PTR [rcx]
    vmovq r9, xmm0
    mov QWORD PTR [r8+8], r9
    ret
op_vmovq_r64_xmm ENDP

; --- vmaskmovdqu: conditional byte store to [rdi] with rdi save/restore ---

op_vmaskmovdqu PROC FRAME
    .endprolog
    vmovdqu xmm0, XMMWORD PTR [rcx]
    vmovdqu xmm1, XMMWORD PTR [rdx]
    mov r10, rdi
    mov rdi, r8
IFDEF ALTER_OP
    vmaskmovdqu xmm1, xmm0
ELSE
    vmaskmovdqu xmm0, xmm1
ENDIF
    mov rdi, r10
    ret
op_vmaskmovdqu ENDP

; --- Chained vzeroupper pipeline and dead-read fault-ordering controls ---

op_pipeline PROC FRAME
    .endprolog
    vmovaps ymm0, YMMWORD PTR [rcx]
    vmovdqa ymm1, YMMWORD PTR [rdx]
    vzeroupper
    vmovq xmm2, QWORD PTR [rdx+16]
    vmovq r9, xmm2
    vmovq xmm3, r9
    vmovdqu XMMWORD PTR [r8+16], xmm3
    vmovups XMMWORD PTR [r8], xmm0
    mov r11, rdi
    mov rdi, r8
    vmaskmovdqu xmm0, xmm1
    mov rdi, r11
    vmovq QWORD PTR [r8+24], xmm2
    ret
op_pipeline ENDP

; The discarded reads must still fault after the first store and before the second.
op_discarded_read PROC FRAME
    .endprolog
    vmovups xmm0, XMMWORD PTR [rcx]
    vmovups XMMWORD PTR [r8], xmm0
    vmovups ymm5, YMMWORD PTR [rdx]
    vmovups XMMWORD PTR [r8+16], xmm0
    ret
op_discarded_read ENDP

op_discarded_read64 PROC FRAME
    .endprolog
    vmovups xmm0, XMMWORD PTR [rcx]
    vmovups XMMWORD PTR [r8], xmm0
    vmovq xmm5, QWORD PTR [rdx]
    vmovups XMMWORD PTR [r8+16], xmm0
    ret
op_discarded_read64 ENDP

; --- vmovupd / vmovapd (VEX.128 and VEX.256): load, store, register copy ---
; Raw data movement: no float conversion and no NaN canonicalisation. Each export
; first loads YMM0 from input_a so any VEX.128 register write must visibly zero
; YMM[255:128]. The aligned forms use displacements that are multiples of the
; operand size from 32-byte aligned buffers; the *_regst exports emit the
; store-form (0F 11 / 0F 29) register-register encodings with raw bytes.

op_vmovupd_xmm_load PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    IFDEF ALTER_OP
    vmovupd xmm0, XMMWORD PTR [rdx+9]
    ELSE
    vmovupd xmm0, XMMWORD PTR [rdx+1]
    ENDIF
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_xmm_load ENDP

op_vmovupd_ymm_load PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovupd ymm0, YMMWORD PTR [rdx]
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_ymm_load ENDP

op_vmovupd_xmm_store PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovupd XMMWORD PTR [r8+5], xmm0
    ret
op_vmovupd_xmm_store ENDP

op_vmovupd_ymm_store PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovupd YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_ymm_store ENDP

op_vmovupd_xmm_reg PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    IFDEF ALTER_OP
    vmovupd ymm0, ymm1
    ELSE
    vmovupd xmm0, xmm1
    ENDIF
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_xmm_reg ENDP

op_vmovupd_ymm_reg PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    vmovupd ymm0, ymm1
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_ymm_reg ENDP

op_vmovupd_xmm_regst PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    ; store-form encoding (0F 11h /r): vmovupd xmm0, xmm1
    db 0C5h, 0F9h, 11h, 0C8h
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_xmm_regst ENDP

op_vmovupd_ymm_regst PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    ; store-form encoding (0F 11h /r): vmovupd ymm0, ymm1
    db 0C5h, 0FDh, 11h, 0C8h
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovupd_ymm_regst ENDP

op_vmovapd_xmm_load PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovapd xmm0, XMMWORD PTR [rdx+16]
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_xmm_load ENDP

op_vmovapd_ymm_load PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    IFDEF ALTER_OP
    vmovapd xmm0, XMMWORD PTR [rdx]
    ELSE
    vmovapd ymm0, YMMWORD PTR [rdx]
    ENDIF
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_ymm_load ENDP

op_vmovapd_xmm_store PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovapd XMMWORD PTR [r8+16], xmm0
    ret
op_vmovapd_xmm_store ENDP

op_vmovapd_ymm_store PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovapd YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_ymm_store ENDP

op_vmovapd_xmm_reg PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    vmovapd xmm0, xmm1
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_xmm_reg ENDP

op_vmovapd_ymm_reg PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    vmovapd ymm0, ymm1
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_ymm_reg ENDP

op_vmovapd_xmm_regst PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    ; store-form encoding (0F 29h /r): vmovapd xmm0, xmm1
    db 0C5h, 0F9h, 29h, 0C8h
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_xmm_regst ENDP

op_vmovapd_ymm_regst PROC FRAME
    .endprolog
    vmovups ymm0, YMMWORD PTR [rcx]
    vmovups ymm1, YMMWORD PTR [rdx]
    ; store-form encoding (0F 29h /r): vmovapd ymm0, ymm1
    db 0C5h, 0FDh, 29h, 0C8h
    vmovups YMMWORD PTR [r8], ymm0
    ret
op_vmovapd_ymm_regst ENDP

op_pd_pipeline PROC FRAME
    .endprolog
    vmovapd ymm0, YMMWORD PTR [rcx]
    vmovupd xmm1, XMMWORD PTR [rdx+1]
    vmovq r9, xmm1
    vmovq xmm2, r9
    vmovapd xmm3, xmm2
    vmovupd XMMWORD PTR [r8+16], xmm3
    vmovapd XMMWORD PTR [r8], xmm0
    ret
op_pd_pipeline ENDP
END
