; coverage: source cpu_aot_integer_flags
; Source-built MASM fixture exercising uncovered Integer form `lahf` from
; docs/coverage/x64_zen2.tsv after same-block register ADD, SUB and CMP across
; 8-, 16-, 32- and 64-bit widths, with full RAX output and live SETcc consumers.
; Every export uses the Windows x64 leaf ABI:
;   RCX = const uint64_t* input_a (32 bytes)
;   RDX = const uint64_t* input_b (32 bytes)
;   R8  = uint64_t*       output  (32 bytes)

LAHF_STORE_AND_CONSUME MACRO
    lahf
    mov QWORD PTR [r8], rax
    setb BYTE PTR [r8+8]
    setl BYTE PTR [r8+9]
    sete BYTE PTR [r8+10]
    sets BYTE PTR [r8+11]
    seto BYTE PTR [r8+12]
    ret
ENDM

.code

op_add8_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    add r9b, r10b
    LAHF_STORE_AND_CONSUME
op_add8_lahf ENDP

op_add16_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    add r9w, r10w
    LAHF_STORE_AND_CONSUME
op_add16_lahf ENDP

op_add32_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    add r9d, r10d
    LAHF_STORE_AND_CONSUME
op_add32_lahf ENDP

op_add64_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    add r9, r10
    LAHF_STORE_AND_CONSUME
op_add64_lahf ENDP

op_sub8_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    sub r9b, r10b
    LAHF_STORE_AND_CONSUME
op_sub8_lahf ENDP

op_sub16_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    sub r9w, r10w
    LAHF_STORE_AND_CONSUME
op_sub16_lahf ENDP

op_sub32_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    sub r9d, r10d
    LAHF_STORE_AND_CONSUME
op_sub32_lahf ENDP

op_sub64_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    sub r9, r10
    LAHF_STORE_AND_CONSUME
op_sub64_lahf ENDP

op_cmp8_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    cmp r9b, r10b
    LAHF_STORE_AND_CONSUME
op_cmp8_lahf ENDP

op_cmp16_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    cmp r9w, r10w
    LAHF_STORE_AND_CONSUME
op_cmp16_lahf ENDP

op_cmp32_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    cmp r9d, r10d
    LAHF_STORE_AND_CONSUME
op_cmp32_lahf ENDP

op_cmp64_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+8]
    cmp r9, r10
    LAHF_STORE_AND_CONSUME
op_cmp64_lahf ENDP

op_add8_al_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
IFDEF ALTER_IMM
    add al, 5
ELSE
    add al, r9b
ENDIF
    LAHF_STORE_AND_CONSUME
op_add8_al_lahf ENDP

op_add32_imm_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
IFDEF ALTER_IMM
    add r9d, 1
ELSE
    add r9d, 17
ENDIF
    LAHF_STORE_AND_CONSUME
op_add32_imm_lahf ENDP

op_sub32_imm_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
IFDEF ALTER_IMM
    sub r9d, 1
ELSE
    sub r9d, 15
ENDIF
    LAHF_STORE_AND_CONSUME
op_sub32_imm_lahf ENDP

op_cmp64_imm_lahf PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov r9, QWORD PTR [rdx]
IFDEF ALTER_IMM
    cmp r9, 1
ELSE
    cmp r9, 17
ENDIF
    LAHF_STORE_AND_CONSUME
op_cmp64_imm_lahf ENDP

; The discarded read must still fault after the first store and before the second.
op_discarded_read PROC FRAME
    .endprolog
    mov rax, QWORD PTR [rcx]
    mov QWORD PTR [r8], rax
    mov r9, QWORD PTR [rdx]
    mov QWORD PTR [r8+8], rax
    ret
op_discarded_read ENDP

END
