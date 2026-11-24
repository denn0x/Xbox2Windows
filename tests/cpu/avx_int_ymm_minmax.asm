; coverage: source cpu_aot_avx_int_ymm_minmax
; Source-owned differential fixture for the 256-bit (VEX.L=1, AVX2) packed integer
; minimum and maximum forms: VPMAXSB/SW/SD/UB/UW/UD and VPMINSB/SW/SD/UB/UW/UD with
; YMM register operands.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes)
;   RDX = const void* input_b (32 bytes)
;   R8  = void*       output  (32 bytes)
.code

op_vpmaxsb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxsb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxsb_ymm ENDP

op_vpmaxsw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxsw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxsw_ymm ENDP

op_vpmaxsd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxsd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxsd_ymm ENDP

op_vpmaxub_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxub ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxub_ymm ENDP

op_vpmaxuw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxuw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxuw_ymm ENDP

op_vpmaxud_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxud ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxud_ymm ENDP

op_vpminsb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminsb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminsb_ymm ENDP

op_vpminsw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminsw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminsw_ymm ENDP

op_vpminsd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminsd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminsd_ymm ENDP

op_vpminub_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminub ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminub_ymm ENDP

op_vpminuw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminuw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminuw_ymm ENDP

op_vpminud_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminud ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpminud_ymm ENDP

op_vpmaxsd_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxsd ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpmaxsd_ymm_dst_src2 ENDP

op_vpminuw_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpminuw ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpminuw_ymm_dst_src1 ENDP

op_vpmaxub_ymm_then_xmm_minsb PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpmaxub ymm0, ymm1, ymm2
    vpminsb xmm0, xmm0, xmm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpmaxub_ymm_then_xmm_minsb ENDP

END
