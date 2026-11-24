; Source-built caller for the acphal.dll HAL, shape-context and heap exports:
;   acphal.dll!ApuCreateHeap
;   acphal.dll!AcpHalCreate
;   acphal.dll!AcpHalAllocateShapeContexts
;   acphal.dll!AcpHalReleaseShapeContexts
; with acphal.dll!ApuAlloc and acphal.dll!ApuFree to observe the heap.
;
; ApuAlloc/ApuFree use the public DirectXTK12 call shape. The other four argument
; lists have no public source and are the fixture contract documented in
; acphal_hal_provider.cpp:
;   HRESULT ApuCreateHeap(UINT32 size)
;   HRESULT AcpHalCreate(void** hal)
;   HRESULT AcpHalAllocateShapeContexts(void* hal, UINT32 count, void** contexts)
;   HRESULT AcpHalReleaseShapeContexts(void* hal, void* contexts)
EXTERN __imp_ApuCreateHeap:QWORD
EXTERN __imp_ApuAlloc:QWORD
EXTERN __imp_ApuFree:QWORD
EXTERN __imp_AcpHalCreate:QWORD
EXTERN __imp_AcpHalAllocateShapeContexts:QWORD
EXTERN __imp_AcpHalReleaseShapeContexts:QWORD
EXTERN __imp_get_acphal_double_release_refusals:QWORD
PUBLIC native_entry

E_INVALIDARG_    EQU 80070057h
E_POINTER_       EQU 80004003h
E_HANDLE_        EQU 80070006h
E_OUTOFMEMORY_   EQU 8007000Eh
ALREADY_EXISTS_  EQU 800700B7h
INVALID_STATE_   EQU 8007139Fh
SLOT_BYTES       EQU 40h

.data
sentinel        dq 0A5A5A5A5A5A5A5A5h
block           dq 0
spare           dq 0
hal             dq 0
hal_second      dq 0
ctx_a           dq 0
ctx_b           dq 0
ctx_c           dq 0

.code
; Returns ZF set when qword [rcx] still holds the sentinel.
still_sentinel PROC
    mov rax, qword ptr [rcx]
    cmp rax, qword ptr [sentinel]
    ret
still_sentinel ENDP

native_entry PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog

    ; Step 1: ApuAlloc before any heap exists is refused and leaves the output.
    mov rax, qword ptr [sentinel]
    mov qword ptr [spare], rax
    lea rcx, spare
    xor edx, edx
    mov r8d, 1000h
    mov r9d, 800h
    call qword ptr [__imp_ApuAlloc]
    cmp eax, INVALID_STATE_
    jne fail_1
    lea rcx, spare
    call still_sentinel
    jne fail_1

    ; Step 2: heap creation refuses size 0, accepts 8 KiB, refuses a second heap.
    xor ecx, ecx
    call qword ptr [__imp_ApuCreateHeap]
    cmp eax, E_INVALIDARG_
    jne fail_2
    mov ecx, 2000h
    call qword ptr [__imp_ApuCreateHeap]
    test eax, eax
    jnz fail_2
    mov ecx, 2000h
    call qword ptr [__imp_ApuCreateHeap]
    cmp eax, ALREADY_EXISTS_
    jne fail_2

    ; Step 3: the heap bounds ApuAlloc. 4 KiB fits; a further 8 KiB does not.
    lea rcx, block
    xor edx, edx
    mov r8d, 1000h
    mov r9d, 800h
    call qword ptr [__imp_ApuAlloc]
    test eax, eax
    jnz fail_3
    mov rax, qword ptr [block]
    test rax, rax
    jz fail_3
    test eax, 7FFh
    jnz fail_3
    mov dword ptr [rax], 0AC9A1001h
    lea rcx, spare
    xor edx, edx
    mov r8d, 2000h
    mov r9d, 800h
    call qword ptr [__imp_ApuAlloc]
    cmp eax, E_OUTOFMEMORY_
    jne fail_3
    lea rcx, spare
    call still_sentinel
    jne fail_3
    mov rax, qword ptr [block]
    cmp dword ptr [rax], 0AC9A1001h
    jne fail_3
    mov rcx, qword ptr [block]
    call qword ptr [__imp_ApuFree]

    ; Step 4: shape-context allocation with a null handle before AcpHalCreate is refused.
    ; The checker replays the real handle before create to reach the state check.
    mov rax, qword ptr [sentinel]
    mov qword ptr [ctx_a], rax
    xor ecx, ecx
    mov edx, 4
    lea r8, ctx_a
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    cmp eax, E_HANDLE_
    jne fail_4
    lea rcx, ctx_a
    call still_sentinel
    jne fail_4

    ; Step 5: AcpHalCreate refuses a null output, creates one HAL, refuses a second.
    xor ecx, ecx
    call qword ptr [__imp_AcpHalCreate]
    cmp eax, E_POINTER_
    jne fail_5
    lea rcx, hal
    call qword ptr [__imp_AcpHalCreate]
    test eax, eax
    jnz fail_5
    cmp qword ptr [hal], 0
    je fail_5
    mov rax, qword ptr [sentinel]
    mov qword ptr [hal_second], rax
    lea rcx, hal_second
    call qword ptr [__imp_AcpHalCreate]
    cmp eax, ALREADY_EXISTS_
    jne fail_5
    lea rcx, hal_second
    call still_sentinel
    jne fail_5

    ; Step 6: invalid counts and a null output are refused; the output is untouched.
    mov rcx, qword ptr [hal]
    xor edx, edx
    lea r8, ctx_a
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    cmp eax, E_INVALIDARG_
    jne fail_6
    mov rcx, qword ptr [hal]
    mov edx, 17
    lea r8, ctx_a
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    cmp eax, E_INVALIDARG_
    jne fail_6
    mov rcx, qword ptr [hal]
    mov edx, 4
    xor r8d, r8d
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    cmp eax, E_POINTER_
    jne fail_6
    lea rcx, ctx_a
    call still_sentinel
    jne fail_6

    ; Step 7: two allocations of 4 contexts are cleared, aligned and disjoint.
    mov rcx, qword ptr [hal]
    mov edx, 4
    lea r8, ctx_a
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    test eax, eax
    jnz fail_7
    mov rax, qword ptr [ctx_a]
    test rax, rax
    jz fail_7
    test al, 3Fh
    jnz fail_7
    mov ecx, (4 * SLOT_BYTES) / 8
zero_a:
    cmp qword ptr [rax], 0
    jne fail_7
    add rax, 8
    dec ecx
    jnz zero_a
    mov rax, qword ptr [ctx_a]
    mov rdx, 5AC0FFEE00000001h
    mov qword ptr [rax], rdx
    mov rdx, 5AC0FFEE000000FFh
    mov qword ptr [rax + 4 * SLOT_BYTES - 8], rdx

    mov rcx, qword ptr [hal]
    mov edx, 4
    lea r8, ctx_b
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    test eax, eax
    jnz fail_7
    mov rax, qword ptr [ctx_b]
    test rax, rax
    jz fail_7
    mov ecx, (4 * SLOT_BYTES) / 8
zero_b:
    cmp qword ptr [rax], 0
    jne fail_7
    add rax, 8
    dec ecx
    jnz zero_b
    ; Disjoint: |ctx_b - ctx_a| >= 4 slots.
    mov rax, qword ptr [ctx_b]
    sub rax, qword ptr [ctx_a]
    jns distance_known
    neg rax
distance_known:
    cmp rax, 4 * SLOT_BYTES
    jb fail_7
    ; The first allocation kept the caller's writes.
    mov rax, qword ptr [ctx_a]
    mov rdx, 5AC0FFEE00000001h
    cmp qword ptr [rax], rdx
    jne fail_7
    mov rdx, 5AC0FFEE000000FFh
    cmp qword ptr [rax + 4 * SLOT_BYTES - 8], rdx
    jne fail_7

    ; Step 8: release, double release, release with a wrong handle, release.
    mov rcx, qword ptr [hal]
    mov rdx, qword ptr [ctx_a]
    call qword ptr [__imp_AcpHalReleaseShapeContexts]
    test eax, eax
    jnz fail_8
    mov rcx, qword ptr [hal]
    mov rdx, qword ptr [ctx_a]
    call qword ptr [__imp_AcpHalReleaseShapeContexts]
    cmp eax, E_INVALIDARG_
    jne fail_8
    lea rcx, hal_second
    mov rdx, qword ptr [ctx_b]
    call qword ptr [__imp_AcpHalReleaseShapeContexts]
    cmp eax, E_HANDLE_
    jne fail_8
    mov rcx, qword ptr [hal]
    mov rdx, qword ptr [ctx_b]
    call qword ptr [__imp_AcpHalReleaseShapeContexts]
    test eax, eax
    jnz fail_8

    ; Step 9: released slots are reusable; 8 contexts come back cleared.
    mov rcx, qword ptr [hal]
    mov edx, 8
    lea r8, ctx_c
    call qword ptr [__imp_AcpHalAllocateShapeContexts]
    test eax, eax
    jnz fail_9
    mov rax, qword ptr [ctx_c]
    cmp rax, qword ptr [ctx_a]
    jne fail_9
    mov ecx, (8 * SLOT_BYTES) / 8
zero_c:
    cmp qword ptr [rax], 0
    jne fail_9
    add rax, 8
    dec ecx
    jnz zero_c
    mov rcx, qword ptr [hal]
    mov rdx, qword ptr [ctx_c]
    call qword ptr [__imp_AcpHalReleaseShapeContexts]
    test eax, eax
    jnz fail_9

    ; Step 10: the provider counted exactly one double release.
    call qword ptr [__imp_get_acphal_double_release_refusals]
    cmp eax, 1
    jne fail_10

    mov eax, 8ACh
    jmp done

fail_1:
    mov eax, 0F01h
    jmp done
fail_2:
    mov eax, 0F02h
    jmp done
fail_3:
    mov eax, 0F03h
    jmp done
fail_4:
    mov eax, 0F04h
    jmp done
fail_5:
    mov eax, 0F05h
    jmp done
fail_6:
    mov eax, 0F06h
    jmp done
fail_7:
    mov eax, 0F07h
    jmp done
fail_8:
    mov eax, 0F08h
    jmp done
fail_9:
    mov eax, 0F09h
    jmp done
fail_10:
    mov eax, 0F0Ah

done:
    add rsp, 28h
    ret
native_entry ENDP
END
