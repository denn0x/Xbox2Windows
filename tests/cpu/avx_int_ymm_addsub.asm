; coverage: source cpu_aot_avx_int_ymm_addsub
; Source-owned differential fixture for the 256-bit (VEX.L=1, AVX2) packed integer
; add, subtract and compare forms: VPADDB/W/D/Q, VPSUBB/W/D/Q, VPCMPEQB/W/D/Q and
; VPCMPGTB/W/D/Q with YMM register operands.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes)
;   RDX = const void* input_b (32 bytes)
;   R8  = void*       output  (32 bytes)
.code

op_vpaddb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddb_ymm ENDP

op_vpaddw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddw_ymm ENDP

op_vpaddd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddd_ymm ENDP

op_vpaddq_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddq ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddq_ymm ENDP

op_vpsubb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubb_ymm ENDP

op_vpsubw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubw_ymm ENDP

op_vpsubd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubd_ymm ENDP

op_vpsubq_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubq ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpsubq_ymm ENDP

op_vpcmpeqb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpeqb_ymm ENDP

op_vpcmpeqw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpeqw_ymm ENDP

op_vpcmpeqd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpeqd_ymm ENDP

op_vpcmpeqq_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqq ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpeqq_ymm ENDP

op_vpcmpgtb_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtb ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpgtb_ymm ENDP

op_vpcmpgtw_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtw ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpgtw_ymm ENDP

op_vpcmpgtd_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtd ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpgtd_ymm ENDP

op_vpcmpgtq_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtq ymm0, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpgtq_ymm ENDP

op_vpsubq_ymm_dst_src1 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubq ymm1, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpsubq_ymm_dst_src1 ENDP

op_vpsubd_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpsubd ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpsubd_ymm_dst_src2 ENDP

op_vpcmpgtq_ymm_dst_src2 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtq ymm2, ymm1, ymm2
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_vpcmpgtq_ymm_dst_src2 ENDP

op_vpaddw_ymm_all PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddw ymm1, ymm1, ymm1
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpaddw_ymm_all ENDP

op_vpcmpeqd_ymm_all PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqd ymm1, ymm1, ymm1
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpcmpeqd_ymm_all ENDP

op_vpcmpgtb_ymm_all PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpgtb ymm1, ymm1, ymm1
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_vpcmpgtb_ymm_all ENDP

op_vpaddq_ymm_then_xmm_subb PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddq ymm0, ymm1, ymm2
    vpsubb xmm0, xmm0, xmm1
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpaddq_ymm_then_xmm_subb ENDP

op_vpcmpeqw_ymm_then_xmm_addb PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpcmpeqw ymm0, ymm1, ymm2
    vpaddb xmm0, xmm0, xmm0
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_vpcmpeqw_ymm_then_xmm_addb ENDP

op_chain_ymm PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vpaddd ymm0, ymm1, ymm2
    vpsubw ymm3, ymm0, ymm2
    vpcmpgtd ymm4, ymm3, ymm1
    vpaddq ymm0, ymm4, ymm3
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    ret
op_chain_ymm ENDP

END
