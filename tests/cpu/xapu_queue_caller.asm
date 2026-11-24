; Source-built caller for the xapu.dll command-queue imports.
; No public source gives the XApu signatures, so this caller uses the fixture convention
; documented in xapu_queue_provider.cpp:
;   HRESULT XApuConnect(UINT64* client)
;   HRESULT XApuEnqueueCommand(UINT64 client, UINT64 id, UINT32 payload)
;   HRESULT XApuGetResultCount(UINT64 client, UINT32* count)
;   HRESULT XApuDequeueResult(UINT64 client, result* {UINT64 id; UINT32 value; HRESULT status})
;   HRESULT XApuDisconnect(UINT64 client)
; Result values (payload + 1000h) and error codes are the test provider's policy.
EXTERN __imp_XApuConnect:QWORD
EXTERN __imp_XApuEnqueueCommand:QWORD
EXTERN __imp_XApuGetResultCount:QWORD
EXTERN __imp_XApuDequeueResult:QWORD
EXTERN __imp_XApuDisconnect:QWORD
EXTERN __imp_get_xapu_connect_count:QWORD
EXTERN __imp_get_xapu_disconnect_count:QWORD
EXTERN __imp_get_xapu_live_client_count:QWORD
EXTERN __imp_get_xapu_enqueued_count:QWORD
EXTERN __imp_get_xapu_dequeued_count:QWORD
EXTERN __imp_get_xapu_refused_count:QWORD
PUBLIC native_entry

SENTINEL equ 0A9A9A9A9A9A9A9A9h
SENTINEL32 equ 0A9A9A9A9h
E_HANDLE equ 080070006h
NO_MORE_ITEMS equ 080070103h
UNKNOWN_OFFSET equ 100h

.data
client          dq 0
result_id       dq 0
result_value    dd 0
result_status   dd 0
result_count    dd 0
                dd 0
expected_ids    dq 101h, 102h, 103h
payloads        dd 11h, 22h, 33h

.code
; Fill the 16-byte result and the count with the sentinel.
fill_sentinel PROC
    mov rax, SENTINEL
    mov qword ptr [result_id], rax
    mov qword ptr [result_value], rax
    mov dword ptr [result_count], SENTINEL32
    ret
fill_sentinel ENDP

; ZF set when the result and the count still hold the sentinel.
check_sentinel PROC
    mov rax, SENTINEL
    cmp qword ptr [result_id], rax
    jne done_check
    cmp qword ptr [result_value], rax
    jne done_check
    cmp dword ptr [result_count], SENTINEL32
done_check:
    ret
check_sentinel ENDP

native_entry PROC FRAME
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    sub rsp, 28h
    .allocstack 28h
    .endprolog

    ; Step 1: connect returns S_OK and a nonzero handle.
    mov rax, SENTINEL
    mov qword ptr [client], rax
    lea rcx, client
    call qword ptr [__imp_XApuConnect]
    test eax, eax
    jnz fail_1
    mov rax, qword ptr [client]
    test rax, rax
    jz fail_1
    mov rdx, SENTINEL
    cmp rax, rdx
    je fail_1

    ; Step 2: enqueue three commands, then the queue holds three results.
    xor ebx, ebx
enqueue_loop:
    mov rcx, qword ptr [client]
    lea rax, expected_ids
    mov rdx, qword ptr [rax+rbx*8]
    lea rax, payloads
    mov r8d, dword ptr [rax+rbx*4]
    call qword ptr [__imp_XApuEnqueueCommand]
    test eax, eax
    jnz fail_2
    inc ebx
    cmp ebx, 3
    jb enqueue_loop
    mov rcx, qword ptr [client]
    lea rdx, result_count
    call qword ptr [__imp_XApuGetResultCount]
    test eax, eax
    jnz fail_2
    cmp dword ptr [result_count], 3
    jne fail_2

    ; Step 3: results come back in submission order, each matching its command.
    xor ebx, ebx
dequeue_loop:
    mov rcx, qword ptr [client]
    lea rdx, result_id
    call qword ptr [__imp_XApuDequeueResult]
    test eax, eax
    jnz fail_3
    lea rax, expected_ids
    mov rdx, qword ptr [rax+rbx*8]
    cmp qword ptr [result_id], rdx
    jne fail_3
    lea rax, payloads
    mov esi, dword ptr [rax+rbx*4]
    add esi, 1000h
    cmp dword ptr [result_value], esi
    jne fail_3
    cmp dword ptr [result_status], 0
    jne fail_3
    ; The count drops by one per dequeue.
    mov rcx, qword ptr [client]
    lea rdx, result_count
    call qword ptr [__imp_XApuGetResultCount]
    test eax, eax
    jnz fail_3
    mov eax, 2
    sub eax, ebx
    cmp dword ptr [result_count], eax
    jne fail_3
    inc ebx
    cmp ebx, 3
    jb dequeue_loop

    ; Step 4: refusal. A dequeue from the empty queue fails and leaves the output intact.
    call fill_sentinel
    mov rcx, qword ptr [client]
    lea rdx, result_id
    call qword ptr [__imp_XApuDequeueResult]
    cmp eax, NO_MORE_ITEMS
    jne fail_4
    call check_sentinel
    jne fail_4

    ; Step 5: refusal. An unknown handle and a zero handle fail with E_HANDLE, leave the
    ; outputs intact, and add nothing to the live client's queue.
    mov rcx, qword ptr [client]
    add rcx, UNKNOWN_OFFSET
    mov edx, 1EEh
    mov r8d, 44h
    call qword ptr [__imp_XApuEnqueueCommand]
    cmp eax, E_HANDLE
    jne fail_5
    call fill_sentinel
    mov rcx, qword ptr [client]
    add rcx, UNKNOWN_OFFSET
    lea rdx, result_id
    call qword ptr [__imp_XApuDequeueResult]
    cmp eax, E_HANDLE
    jne fail_5
    xor ecx, ecx
    lea rdx, result_count
    call qword ptr [__imp_XApuGetResultCount]
    cmp eax, E_HANDLE
    jne fail_5
    call check_sentinel
    jne fail_5
    mov rcx, qword ptr [client]
    lea rdx, result_count
    call qword ptr [__imp_XApuGetResultCount]
    test eax, eax
    jnz fail_5
    cmp dword ptr [result_count], 0
    jne fail_5

    ; Step 6: disconnect, then the handle is refused for a second disconnect and an enqueue.
    mov rcx, qword ptr [client]
    call qword ptr [__imp_XApuDisconnect]
    test eax, eax
    jnz fail_6
    mov rcx, qword ptr [client]
    call qword ptr [__imp_XApuDisconnect]
    cmp eax, E_HANDLE
    jne fail_6
    mov rcx, qword ptr [client]
    mov edx, 104h
    mov r8d, 55h
    call qword ptr [__imp_XApuEnqueueCommand]
    cmp eax, E_HANDLE
    jne fail_6

    ; Step 7: this process's provider saw one connect, one disconnect, no live client,
    ; three enqueued and three dequeued results, and six refusals.
    call qword ptr [__imp_get_xapu_connect_count]
    cmp eax, 1
    jne fail_7
    call qword ptr [__imp_get_xapu_disconnect_count]
    cmp eax, 1
    jne fail_7
    call qword ptr [__imp_get_xapu_live_client_count]
    test eax, eax
    jnz fail_7
    call qword ptr [__imp_get_xapu_enqueued_count]
    cmp eax, 3
    jne fail_7
    call qword ptr [__imp_get_xapu_dequeued_count]
    cmp eax, 3
    jne fail_7
    call qword ptr [__imp_get_xapu_refused_count]
    cmp eax, 6
    jne fail_7

    mov eax, 8B0h
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

done:
    add rsp, 28h
    pop rsi
    pop rbx
    ret
native_entry ENDP
END
