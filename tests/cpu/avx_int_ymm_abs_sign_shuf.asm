; coverage: source cpu_aot_avx_int_ymm_abs_sign_shuf
; Source-owned differential fixture for the 256-bit (VEX.L=1, AVX2) packed integer
; absolute value, sign and word shuffle forms: VPABSB/W/D, VPSIGNB/W/D, VPSHUFHW and
; VPSHUFLW with YMM register operands. Every 128-bit half is processed on its own.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes)
;   RDX = const void* input_b (32 bytes, read only by the sign forms)
;   R8  = void*       output  (32 bytes)
.code

op_vpabsb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpabsb ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpabsb_ymm ENDP

op_vpabsw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpabsw ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpabsw_ymm ENDP

op_vpabsd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpabsd ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpabsd_ymm ENDP

op_vpsignb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsignb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsignb_ymm ENDP

op_vpsignw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsignw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsignw_ymm ENDP

op_vpsignd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsignd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsignd_ymm ENDP

op_vpshufhw_ymm_1b PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpshufhw ymm0, ymm1, 1Bh
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpshufhw_ymm_1b ENDP

op_vpshufhw_ymm_93 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpshufhw ymm0, ymm1, 93h
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpshufhw_ymm_93 ENDP

op_vpshuflw_ymm_1b PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpshuflw ymm0, ymm1, 1Bh
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpshuflw_ymm_1b ENDP

op_vpshuflw_ymm_93 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpshuflw ymm0, ymm1, 93h
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpshuflw_ymm_93 ENDP

op_vpabsd_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpabsd ymm1, ymm1
    vmovdqu ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpabsd_ymm_dst_src1 ENDP

op_vpsignw_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsignw ymm2, ymm1, ymm2
    vmovdqu ymm0, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsignw_ymm_dst_src2 ENDP

op_vpsignb_ymm_all PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsignb ymm1, ymm1, ymm1
    vmovdqu ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsignb_ymm_all ENDP

op_vpshufhw_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpshufhw ymm1, ymm1, 1Bh
    vmovdqu ymm0, ymm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpshufhw_ymm_dst_src1 ENDP

op_vpabsb_ymm_then_xmm_vpsignb PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpabsb ymm0, ymm1
    vpsignb xmm0, xmm0, xmm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpabsb_ymm_then_xmm_vpsignb ENDP

END
