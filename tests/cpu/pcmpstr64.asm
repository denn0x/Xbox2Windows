; coverage: record cpu_aot_pcmpstr64
; Source-built MASM fixture exercising 64-bit explicit-length packed string compares
; (pcmpestri64, pcmpestrm64, vpcmpestri64, vpcmpestrm64) from docs/coverage/x64_zen2.tsv.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const void* input_a (32 bytes; [0..15] string A)
;   RDX = const void* input_b (16 bytes; string B)
;   R8  = void*       io      (64 bytes; [0..7] signed int64 la / [8..15] signed int64 lb on input;
;                              [16..31] YMM0[255:128] sentinel on input for *strm64;
;                              on output [0..7] RCX index for *stri64, [0..31] YMM0 for *strm64,
;                              [32..47] staged m128 or preserved XMM1)
; Return value in RAX: (CF << 0) | (ZF << 1) | (SF << 2) | (OF << 3) | (PF << 4)

PCMPESTRI64_XMM1_XMM2 MACRO imm
    DB 66h, 48h, 0Fh, 3Ah, 61h, 0CAh, imm
ENDM

PCMPESTRI64_XMM0_XMM0 MACRO imm
    DB 66h, 48h, 0Fh, 3Ah, 61h, 0C0h, imm
ENDM

PCMPESTRI64_XMM1_R8_32 MACRO imm
    DB 66h, 49h, 0Fh, 3Ah, 61h, 48h, 20h, imm
ENDM

PCMPESTRM64_XMM1_XMM2 MACRO imm
    DB 66h, 48h, 0Fh, 3Ah, 60h, 0CAh, imm
ENDM

PCMPESTRM64_XMM0_XMM1 MACRO imm
    DB 66h, 48h, 0Fh, 3Ah, 60h, 0C1h, imm
ENDM

PCMPESTRM64_XMM1_R8_32 MACRO imm
    DB 66h, 49h, 0Fh, 3Ah, 60h, 48h, 20h, imm
ENDM

VPCMPESTRI64_XMM1_XMM2 MACRO imm
    DB 0C4h, 0E3h, 0F9h, 61h, 0CAh, imm
ENDM

VPCMPESTRI64_XMM0_XMM0 MACRO imm
    DB 0C4h, 0E3h, 0F9h, 61h, 0C0h, imm
ENDM

VPCMPESTRI64_XMM1_R8_32 MACRO imm
    DB 0C4h, 0C3h, 0F9h, 61h, 48h, 20h, imm
ENDM

VPCMPESTRM64_XMM1_XMM2 MACRO imm
    DB 0C4h, 0E3h, 0F9h, 60h, 0CAh, imm
ENDM

VPCMPESTRM64_XMM0_XMM1 MACRO imm
    DB 0C4h, 0E3h, 0F9h, 60h, 0C1h, imm
ENDM

VPCMPESTRM64_XMM1_R8_32 MACRO imm
    DB 0C4h, 0C3h, 0F9h, 60h, 48h, 20h, imm
ENDM

VPCMPESTRM64_XMM1_RDX MACRO imm
    DB 0C4h, 0E3h, 0F9h, 60h, 0Ah, imm
ENDM

PACK_FLAGS MACRO
    setb al
    sete r9b
    sets r10b
    seto r11b
    setp cl
    movzx eax, al
    movzx r9d, r9b
    shl r9d, 1
    or eax, r9d
    movzx r10d, r10b
    shl r10d, 2
    or eax, r10d
    movzx r11d, r11b
    shl r11d, 3
    or eax, r11d
    movzx ecx, cl
    shl ecx, 4
    or eax, ecx
    ret
ENDM

.code

; --- Legacy SSE4.2 PCMPESTRI64 (REX.W=1, index in RCX) ---

pcmpestri64_ub_eqany_pos_lsb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov r10d, -1
    or r10d, r10d
    mov rcx, -1
IFDEF ALTER_IMM
    PCMPESTRI64_XMM1_XMM2 01h
ELSE
    PCMPESTRI64_XMM1_XMM2 00h
ENDIF
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
pcmpestri64_ub_eqany_pos_lsb ENDP

pcmpestri64_sw_ranges_neg_msb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    PCMPESTRI64_XMM1_XMM2 57h
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
pcmpestri64_sw_ranges_neg_msb ENDP

pcmpestri64_mem_sb_eqeach_mpos_msb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    vmovdqu XMMWORD PTR [r8+32], xmm2
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    PCMPESTRI64_XMM1_R8_32 6Ah
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
pcmpestri64_mem_sb_eqeach_mpos_msb ENDP

pcmpestri64_alias_xmm0 PROC FRAME
    .endprolog
    vmovdqu xmm0, XMMWORD PTR [rcx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    PCMPESTRI64_XMM0_XMM0 9Ch
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
pcmpestri64_alias_xmm0 ENDP

; --- Legacy SSE4.2 PCMPESTRM64 (REX.W=1, mask in XMM0, preserves YMM0[255:128]) ---

pcmpestrm64_ub_eqany_pos_bit PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov r10d, -1
    or r10d, r10d
    PCMPESTRM64_XMM1_XMM2 00h
    vmovdqu YMMWORD PTR [r8], ymm0
    vmovdqu XMMWORD PTR [r8+32], xmm1
    vzeroupper
    PACK_FLAGS
pcmpestrm64_ub_eqany_pos_bit ENDP

pcmpestrm64_sb_ranges_neg_exp PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
IFDEF ALTER_IMM
    PCMPESTRM64_XMM1_XMM2 16h
ELSE
    PCMPESTRM64_XMM1_XMM2 56h
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    PACK_FLAGS
pcmpestrm64_sb_ranges_neg_exp ENDP

pcmpestrm64_mem_uw_eqord_mneg_exp PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    vmovdqu XMMWORD PTR [r8+32], xmm2
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    PCMPESTRM64_XMM1_R8_32 7Dh
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    PACK_FLAGS
pcmpestrm64_mem_uw_eqord_mneg_exp ENDP

pcmpestrm64_xmm0_self PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [rcx]
    vmovdqu xmm1, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    PCMPESTRM64_XMM0_XMM1 40h
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    PACK_FLAGS
pcmpestrm64_xmm0_self ENDP

; --- AVX VEX.128 VPCMPESTRI64 (W=1, index in RCX) ---

vpcmpestri64_ub_eqany_pos_lsb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov r10d, -1
    or r10d, r10d
    mov rcx, -1
IFDEF ALTER_IMM
    VPCMPESTRI64_XMM1_XMM2 01h
ELSE
    VPCMPESTRI64_XMM1_XMM2 00h
ENDIF
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
vpcmpestri64_ub_eqany_pos_lsb ENDP

vpcmpestri64_uw_eqord_mneg_lsb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    VPCMPESTRI64_XMM1_XMM2 3Dh
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
vpcmpestri64_uw_eqord_mneg_lsb ENDP

vpcmpestri64_mem_sw_ranges_neg_msb PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    vmovdqu XMMWORD PTR [r8+32], xmm2
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    VPCMPESTRI64_XMM1_R8_32 57h
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
vpcmpestri64_mem_sw_ranges_neg_msb ENDP

vpcmpestri64_alias_xmm0 PROC FRAME
    .endprolog
    vmovdqu xmm0, XMMWORD PTR [rcx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov rcx, -1
    VPCMPESTRI64_XMM0_XMM0 9Ch
    mov QWORD PTR [r8], rcx
    vzeroupper
    PACK_FLAGS
vpcmpestri64_alias_xmm0 ENDP

; --- AVX VEX.128 VPCMPESTRM64 (W=1, mask in XMM0, zeroes YMM0[255:128]) ---

vpcmpestrm64_ub_eqany_pos_bit PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    mov r10d, -1
    or r10d, r10d
    VPCMPESTRM64_XMM1_XMM2 00h
    vmovdqu YMMWORD PTR [r8], ymm0
    vmovdqu XMMWORD PTR [r8+32], xmm1
    vzeroupper
    PACK_FLAGS
vpcmpestrm64_ub_eqany_pos_bit ENDP

vpcmpestrm64_uw_eqeach_mpos_exp PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
IFDEF ALTER_IMM
    VPCMPESTRM64_XMM1_XMM2 29h
ELSE
    VPCMPESTRM64_XMM1_XMM2 69h
ENDIF
    vmovdqu YMMWORD PTR [r8], ymm0
    vmovdqu XMMWORD PTR [r8+32], xmm1
    vzeroupper
    PACK_FLAGS
vpcmpestrm64_uw_eqeach_mpos_exp ENDP

vpcmpestrm64_mem_ub_eqany_exp PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [r8]
    vmovdqu xmm1, XMMWORD PTR [rcx]
    vmovdqu xmm2, XMMWORD PTR [rdx]
    vmovdqu XMMWORD PTR [r8+32], xmm2
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    VPCMPESTRM64_XMM1_R8_32 40h
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    PACK_FLAGS
vpcmpestrm64_mem_ub_eqany_exp ENDP

vpcmpestrm64_xmm0_self PROC FRAME
    .endprolog
    vmovdqu ymm0, YMMWORD PTR [rcx]
    vmovdqu xmm1, XMMWORD PTR [rdx]
    mov rax, QWORD PTR [r8]
    mov rdx, QWORD PTR [r8+8]
    VPCMPESTRM64_XMM0_XMM1 40h
    vmovdqu YMMWORD PTR [r8], ymm0
    vzeroupper
    PACK_FLAGS
vpcmpestrm64_xmm0_self ENDP

; The discarded memory read must still fault after the first store and before the second.
vpcmpestrm64_discarded_read PROC FRAME
    .endprolog
    vmovdqu xmm1, XMMWORD PTR [rcx]
    mov rax, 4
    mov r10d, 11223344h
    mov DWORD PTR [r8], r10d
    VPCMPESTRM64_XMM1_RDX 00h
    mov eax, 55667788h
    mov DWORD PTR [r8], eax
    vzeroupper
    ret
vpcmpestrm64_discarded_read ENDP

END
