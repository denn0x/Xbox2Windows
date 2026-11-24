; coverage: source cpu_aot_avx_int_ymm_sat
; Source-owned differential fixture for the 256-bit (VEX.L=1, AVX2) packed integer
; saturating add and subtract forms: VPADDSB/SW/USB/USW and VPSUBSB/SW/USB/USW with
; YMM register operands. The subtract forms compute src1 - src2 where src1 is the
; first source register operand, so the aliased exports pin the operand order.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes)
;   RDX = const void* input_b (32 bytes)
;   R8  = void*       output  (32 bytes)
.code

op_vpaddsb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddsb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddsb_ymm ENDP

op_vpaddsw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddsw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddsw_ymm ENDP

op_vpaddusb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddusb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddusb_ymm ENDP

op_vpaddusw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddusw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddusw_ymm ENDP

op_vpsubsb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubsb_ymm ENDP

op_vpsubsw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubsw_ymm ENDP

op_vpsubusb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubusb_ymm ENDP

op_vpsubusw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubusw_ymm ENDP

op_vpsubsb_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsb ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpsubsb_ymm_dst_src1 ENDP

op_vpsubsw_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsw ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpsubsw_ymm_dst_src1 ENDP

op_vpsubusb_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusb ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpsubusb_ymm_dst_src1 ENDP

op_vpsubusw_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusw ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpsubusw_ymm_dst_src1 ENDP

op_vpsubsb_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsb ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpsubsb_ymm_dst_src2 ENDP

op_vpsubsw_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubsw ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpsubsw_ymm_dst_src2 ENDP

op_vpsubusb_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusb ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpsubusb_ymm_dst_src2 ENDP

op_vpsubusw_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubusw ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpsubusw_ymm_dst_src2 ENDP

op_vpaddsb_ymm_all PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddsb ymm1, ymm1, ymm1
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpaddsb_ymm_all ENDP

op_vpaddusb_ymm_then_xmm_subsb PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddusb ymm0, ymm1, ymm2
    vpsubsb xmm0, xmm0, xmm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddusb_ymm_then_xmm_subsb ENDP

END
