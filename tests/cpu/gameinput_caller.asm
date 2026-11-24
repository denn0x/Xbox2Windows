; Source-built caller for GameInput.dll ordinal 1 and the v0 objects it returns.
;
; The call shapes reproduce a statically read console caller (#1056): ordinal 1 called with one
; argument; IGameInput slots 4, 5, 6, 9, 13, 16 (vtable +20h, +28h, +30h, +48h, +68h, +80h);
; IGameInputReading slots 14, 15, 22 (+70h, +78h, +0B0h); IGameInputDevice slots 3, 10
; (+18h, +50h). Method names and structures follow the public v0 header um/GameInput.h.
; The keyboard registration is made while holding a recursive lock that its device callback
; takes again, as in that caller. The callback takes the lock with a bounded spin, so a
; provider that calls back on another thread and waits is reported instead of hanging.
; The runtime behaviour checked here is the fixture policy of gameinput_provider.cpp.
EXTERN __imp_GameInputCreate:QWORD
EXTERN __imp_get_gameinput_outstanding_refs:QWORD
PUBLIC native_entry

E_POINTER_           EQU 80004003h
E_INVALIDARG_        EQU 80070057h
DEVICE_NOT_FOUND_    EQU 838A0002h
READING_NOT_FOUND_   EQU 838A0003h
KIND_KEYBOARD        EQU 10h
KIND_MOUSE           EQU 20h
KIND_GAMEPAD         EQU 40000h
STATUS_CONNECTED     EQU 1
BLOCKING             EQU 2
SPIN_LIMIT           EQU 1000000h

.data
sentinel        dq 0A5A5A5A5A5A5A5A5h
gi              dq 0
gi2             dq 0
main_tid        dq 0
lock_owner      dq 0
lock_count      dd 0
in_register     dd 0
saved_hr        dd 0
token_k         dq 0
token_g         dq 0
token_x         dq 0
kb_device       dq 0
pad_device      dq 0
reading_a       dq 0
reading_b       dq 0
found           dq 0
kb_calls        dd 0
kb_during       dd 0
kb_other        dd 0
kb_lock_fail    dd 0
kb_bad          dd 0
pad_calls       dd 0
pad_during      dd 0
pad_other       dd 0
pad_bad         dd 0
pad_ctx         dq 0
ALIGN 8
state_a         db 28 dup(0)
ALIGN 8
state_b         db 28 dup(0)
ALIGN 8
keys            db 16 * 12 dup(0)
; Expected GameInputGamepadState: A + RB, triggers 0.25 / 1.0, sticks (-0.5, 0.75), (0, -1.0).
exp_pad_cur     dd 0804h, 3E800000h, 3F800000h, 0BF000000h, 3F400000h, 0, 0BF800000h
; Expected previous reading: A only.
exp_pad_prev    dd 4, 0, 0, 0, 0, 0, 0
; Expected GameInputKeyState fields: (scanCode, codePoint) as one qword, (virtualKey, isDeadKey).
exp_key_0       dq 000000610000001Eh
                dw 0041h
exp_key_1       dq 000000000000E04Bh
                dw 0025h
exp_key_2       dq 0000000000000036h
                dw 00A1h
rumble          dd 3F000000h, 3F000000h, 0, 0
kb_id           db 10h, 11h, 12h, 13h, 14h, 15h, 16h, 17h, 18h, 19h, 1Ah, 1Bh, 1Ch, 1Dh, 1Eh, 1Fh
                db 20h, 21h, 22h, 23h, 24h, 25h, 26h, 27h, 28h, 29h, 2Ah, 2Bh, 2Ch, 2Dh, 2Eh, 2Fh
pad_id          db 40h, 41h, 42h, 43h, 44h, 45h, 46h, 47h, 48h, 49h, 4Ah, 4Bh, 4Ch, 4Dh, 4Eh, 4Fh
                db 50h, 51h, 52h, 53h, 54h, 55h, 56h, 57h, 58h, 59h, 5Ah, 5Bh, 5Ch, 5Dh, 5Eh, 5Fh
zero_id         db 32 dup(0)

.code
; ZF set when the 32 bytes at [rcx] equal the 32 bytes at [rdx].
same32 PROC
    mov rax, qword ptr [rcx]
    cmp rax, qword ptr [rdx]
    jne same32_done
    mov rax, qword ptr [rcx + 8]
    cmp rax, qword ptr [rdx + 8]
    jne same32_done
    mov rax, qword ptr [rcx + 16]
    cmp rax, qword ptr [rdx + 16]
    jne same32_done
    mov rax, qword ptr [rcx + 24]
    cmp rax, qword ptr [rdx + 24]
same32_done:
    ret
same32 ENDP

; ZF set when the 28 bytes at [rcx] equal the 28 bytes at [rdx].
same28 PROC
    mov rax, qword ptr [rcx]
    cmp rax, qword ptr [rdx]
    jne same28_done
    mov rax, qword ptr [rcx + 8]
    cmp rax, qword ptr [rdx + 8]
    jne same28_done
    mov rax, qword ptr [rcx + 16]
    cmp rax, qword ptr [rdx + 16]
    jne same28_done
    mov eax, dword ptr [rcx + 24]
    cmp eax, dword ptr [rdx + 24]
same28_done:
    ret
same28 ENDP

; ZF set when the key state at [rcx] has the fields at [rdx] (qword, then word).
same_key PROC
    mov rax, qword ptr [rcx]
    cmp rax, qword ptr [rdx]
    jne same_key_done
    mov ax, word ptr [rcx + 8]
    cmp ax, word ptr [rdx + 8]
same_key_done:
    ret
same_key ENDP

; Recursive lock owned by a thread id from the TEB. Returns eax = 1 when held, 0 when the
; bounded spin gave up. Clobbers rax, rdx, r8.
lock_acquire PROC
    mov rdx, qword ptr gs:[48h]
    cmp qword ptr [lock_owner], rdx
    jne lock_try
    inc dword ptr [lock_count]
    mov eax, 1
    ret
lock_try:
    mov r8d, SPIN_LIMIT
lock_spin:
    xor eax, eax
    lock cmpxchg qword ptr [lock_owner], rdx
    je lock_got
    pause
    dec r8d
    jnz lock_spin
    xor eax, eax
    ret
lock_got:
    mov dword ptr [lock_count], 1
    mov eax, 1
    ret
lock_acquire ENDP

lock_release PROC
    dec dword ptr [lock_count]
    jnz lock_release_done
    mov qword ptr [lock_owner], 0
lock_release_done:
    ret
lock_release ENDP

; Device callback for keyboards: (token, context, device, timestamp, currentStatus, previousStatus).
kb_callback PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rbx, r8
    call lock_acquire
    test eax, eax
    jz kb_nolock
    inc dword ptr [kb_calls]
    mov rax, qword ptr gs:[48h]
    cmp rax, qword ptr [main_tid]
    je kb_thread_ok
    inc dword ptr [kb_other]
kb_thread_ok:
    cmp dword ptr [in_register], 0
    je kb_not_during
    inc dword ptr [kb_during]
kb_not_during:
    cmp dword ptr [rsp + 50h], STATUS_CONNECTED
    je kb_status_ok
    inc dword ptr [kb_bad]
kb_status_ok:
    test rbx, rbx
    jz kb_bad_id
    mov rcx, rbx
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 18h]          ; GetDeviceInfo
    test rax, rax
    jz kb_bad_id
    lea rcx, [rax + 20h]                ; deviceId
    lea rdx, kb_id
    call same32
    jne kb_bad_id
    cmp qword ptr [kb_device], 0
    jne kb_unlock
    mov rcx, rbx
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 8]            ; AddRef: kept like an entry in the caller's list
    mov qword ptr [kb_device], rbx
    jmp kb_unlock
kb_bad_id:
    inc dword ptr [kb_bad]
kb_unlock:
    call lock_release
    jmp kb_done
kb_nolock:
    inc dword ptr [kb_lock_fail]
kb_done:
    add rsp, 20h
    pop rbx
    ret
kb_callback ENDP

; Device callback for gamepads; context must be the address of pad_ctx.
pad_callback PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rbx, r8
    inc dword ptr [pad_calls]
    lea rax, pad_ctx
    cmp rdx, rax
    je pad_ctx_ok
    inc dword ptr [pad_bad]
pad_ctx_ok:
    mov rax, qword ptr gs:[48h]
    cmp rax, qword ptr [main_tid]
    je pad_thread_ok
    inc dword ptr [pad_other]
pad_thread_ok:
    cmp dword ptr [in_register], 0
    je pad_not_during
    inc dword ptr [pad_during]
pad_not_during:
    cmp dword ptr [rsp + 50h], STATUS_CONNECTED
    je pad_status_ok
    inc dword ptr [pad_bad]
pad_status_ok:
    test rbx, rbx
    jz pad_bad_id
    mov rcx, rbx
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 18h]          ; GetDeviceInfo
    test rax, rax
    jz pad_bad_id
    lea rcx, [rax + 20h]
    lea rdx, pad_id
    call same32
    jne pad_bad_id
    cmp qword ptr [pad_device], 0
    jne pad_done
    mov rcx, rbx
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 8]            ; AddRef: kept like the caller's device record
    mov qword ptr [pad_device], rbx
    jmp pad_done
pad_bad_id:
    inc dword ptr [pad_bad]
pad_done:
    add rsp, 20h
    pop rbx
    ret
pad_callback ENDP

; Releases the object in qword [rcx] if any and clears the slot.
release_slot PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rbx, rcx
    mov rcx, qword ptr [rbx]
    test rcx, rcx
    jz release_slot_done
    mov qword ptr [rbx], 0
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 10h]          ; Release
release_slot_done:
    add rsp, 20h
    pop rbx
    ret
release_slot ENDP

native_entry PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 40h
    .allocstack 40h
    .endprolog

    ; Step 1: ordinal 1 with one argument. NULL is refused; a second call returns the same object.
    xor ecx, ecx
    call qword ptr [__imp_GameInputCreate]
    cmp eax, E_POINTER_
    jne fail_1
    lea rcx, gi
    call qword ptr [__imp_GameInputCreate]
    test eax, eax
    jnz fail_1
    mov rbx, qword ptr [gi]
    test rbx, rbx
    jz fail_1
    lea rcx, gi2
    call qword ptr [__imp_GameInputCreate]
    test eax, eax
    jnz fail_1
    cmp qword ptr [gi2], rbx
    jne fail_1
    lea rcx, gi2
    call release_slot

    ; Step 2: keyboard callback, blocking enumeration, registered while holding the recursive
    ; lock that the callback takes again. It must run during the call, on this thread.
    mov rax, qword ptr gs:[48h]
    mov qword ptr [main_tid], rax
    call lock_acquire
    test eax, eax
    jz fail_2
    mov dword ptr [in_register], 1
    mov rcx, rbx
    xor edx, edx
    mov r8d, KIND_KEYBOARD
    mov r9d, STATUS_CONNECTED
    mov dword ptr [rsp + 20h], BLOCKING
    mov qword ptr [rsp + 28h], 0
    lea rax, kb_callback
    mov qword ptr [rsp + 30h], rax
    lea rax, token_k
    mov qword ptr [rsp + 38h], rax
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 48h]          ; RegisterDeviceCallback
    mov dword ptr [in_register], 0
    mov dword ptr [saved_hr], eax
    call lock_release
    cmp dword ptr [saved_hr], 0
    jne fail_2
    cmp dword ptr [kb_lock_fail], 0
    jne fail_2
    cmp dword ptr [kb_other], 0
    jne fail_2
    cmp dword ptr [kb_calls], 1
    jne fail_2
    cmp dword ptr [kb_during], 1
    jne fail_2
    cmp dword ptr [kb_bad], 0
    jne fail_2
    cmp qword ptr [token_k], 0
    je fail_2
    cmp qword ptr [kb_device], 0
    je fail_2
    cmp qword ptr [lock_owner], 0
    jne fail_2

    ; Step 3: gamepad callback without a caller lock, with a context pointer.
    mov dword ptr [in_register], 1
    mov rcx, rbx
    xor edx, edx
    mov r8d, KIND_GAMEPAD
    mov r9d, STATUS_CONNECTED
    mov dword ptr [rsp + 20h], BLOCKING
    lea rax, pad_ctx
    mov qword ptr [rsp + 28h], rax
    lea rax, pad_callback
    mov qword ptr [rsp + 30h], rax
    lea rax, token_g
    mov qword ptr [rsp + 38h], rax
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 48h]
    mov dword ptr [in_register], 0
    test eax, eax
    jnz fail_3
    cmp dword ptr [pad_calls], 1
    jne fail_3
    cmp dword ptr [pad_during], 1
    jne fail_3
    cmp dword ptr [pad_other], 0
    jne fail_3
    cmp dword ptr [pad_bad], 0
    jne fail_3
    cmp qword ptr [token_g], 0
    je fail_3
    cmp qword ptr [pad_device], 0
    je fail_3

    ; Step 4: a kind the caller never registers (mouse) is refused with the token untouched.
    mov rax, qword ptr [sentinel]
    mov qword ptr [token_x], rax
    mov rcx, rbx
    xor edx, edx
    mov r8d, KIND_MOUSE
    mov r9d, STATUS_CONNECTED
    mov dword ptr [rsp + 20h], BLOCKING
    mov qword ptr [rsp + 28h], 0
    lea rax, kb_callback
    mov qword ptr [rsp + 30h], rax
    lea rax, token_x
    mov qword ptr [rsp + 38h], rax
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 48h]
    cmp eax, E_INVALIDARG_
    jne fail_4
    mov rax, qword ptr [token_x]
    cmp rax, qword ptr [sentinel]
    jne fail_4
    cmp dword ptr [kb_calls], 1
    jne fail_4

    ; Step 5: gamepad current reading into 28 zeroed bytes, then the reading before it.
    mov rcx, rbx
    mov edx, KIND_GAMEPAD
    mov r8, qword ptr [pad_device]
    lea r9, reading_a
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 20h]          ; GetCurrentReading
    test eax, eax
    jnz fail_5
    mov rcx, qword ptr [reading_a]
    test rcx, rcx
    jz fail_5
    lea rdx, state_a
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 0B0h]         ; GetGamepadState
    test al, al
    jz fail_5
    lea rcx, state_a
    lea rdx, exp_pad_cur
    call same28
    jne fail_5
    mov rcx, rbx
    mov rdx, qword ptr [reading_a]
    mov r8d, KIND_GAMEPAD
    mov r9, qword ptr [pad_device]
    lea rax, reading_b
    mov qword ptr [rsp + 20h], rax
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 30h]          ; GetPreviousReading
    test eax, eax
    jnz fail_5
    mov rcx, qword ptr [reading_b]
    test rcx, rcx
    jz fail_5
    lea rdx, state_b
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 0B0h]
    test al, al
    jz fail_5
    lea rcx, state_b
    lea rdx, exp_pad_prev
    call same28
    jne fail_5
    lea rcx, reading_b
    call release_slot
    lea rcx, reading_a
    call release_slot

    ; Step 6: keyboard current reading, key count and 12-byte key states; nothing newer.
    mov rcx, rbx
    mov edx, KIND_KEYBOARD
    mov r8, qword ptr [kb_device]
    lea r9, reading_a
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 20h]
    test eax, eax
    jnz fail_6
    mov rcx, qword ptr [reading_a]
    test rcx, rcx
    jz fail_6
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 70h]          ; GetKeyCount
    cmp eax, 3
    jne fail_6
    mov rcx, qword ptr [reading_a]
    mov edx, 16
    lea r8, keys
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 78h]          ; GetKeyState
    cmp eax, 3
    jne fail_6
    lea rcx, keys
    lea rdx, exp_key_0
    call same_key
    jne fail_6
    lea rcx, keys + 12
    lea rdx, exp_key_1
    call same_key
    jne fail_6
    lea rcx, keys + 24
    lea rdx, exp_key_2
    call same_key
    jne fail_6
    mov rcx, rbx
    mov rdx, qword ptr [reading_a]
    mov r8d, KIND_KEYBOARD
    mov r9, qword ptr [kb_device]
    lea rax, reading_b
    mov qword ptr [rsp + 20h], rax
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 28h]          ; GetNextReading
    cmp eax, READING_NOT_FOUND_
    jne fail_6
    cmp qword ptr [reading_b], 0
    jne fail_6
    lea rcx, reading_a
    call release_slot

    ; Step 7: rumble through the device found from its own deviceId; motors only.
    mov rcx, qword ptr [pad_device]
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 18h]          ; GetDeviceInfo
    test rax, rax
    jz fail_7
    mov rcx, rbx
    lea rdx, [rax + 20h]
    lea r8, found
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 80h]          ; FindDeviceFromId
    test eax, eax
    jnz fail_7
    mov rax, qword ptr [found]
    cmp rax, qword ptr [pad_device]
    jne fail_7
    mov rcx, qword ptr [found]
    lea rdx, rumble
    mov rax, qword ptr [rcx]
    call qword ptr [rax + 50h]          ; SetRumbleState
    lea rcx, found
    call release_slot
    mov rcx, rbx
    lea rdx, zero_id
    lea r8, found
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 80h]
    cmp eax, DEVICE_NOT_FOUND_
    jne fail_7
    cmp qword ptr [found], 0
    jne fail_7

    ; Step 8: unregister with a timeout and an infinite wait; a second unregister fails; every
    ; reference the caller took is released.
    mov rcx, rbx
    mov rdx, qword ptr [token_g]
    mov r8d, 1388h
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 68h]          ; UnregisterCallback
    test al, al
    jz fail_8
    mov rcx, rbx
    mov rdx, qword ptr [token_k]
    mov r8, -1
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 68h]
    test al, al
    jz fail_8
    mov rcx, rbx
    mov rdx, qword ptr [token_k]
    mov r8d, 1388h
    mov rax, qword ptr [rbx]
    call qword ptr [rax + 68h]
    test al, al
    jnz fail_8
    lea rcx, pad_device
    call release_slot
    lea rcx, kb_device
    call release_slot
    lea rcx, gi
    call release_slot
    call qword ptr [__imp_get_gameinput_outstanding_refs]
    test eax, eax
    jnz fail_8

    mov eax, 6A1h
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

done:
    add rsp, 40h
    pop rbx
    ret
native_entry ENDP
END
