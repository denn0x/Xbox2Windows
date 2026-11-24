; coverage: source cpu_aot_switch_bswap
; Source-owned differential fixture and negative controls for BSWAP in tools/cpu_switch_aot.py.
; Every differential export takes RCX = x, RDX = y and R8 = address of three writable QWORDs. It runs
; CMP rcx, rdx, loads y into one register, reverses its bytes (bswap32_REG on the low half, bswap64_REG
; on all of it) and stores the sixteen conditions O NO B AE E NE BE A S NS P NP L GE LE G as bytes at
; [r8], then the whole 64-bit register at [r8+16]; RAX returns it. BSWAP leaves every flag unchanged:
; the first eight conditions are read after one BSWAP and the other eight after a second one, each after
; a fresh CMP of copies of x and y that the register under test cannot overwrite.
.code

BSWAP_BODY MACRO full, part, addr, hold, hold2
    mov addr, r8
    mov hold, rcx
    mov hold2, rdx
    cmp hold, hold2
    mov full, hold2
    bswap part
    seto BYTE PTR [addr+0]
    setno BYTE PTR [addr+1]
    setb BYTE PTR [addr+2]
    setae BYTE PTR [addr+3]
    sete BYTE PTR [addr+4]
    setne BYTE PTR [addr+5]
    setbe BYTE PTR [addr+6]
    seta BYTE PTR [addr+7]
    cmp hold, hold2
    mov full, hold2
    bswap part
    sets BYTE PTR [addr+8]
    setns BYTE PTR [addr+9]
    setp BYTE PTR [addr+10]
    setnp BYTE PTR [addr+11]
    setl BYTE PTR [addr+12]
    setge BYTE PTR [addr+13]
    setle BYTE PTR [addr+14]
    setg BYTE PTR [addr+15]
    mov QWORD PTR [addr+16], full
    mov rax, full
ENDM

BSWAP_VOLATILE MACRO name, full, part, addr, hold, hold2
name PROC FRAME
    .endprolog
    BSWAP_BODY full, part, addr, hold, hold2
    ret
name ENDP
ENDM

BSWAP_SAVED MACRO name, full, part
name PROC FRAME
    push full
    .pushreg full
    .endprolog
    BSWAP_BODY full, part, r8, r10, r11
    pop full
    ret
name ENDP
ENDM

BSWAP_VOLATILE bswap32_rax, rax, eax, r8, r10, r11
BSWAP_VOLATILE bswap64_rax, rax, rax, r8, r10, r11
BSWAP_VOLATILE bswap32_rcx, rcx, ecx, r8, r10, r11
BSWAP_VOLATILE bswap64_rcx, rcx, rcx, r8, r10, r11
BSWAP_VOLATILE bswap32_rdx, rdx, edx, r8, r10, r11
BSWAP_VOLATILE bswap64_rdx, rdx, rdx, r8, r10, r11
BSWAP_VOLATILE bswap32_r8, r8, r8d, r9, r10, r11
BSWAP_VOLATILE bswap64_r8, r8, r8, r9, r10, r11
BSWAP_VOLATILE bswap32_r9, r9, r9d, r8, r10, r11
BSWAP_VOLATILE bswap64_r9, r9, r9, r8, r10, r11
BSWAP_VOLATILE bswap32_r10, r10, r10d, r8, rax, r9
BSWAP_VOLATILE bswap64_r10, r10, r10, r8, rax, r9
BSWAP_VOLATILE bswap32_r11, r11, r11d, r8, rax, r9
BSWAP_VOLATILE bswap64_r11, r11, r11, r8, rax, r9
BSWAP_SAVED bswap32_rbx, rbx, ebx
BSWAP_SAVED bswap64_rbx, rbx, rbx
BSWAP_SAVED bswap32_rbp, rbp, ebp
BSWAP_SAVED bswap64_rbp, rbp, rbp
BSWAP_SAVED bswap32_rsi, rsi, esi
BSWAP_SAVED bswap64_rsi, rsi, rsi
BSWAP_SAVED bswap32_rdi, rdi, edi
BSWAP_SAVED bswap64_rdi, rdi, rdi
BSWAP_SAVED bswap32_r12, r12, r12d
BSWAP_SAVED bswap64_r12, r12, r12
BSWAP_SAVED bswap32_r13, r13, r13d
BSWAP_SAVED bswap64_r13, r13, r13
BSWAP_SAVED bswap32_r14, r14, r14d
BSWAP_SAVED bswap64_r14, r14, r14
BSWAP_SAVED bswap32_r15, r15, r15d
BSWAP_SAVED bswap64_r15, r15, r15

; BSWAP at 64 bits then at 32 bits: the low half of the first result is the high half of y.
bswap_mixed PROC FRAME
    .endprolog
    mov rax, rdx
    bswap rax
    bswap eax
    ret
bswap_mixed ENDP

; The flags of CMP survive BSWAP into a conditional branch and a conditional move.
jcc_bswap_b PROC FRAME
    .endprolog
    cmp rcx, rdx
    mov rax, rdx
    bswap rax
    jb taken
    ret
taken:
    not rax
    ret
jcc_bswap_b ENDP

cmov_bswap PROC FRAME
    .endprolog
    cmp rcx, rdx
    mov eax, ecx
    bswap eax
    mov r9, rdx
    bswap r9
    cmovl rax, r9
    ret
cmov_bswap ENDP

; Rejections. The 16-bit form (66h prefix) has an undefined result in the Intel SDM; the other forms use
; a prefix BSWAP does not take, an unwritten source register and RSP, which the frame analysis owns.
neg_bswap16 PROC FRAME
    .endprolog
    mov eax, ecx
    db 66h, 0fh, 0c8h
    ret
neg_bswap16 ENDP

neg_bswap_rep PROC FRAME
    .endprolog
    mov rax, rcx
    db 0f3h, 48h, 0fh, 0c8h
    ret
neg_bswap_rep ENDP

neg_bswap_undef PROC FRAME
    .endprolog
    bswap r10
    mov rax, r10
    ret
neg_bswap_undef ENDP

neg_bswap_rsp PROC FRAME
    .endprolog
    mov rax, rcx
    bswap rsp
    ret
neg_bswap_rsp ENDP

END
