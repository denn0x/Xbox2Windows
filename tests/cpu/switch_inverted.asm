; coverage: source cpu_aot_switch_inverted
; Windows x64 jump-table dispatch behind an inverted unsigned bound check: the check branches to
; the dispatch block when the index is in range (JB or JBE) and a JMP to the default follows it.
; ECX/RCX = selector, EDX = operand, R8 = address of one writable DWORD; EAX is returned whole.
EXTERN __ImageBase:BYTE
.code

; MSVC layout, JBE: selectors 0 to 4 dispatch (five entries) through image-relative entries. The
; 32-bit compare ignores the upper half of RCX. The default stores the complement.
inv_rva_jbe PROC FRAME
    .endprolog
    mov eax, ecx
    cmp ecx, 4
    jbe inv_rva_in_range
    jmp inv_rva_default
inv_rva_in_range:
    movsxd r9, ecx
    lea r10, __ImageBase
    mov r9d, DWORD PTR [(IMAGEREL inv_rva_table) + r10 + r9*4]
    add r9, r10
    jmp r9
inv_rva_case0:
    add edx, 11
    jmp inv_rva_store
inv_rva_case1:
    xor edx, 5A5Ah
    jmp inv_rva_store
inv_rva_case2:
    imul edx, edx, 3
    jmp inv_rva_store
inv_rva_case3:
    sub edx, 2
    jmp inv_rva_store
inv_rva_case4:
    shl edx, 4
    jmp inv_rva_store
inv_rva_default:
    not edx
inv_rva_store:
    mov DWORD PTR [r8], edx
    add eax, edx
    ret
    ALIGN 4
inv_rva_table LABEL DWORD
    DD IMAGEREL inv_rva_case0
IFDEF SWAP_TABLE
    DD IMAGEREL inv_rva_case4
ELSE
    DD IMAGEREL inv_rva_case1
ENDIF
    DD IMAGEREL inv_rva_case2, IMAGEREL inv_rva_case3
IFDEF SWAP_TABLE
    DD IMAGEREL inv_rva_case1
ELSE
    DD IMAGEREL inv_rva_case4
ENDIF
inv_rva_jbe ENDP

; LLVM layout, JB: the compare is 64 bits wide, so any selector with an upper bit set is the
; default. Selectors 0 to 5 dispatch (six entries, two of them to one target). The default
; returns all ones and leaves the DWORD at R8 untouched.
inv_rel_jb PROC FRAME
    .endprolog
    mov eax, ecx
    cmp rcx, 6
    jb inv_rel_in_range
    jmp inv_rel_default
inv_rel_in_range:
    lea r9, inv_rel_table
    movsxd rcx, DWORD PTR [r9+rcx*4]
    add rcx, r9
    jmp rcx
inv_rel_case0:
    lea edx, [rdx+rdx*4]
    jmp inv_rel_store
inv_rel_case1:
    add edx, -3
    jmp inv_rel_store
inv_rel_case24:
    xor edx, 0C3C3h
    jmp inv_rel_store
inv_rel_case3:
    shr edx, 3
    jmp inv_rel_store
inv_rel_case5:
    neg edx
inv_rel_store:
    mov DWORD PTR [r8], edx
    add eax, edx
    ret
inv_rel_default:
    mov eax, -1
    ret
    ALIGN 4
inv_rel_table:
    DD inv_rel_case0 - inv_rel_table
IFDEF SWAP_TABLE
    DD inv_rel_case5 - inv_rel_table
ELSE
    DD inv_rel_case1 - inv_rel_table
ENDIF
    DD inv_rel_case24 - inv_rel_table, inv_rel_case3 - inv_rel_table, inv_rel_case24 - inv_rel_table
IFDEF SWAP_TABLE
    DD inv_rel_case1 - inv_rel_table
ELSE
    DD inv_rel_case5 - inv_rel_table
ENDIF
inv_rel_jb ENDP

; LLVM layout, JBE, with the default placed in front of the check and a zero-extending copy of
; the compared value inside the dispatch block: selectors 0 to 6 dispatch (seven entries, two of
; them to one target) and only the low 32 bits of RCX are compared and used.
inv_rel_window_jbe PROC FRAME
    .endprolog
    jmp inv_win_guard
inv_win_default:
    add edx, 77
    jmp inv_win_store
inv_win_guard:
    mov eax, ecx
    cmp ecx, 6
    jbe inv_win_in_range
    jmp inv_win_default
inv_win_in_range:
    mov ecx, eax
    lea r9, inv_win_table
    movsxd rcx, DWORD PTR [r9+rcx*4]
    add rcx, r9
    jmp rcx
inv_win_case0:
    add edx, 1234h
    jmp inv_win_store
inv_win_case16:
    sub edx, 10h
    jmp inv_win_store
inv_win_case2:
    xor edx, 0FF00FFh
    jmp inv_win_store
inv_win_case3:
    imul edx, edx, 7
    jmp inv_win_store
inv_win_case4:
    shl edx, 1
    jmp inv_win_store
inv_win_case5:
    sar edx, 2
inv_win_store:
    mov DWORD PTR [r8], edx
    add eax, edx
    ret
    ALIGN 4
inv_win_table:
IFDEF SWAP_TABLE
    DD inv_win_case5 - inv_win_table
ELSE
    DD inv_win_case0 - inv_win_table
ENDIF
    DD inv_win_case16 - inv_win_table, inv_win_case2 - inv_win_table, inv_win_case3 - inv_win_table
    DD inv_win_case4 - inv_win_table
IFDEF SWAP_TABLE
    DD inv_win_case0 - inv_win_table
ELSE
    DD inv_win_case5 - inv_win_table
ENDIF
    DD inv_win_case16 - inv_win_table
inv_rel_window_jbe ENDP
END
