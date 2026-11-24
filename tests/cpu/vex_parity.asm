; coverage: source cpu_aot_vex_parity
; Source-owned differential fixture for PF after VCOMISS/VUCOMISS/VCOMISD/VUCOMISD and
; SETP/SETNP/CMOVP/CMOVNP/JP/JNP lowering (plus CFG joins and memory-read fault checks).
.code

vcomiss_reg_flags PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    vcomiss xmm0, xmm1
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vcomiss_reg_jp
    mov byte ptr [r8+4], 0
vcomiss_reg_jp:
    mov byte ptr [r8+5], 1
    jnp vcomiss_reg_jnp
    mov byte ptr [r8+5], 0
vcomiss_reg_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vcomiss_reg_flags ENDP

vcomiss_mem_flags PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vcomiss xmm0, dword ptr [rdx]
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vcomiss_mem_jp
    mov byte ptr [r8+4], 0
vcomiss_mem_jp:
    mov byte ptr [r8+5], 1
    jnp vcomiss_mem_jnp
    mov byte ptr [r8+5], 0
vcomiss_mem_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vcomiss_mem_flags ENDP

vucomiss_reg_flags PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    vucomiss xmm0, xmm1
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vucomiss_reg_jp
    mov byte ptr [r8+4], 0
vucomiss_reg_jp:
    mov byte ptr [r8+5], 1
    jnp vucomiss_reg_jnp
    mov byte ptr [r8+5], 0
vucomiss_reg_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vucomiss_reg_flags ENDP

vucomiss_mem_flags PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vucomiss xmm0, dword ptr [rdx]
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vucomiss_mem_jp
    mov byte ptr [r8+4], 0
vucomiss_mem_jp:
    mov byte ptr [r8+5], 1
    jnp vucomiss_mem_jnp
    mov byte ptr [r8+5], 0
vucomiss_mem_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vucomiss_mem_flags ENDP

vcomisd_reg_flags PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [rcx]
    vmovsd xmm1, qword ptr [rdx]
    vcomisd xmm0, xmm1
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vcomisd_reg_jp
    mov byte ptr [r8+4], 0
vcomisd_reg_jp:
    mov byte ptr [r8+5], 1
    jnp vcomisd_reg_jnp
    mov byte ptr [r8+5], 0
vcomisd_reg_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vcomisd_reg_flags ENDP

vcomisd_mem_flags PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [rcx]
    vcomisd xmm0, qword ptr [rdx]
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vcomisd_mem_jp
    mov byte ptr [r8+4], 0
vcomisd_mem_jp:
    mov byte ptr [r8+5], 1
    jnp vcomisd_mem_jnp
    mov byte ptr [r8+5], 0
vcomisd_mem_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vcomisd_mem_flags ENDP

vucomisd_reg_flags PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [rcx]
    vmovsd xmm1, qword ptr [rdx]
    vucomisd xmm0, xmm1
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vucomisd_reg_jp
    mov byte ptr [r8+4], 0
vucomisd_reg_jp:
    mov byte ptr [r8+5], 1
    jnp vucomisd_reg_jnp
    mov byte ptr [r8+5], 0
vucomisd_reg_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vucomisd_reg_flags ENDP

vucomisd_mem_flags PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [rcx]
    vucomisd xmm0, qword ptr [rdx]
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    sete byte ptr [r8+2]
    setb byte ptr [r8+3]
    mov byte ptr [r8+4], 1
    jp vucomisd_mem_jp
    mov byte ptr [r8+4], 0
vucomisd_mem_jp:
    mov byte ptr [r8+5], 1
    jnp vucomisd_mem_jnp
    mov byte ptr [r8+5], 0
vucomisd_mem_jnp:
    setnp al
    sete ah
    and al, ah
    mov byte ptr [r8+6], al
    xor eax, eax
    vzeroupper
    ret
vucomisd_mem_flags ENDP

setp_reg_bytes PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    mov rax, r8
    vcomiss xmm0, xmm1
    setp al
    setnp ah
    vzeroupper
    ret
setp_reg_bytes ENDP

cmov_reg_outcomes PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    vcomiss xmm0, xmm1
    mov r10, 1122334455667788h
    cmovp r10d, r8d
    mov qword ptr [r9+0], r10
    mov r10, 1122334455667788h
    cmovnp r10d, r8d
    mov qword ptr [r9+8], r10
    mov r10, 1122334455667788h
    cmovp r10, r8
    mov qword ptr [r9+16], r10
    mov r10, 1122334455667788h
    cmovnp r10, r8
    mov qword ptr [r9+24], r10
    mov r10, 1122334455667788h
    cmovp r10w, r8w
    mov qword ptr [r9+32], r10
    mov r10, 1122334455667788h
    cmovnp r10w, r8w
    mov qword ptr [r9+40], r10
    xor eax, eax
    vzeroupper
    ret
cmov_reg_outcomes ENDP

cmovp_mem32 PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rcx+4]
    mov rax, 1122334455667788h
    vcomiss xmm0, xmm1
    cmovp eax, dword ptr [rdx]
    vzeroupper
    ret
cmovp_mem32 ENDP

cmovnp_mem32 PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rcx+4]
    mov rax, 1122334455667788h
    vcomiss xmm0, xmm1
    cmovnp eax, dword ptr [rdx]
    vzeroupper
    ret
cmovnp_mem32 ENDP

join_vcomi_pf PROC FRAME
    .endprolog
    test r8d, r8d
    je join_vcomi_pf_alt
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    vcomiss xmm0, xmm1
    jmp join_vcomi_pf_join
join_vcomi_pf_alt:
    vmovsd xmm0, qword ptr [rcx+8]
    vmovsd xmm1, qword ptr [rdx+8]
    vucomisd xmm0, xmm1
join_vcomi_pf_join:
    mov rax, 1122334455667788h
    mov r10d, 0abcdef01h
    cmovp eax, r10d
    mov qword ptr [r9+0], rax
    setp byte ptr [r9+8]
    mov byte ptr [r9+9], 1
    jp join_vcomi_pf_jp
    mov byte ptr [r9+9], 0
join_vcomi_pf_jp:
    xor eax, eax
    vzeroupper
    ret
join_vcomi_pf ENDP

back_to_back_vcomi PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [rcx]
    vmovss xmm1, dword ptr [rdx]
    vucomiss xmm0, xmm1
    vmovss xmm0, dword ptr [rcx+4]
    vmovss xmm1, dword ptr [rdx+4]
    vcomiss xmm0, xmm1
    setp byte ptr [r8+0]
    setnp byte ptr [r8+1]
    mov rax, 1122334455667788h
    mov r10d, 0abcdef01h
    cmovp eax, r10d
    mov qword ptr [r8+8], rax
    xor eax, eax
    vzeroupper
    ret
back_to_back_vcomi ENDP

END
