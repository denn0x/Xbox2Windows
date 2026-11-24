; coverage: source cpu_aot_parity
; Source-owned differential fixture for PF readers after the VEX scalar compares in the shared lowering. Each
; export takes (a, b, c, state) in RCX, RDX, R8 and R9. state (bytes): 00 and 08 the compared operands (written by
; the test at run time), 10 SETNP, 11 SETP, 12 SETE, 13 SETB, 18 CMOVP EAX (RAX all ones before, ECX = 7), 20
; CMOVNP EDX from memory (RDX all ones before, [state+28] = 9), 28 the CMOVNP memory source, 30 the JP marker
; (1 taken, 2 not taken). pf_fucomi runs the same readers after FUCOMIP of the two doubles in the x87 profile.
; pf_fault and pf_fault_p read their CMOVNP and CMOVP source through RCX, which the test points at an inaccessible
; page. pf_lahf returns SETP of the low byte of a + b, a register ADD whose flags a LAHF reads. The exports after
; it exist only to be refused (tests/cpu/parity.py).
.code

PF_READERS MACRO
    LOCAL not_taken, done
    setnp byte ptr [r9+10h]
    setp byte ptr [r9+11h]
    sete byte ptr [r9+12h]
    setb byte ptr [r9+13h]
    mov rax, -1
    mov ecx, 7
    cmovp eax, ecx
    mov qword ptr [r9+18h], rax
    mov rdx, -1
    cmovnp edx, dword ptr [r9+28h]
    mov qword ptr [r9+20h], rdx
    jp not_taken
    mov qword ptr [r9+30h], 2
    jmp done
not_taken:
    mov qword ptr [r9+30h], 1
done:
ENDM

pf_ucomiss PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vmovss xmm1, dword ptr [r9+8]
    vucomiss xmm0, xmm1
    PF_READERS
    ret
pf_ucomiss ENDP

pf_comiss PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vmovss xmm1, dword ptr [r9+8]
    vcomiss xmm0, xmm1
    PF_READERS
    ret
pf_comiss ENDP

pf_ucomisd PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [r9]
    vmovsd xmm1, qword ptr [r9+8]
    vucomisd xmm0, xmm1
    PF_READERS
    ret
pf_ucomisd ENDP

pf_comisd PROC FRAME
    .endprolog
    vmovsd xmm0, qword ptr [r9]
    vmovsd xmm1, qword ptr [r9+8]
    vcomisd xmm0, xmm1
    PF_READERS
    ret
pf_comisd ENDP

; The x87 profile decides PF after FUCOMIP (x87_lower.check_flags).
pf_fucomi PROC FRAME
    .endprolog
    fld qword ptr [r9+8]
    fld qword ptr [r9]
    fucomip st(0), st(1)
    fstp st(0)
    PF_READERS
    ret
pf_fucomi ENDP

; The float-equality idiom: PF clear and ZF set.
pf_equal PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vmovss xmm1, dword ptr [r9+8]
    vucomiss xmm0, xmm1
    setnp al
    sete cl
    and al, cl
    movzx eax, al
    ret
pf_equal ENDP

; The CMOVNP memory source is read whatever PF says: an inaccessible source faults either way.
pf_fault PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vmovss xmm1, dword ptr [r9+8]
    vucomiss xmm0, xmm1
    mov edx, 5
    cmovnp edx, dword ptr [rcx]
    mov eax, edx
    ret
pf_fault ENDP

pf_fault_p PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vmovss xmm1, dword ptr [r9+8]
    vucomiss xmm0, xmm1
    mov edx, 5
    cmovp edx, dword ptr [rcx]
    mov eax, edx
    ret
pf_fault_p ENDP

; A register ADD that a LAHF reads assigns PF (lahf_sources).
pf_lahf PROC FRAME
    .endprolog
    add ecx, edx
    lahf
    setp al
    movzx eax, al
    ret
pf_lahf ENDP

; Refused: PF of ADD is not modeled.
pf_after_add PROC FRAME
    .endprolog
    add ecx, edx
    setnp al
    ret
pf_after_add ENDP

; Refused: PF is known on one incoming path only.
pf_join PROC FRAME
    .endprolog
    test ecx, ecx
    jz joined
    vmovss xmm0, dword ptr [r9]
    vucomiss xmm0, xmm0
joined:
    setnp al
    ret
pf_join ENDP

; Refused: IMUL leaves PF undefined after the compare.
pf_undefined PROC FRAME
    .endprolog
    vmovss xmm0, dword ptr [r9]
    vucomiss xmm0, xmm0
    imul ecx, edx
    cmovp eax, ecx
    ret
pf_undefined ENDP

END
