; coverage: record cpu_counted_clzero
; RCX=start, RDX=end, R8=pointer to three result bytes.
; The reference branch uses Win64 integer stores to the same aligned cache line.
.code
CLEAR_LOOP MACRO reference
    LOCAL again
    mov rax,rcx
again:
    cmp rcx,rdx
    IF reference
        mov r10,rax
        pushfq
        and r10,-64
        popfq
        FOR i,<0,8,16,24,32,40,48,56>
            mov qword ptr [r10+i],0
        ENDM
    ELSE
        db 0fh,01h,0fch
    ENDIF
    setb r9b
    mov [r8],r9b
    add rax,64
    cmp rax,rdx
    sete r9b
    mov [r8+1],r9b
    seta r9b
    mov [r8+2],r9b
    jb again
    ret
ENDM
counted PROC
    CLEAR_LOOP 0
counted ENDP
reference_counted PROC
    CLEAR_LOOP 1
reference_counted ENDP
; Exercise the undecorated four-instruction loop as emitted by optimizing compilers.
PLAIN_LOOP MACRO procname,limitreg
    LOCAL inner
procname PROC
    mov rax,rcx
    mov limitreg,rdx
inner:
    db 0fh,01h,0fch
    add rax,64
    cmp rax,limitreg
    jb inner
    ret
procname ENDP
ENDM
PLAIN_LOOP loop_dx,rdx
PLAIN_LOOP loop_r8,r8
PLAIN_LOOP loop_r9,r9
PLAIN_LOOP loop_r10,r10
prefix_addr32 PROC
    mov rax,rcx
    db 67h,0fh,01h,0fch
    ret
prefix_addr32 ENDP
unknown_rax PROC
    db 0fh,01h,0fch
    ret
unknown_rax ENDP
END
