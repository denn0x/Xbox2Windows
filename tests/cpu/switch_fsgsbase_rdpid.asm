; coverage: record cpu_aot_switch_fsgsbase_rdpid
; Source-built MASM fixture for read-only RDFSBASE, RDGSBASE, and RDPID in
; tools/cpu_switch_aot.py (--host-cpu opt-in), plus native CPUID/RDTSCP/FS_BASE
; helpers and negative controls (including host segment-base write rejections).
; Every translated export uses the Windows x64 ABI: RCX, RDX, R8 -> RAX.
.code

; --- Translated positive exports ---

rdfsbase_32 PROC FRAME
    .endprolog
    mov rax, -1
    rdfsbase eax
    ret
rdfsbase_32 ENDP

rdfsbase_64 PROC FRAME
    .endprolog
    rdfsbase rax
    ret
rdfsbase_64 ENDP

rdgsbase_32 PROC FRAME
    .endprolog
    mov rax, -1
    rdgsbase eax
    ret
rdgsbase_32 ENDP

rdgsbase_64 PROC FRAME
    .endprolog
    rdgsbase rax
    ret
rdgsbase_64 ENDP

read_host_nonvolatile PROC FRAME
    push rbx
    .pushreg rbx
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    .endprolog
    rdfsbase rbx
    rdgsbase r12
    rdpid r13
    mov QWORD PTR [r8], rbx
    mov QWORD PTR [r8+8], r12
    mov QWORD PTR [r8+16], r13
    mov rax, rbx
    xor rax, r12
    xor rax, r13
    pop r13
    pop r12
    pop rbx
    ret
read_host_nonvolatile ENDP

rdpid_64 PROC FRAME
    .endprolog
    mov rax, -1
    rdpid rax
    ret
rdpid_64 ENDP

rdpid_twice PROC FRAME
    .endprolog
    rdpid r9
    rdpid r10
    mov QWORD PTR [r8], r9
    mov QWORD PTR [r8+8], r10
    mov rax, r9
    ret
rdpid_twice ENDP

flag_preserve_fsgsbase_rdpid PROC FRAME
    .endprolog
    add rcx, rdx
    rdfsbase r9
    rdgsbase r10
    rdpid r11
    setb BYTE PTR [r8]
    sete BYTE PTR [r8+1]
    sets BYTE PTR [r8+2]
    seto BYTE PTR [r8+3]
    setp BYTE PTR [r8+4]
    mov QWORD PTR [r8+8], r9
    mov QWORD PTR [r8+16], r10
    mov QWORD PTR [r8+24], r11
    mov rax, rcx
    ret
flag_preserve_fsgsbase_rdpid ENDP

unused_host_reads PROC FRAME
    .endprolog
    rdfsbase r10
    rdgsbase r10
    rdpid r10
    mov eax, 42
    ret
unused_host_reads ENDP

; --- Native-only host helpers (never translated) ---

native_host_features PROC FRAME
    push rbx
    .pushreg rbx
    .endprolog
    xor eax, eax
    cpuid
    xor r8d, r8d
    cmp eax, 7
    jb done_features
    mov eax, 7
    xor ecx, ecx
    cpuid
    test ebx, 1
    jz check_rdpid
    or r8d, 1
check_rdpid:
    bt ecx, 22
    jnc done_features
    or r8d, 2
done_features:
    mov eax, r8d
    pop rbx
    ret
native_host_features ENDP

native_rdtscp_aux PROC FRAME
    .endprolog
    rdtscp
    mov eax, ecx
    ret
native_rdtscp_aux ENDP

; Temporarily sets FS_BASE to RCX around calling RDX(0, 0, 0) and restores FS_BASE.
; Windows x64 uses GS_BASE for the TEB; FS_BASE is unused by the CRT/OS.
native_with_fsbase PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 32
    .allocstack 32
    .endprolog
    rdfsbase rbx
    wrfsbase rcx
    mov r10, rdx
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    call r10
    wrfsbase rbx
    add rsp, 32
    pop rbx
    ret
native_with_fsbase ENDP

; --- Negative controls (refused by translator; never translated in build) ---

neg_wrfsbase_32 PROC FRAME
    .endprolog
    wrfsbase ecx
    xor eax, eax
    ret
neg_wrfsbase_32 ENDP

neg_wrfsbase_64 PROC FRAME
    .endprolog
    wrfsbase rcx
    xor eax, eax
    ret
neg_wrfsbase_64 ENDP

neg_wrgsbase_32 PROC FRAME
    .endprolog
    wrgsbase ecx
    xor eax, eax
    ret
neg_wrgsbase_32 ENDP

neg_wrgsbase_64 PROC FRAME
    .endprolog
    wrgsbase rcx
    xor eax, eax
    ret
neg_wrgsbase_64 ENDP

neg_rsp_rdfsbase PROC FRAME
    .endprolog
    rdfsbase rsp
    xor eax, eax
    ret
neg_rsp_rdfsbase ENDP

neg_rsp_rdgsbase PROC FRAME
    .endprolog
    rdgsbase rsp
    xor eax, eax
    ret
neg_rsp_rdgsbase ENDP

neg_rsp_rdpid PROC FRAME
    .endprolog
    rdpid rsp
    xor eax, eax
    ret
neg_rsp_rdpid ENDP

END
