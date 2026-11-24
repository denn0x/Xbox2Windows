; Source-built MASM fixture for the memory-source forms of the eight AVX logical
; ops (vandpd, vandps, vandnpd, vandnps, vorpd, vorps, vxorpd, vxorps): the last
; source is a flat [base + disp] operand, m128 for VEX.128 and m256 for VEX.256.
; Every export uses the Windows x64 leaf ABI, with three 96-byte buffers:
;   RCX = const void* a  (a[0..31] is the register source; [rcx+disp] may also be the memory source)
;   RDX = const void* b  (b[0..31] seeds the destination; [rdx+disp] may be the memory source)
;   R8  = void*       c  (c[0..31] receives the YMM result; [r8+disp], disp >= 32, may be the memory source)
; The register source is loaded as a full 256-bit vector, so its upper half is
; live and must never reach a VEX.128 result. Odd-numbered ops use the destination
; as the register source. ALTER_MUT swaps the VANDN operand roles in the four
; vandn* exports and keeps the old upper half in op_orpd_mem128 and op_xorps_mem128.
.code

op_andpd_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vandpd xmm2, xmm1, XMMWORD PTR [rdx+16]
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_andpd_mem128 ENDP

op_andps_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vandps xmm1, xmm1, XMMWORD PTR [rcx+32]
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_andps_mem128 ENDP

op_andnpd_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vmovdqu xmm3, XMMWORD PTR [r8+32]
    vandnpd xmm2, xmm3, XMMWORD PTR [rcx]
ELSE
    vandnpd xmm2, xmm1, XMMWORD PTR [r8+32]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_andnpd_mem128 ENDP

op_andnps_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vmovdqu xmm3, XMMWORD PTR [rdx+64]
    vandnps xmm1, xmm3, XMMWORD PTR [rcx]
ELSE
    vandnps xmm1, xmm1, XMMWORD PTR [rdx+64]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_andnps_mem128 ENDP

op_orpd_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vorpd xmm3, xmm1, XMMWORD PTR [rcx+64]
    vblendps ymm2, ymm2, ymm3, 0Fh
ELSE
    vorpd xmm2, xmm1, XMMWORD PTR [rcx+64]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_orpd_mem128 ENDP

op_orps_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vorps xmm1, xmm1, XMMWORD PTR [r8+64]
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_orps_mem128 ENDP

op_xorpd_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vxorpd xmm2, xmm1, XMMWORD PTR [rdx+48]
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_xorpd_mem128 ENDP

op_xorps_mem128 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vxorps xmm3, xmm1, XMMWORD PTR [rdx]
    vblendps ymm1, ymm1, ymm3, 0Fh
ELSE
    vxorps xmm1, xmm1, XMMWORD PTR [rdx]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_xorps_mem128 ENDP

op_andpd_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vandpd ymm2, ymm1, YMMWORD PTR [rcx+32]
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_andpd_mem256 ENDP

op_andps_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vandps ymm1, ymm1, YMMWORD PTR [r8+32]
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_andps_mem256 ENDP

op_andnpd_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vmovdqu ymm3, YMMWORD PTR [rdx+64]
    vandnpd ymm2, ymm3, YMMWORD PTR [rcx]
ELSE
    vandnpd ymm2, ymm1, YMMWORD PTR [rdx+64]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_andnpd_mem256 ENDP

op_andnps_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
IFDEF ALTER_MUT
    vmovdqu ymm3, YMMWORD PTR [rdx+32]
    vandnps ymm1, ymm3, YMMWORD PTR [rcx]
ELSE
    vandnps ymm1, ymm1, YMMWORD PTR [rdx+32]
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_andnps_mem256 ENDP

op_orpd_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vorpd ymm2, ymm1, YMMWORD PTR [rcx+40]
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_orpd_mem256 ENDP

op_orps_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vorps ymm1, ymm1, YMMWORD PTR [rdx+8]
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_orps_mem256 ENDP

op_xorpd_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm2, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vxorpd ymm2, ymm1, YMMWORD PTR [r8+64]
    vmovdqu YMMWORD PTR [r8], ymm2
    vzeroupper
    ret
op_xorpd_mem256 ENDP

op_xorps_mem256 PROC FRAME
    .endprolog
    vmovdqu ymm1, YMMWORD PTR [rcx]
    vxorps ymm1, ymm1, YMMWORD PTR [rcx+64]
    vmovdqu YMMWORD PTR [r8], ymm1
    vzeroupper
    ret
op_xorps_mem256 ENDP

END
