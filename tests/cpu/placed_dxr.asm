; Source-built fixture ABI. Requests are two {guest destination, guest scratch}
; records in the provider's writable exchange buffer, in BLAS then TLAS order.
EXTERN __imp_dxr_inputs:QWORD
EXTERN __imp_dxr_batch:QWORD
EXTERN __imp_dxr_release:QWORD
.code
native_entry PROC FRAME
    sub rsp, 56
    .allocstack 56
    .endprolog
    call qword ptr [__imp_dxr_inputs]
    cmp rax,77
    je cleanup
    test rax,rax
    jz failed
    mov rcx,rax
    mov rdx,[rcx]
    add rdx,512
    mov [rcx+48],rdx
    mov rdx,[rcx+16]
    add rdx,768
    mov [rcx+56],rdx
    mov rdx,[rcx+8]
    add rdx,512
    mov [rcx+64],rdx
    mov rdx,[rcx+24]
    cmp qword ptr [rcx+40],1
    jne check_unmapped
    mov rdx,[rcx+32]
check_unmapped:
    add rdx,768
    cmp qword ptr [rcx+40],2
    jne submit
    mov edx,1000h
submit:
    mov [rcx+72],rdx
    add rcx,48
    mov edx,2
    call qword ptr [__imp_dxr_batch]
    test rax,rax
    jz success
    cmp rax,2
    jne failed
    mov eax,22
    jmp cleanup
success:
    mov eax,42
    jmp cleanup
failed:
    mov eax,20
cleanup:
    mov [rsp+32],eax
    call qword ptr [__imp_dxr_release]
    test rax,rax
    jnz release_failed
    mov eax,[rsp+32]
    jmp done
release_failed:
    mov eax,18
done:
    add rsp,56
    ret
native_entry ENDP
END
