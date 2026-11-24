EXTERN __imp_placed_open:QWORD
EXTERN __imp_placed_mode:QWORD
EXTERN __imp_placed_resource:QWORD
EXTERN __imp_placed_bind:QWORD
EXTERN __imp_placed_draw:QWORD
EXTERN __imp_placed_submit:QWORD
EXTERN __imp_placed_close:QWORD
.code
native_entry PROC FRAME
    sub rsp, 72
    .allocstack 72
    .endprolog
    call qword ptr [__imp_placed_open]
    cmp rax, 77
    je cleanup
    test rax,rax
    jz fail
    mov [rsp+32],rax
    call qword ptr [__imp_placed_mode]
    mov [rsp+52],eax
    mov rcx,[rsp+32]
    mov edx,260
    call qword ptr [__imp_placed_resource]
    test rax,rax
    jz fail
    mov rcx,10000000000h
    add rcx,[rsp+32]
    mov [rsp+40],rcx
    mov edx,260
    cmp dword ptr [rsp+52],4
    jne high_resource
    xor edx,edx
high_resource:
    call qword ptr [__imp_placed_resource]
    test rax,rax
    jz fail
    mov eax,[rsp+52]
    mov rcx,[rsp+32]
    mov edx,4
    test eax,eax
    jz bind_view
    mov rcx,[rsp+40]
    cmp eax,2
    jne bind_view
    mov edx,8
bind_view:
    add rcx,256
    mov r8d,4
    call qword ptr [__imp_placed_bind]
    mov ecx,3
    mov edx,1
    call qword ptr [__imp_placed_draw]
    ; Update the last DWORD after recording, before queue submission.
    mov rcx,[rsp+32]
    mov dword ptr [rcx+256],41
    mov rcx,[rsp+40]
    mov dword ptr [rcx+256],135
    mov ecx,1
    mov edx,1
    cmp dword ptr [rsp+52],3
    jne submit_draw
    xor ecx,ecx
submit_draw:
    call qword ptr [__imp_placed_submit]
    test eax,eax
    jnz fail
    mov eax,42
    jmp cleanup
fail:
    mov eax,20
cleanup:
    mov [rsp+48],eax
    call qword ptr [__imp_placed_close]
    test eax,eax
    jz done_status
    cmp dword ptr [rsp+48],42
    je release_failed
done_status:
    mov eax,[rsp+48]
    jmp done
release_failed:
    mov eax,18
done:
    add rsp,72
    ret
native_entry ENDP
END
