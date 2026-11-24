; coverage: source cpu_aot_trap
; Source-owned differential fixture for INT3 (CC) and UD2 (0F 0B) in the shared lowering. Each export takes
; (a, b, c, state) in RCX, RDX, R8 and R9. state[0] is the store before the trap and state[1] the guest work after
; it, which must not run when the trap unwinds to the caller. trap_branch reaches INT3 only when a is nonzero.
.code

trap_int3 PROC FRAME
    .endprolog
    mov qword ptr [r9], 1
    int 3
    mov qword ptr [r9+8], 2
    mov eax, 7
    ret
trap_int3 ENDP

trap_ud2 PROC FRAME
    .endprolog
    mov qword ptr [r9], 1
    ud2
    mov qword ptr [r9+8], 2
    mov eax, 7
    ret
trap_ud2 ENDP

trap_branch PROC FRAME
    .endprolog
    mov qword ptr [r9], 1
    test ecx, ecx
    jz skip
    int 3
skip:
    mov qword ptr [r9+8], 2
    mov eax, 7
    ret
trap_branch ENDP

END
