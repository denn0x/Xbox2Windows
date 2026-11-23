; A typed startup callback called through the load config's dispatch slot (#903).
;
; load_config_callback.cpp, compiled with /guard:cf, calls both entries of callback_table through
; __guard_dispatch_icall_fptr. The table and both guard slots are in .rdata (read only), and each holds an
; address, so each has a DIR64 relocation. The second entry is null, and the caller tests and skips it.
; The callback records its arguments and the number of calls, and native_entry hands them to emit_trace
; (cpu_exe_io.dll) as three words:
;   a | b << 32, c | calls << 32, d - __ImageBase
; native_entry returns the callback's result, ORed with 10000h if the dispatch slot no longer holds this
; image's guard_dispatch_nop and with 20000h if the check slot no longer holds guard_check_nop.
;
; Variants: OTHER binds other_callback (the negative control). INVALID binds mid_target, which is inside
; record_callback, so a /guard:cf link leaves it out of the function table. The same objects are linked
; with and without /guard:cf; without it the linker gives __guard_fids_table and __guard_fids_count 0.
option casemap:none
EXTERN __ImageBase:BYTE
EXTERN __imp_emit_trace:QWORD
EXTERN run_callbacks:PROC
EXTERN __guard_fids_table:BYTE
EXTERN __guard_fids_count:ABS
EXTERN __guard_flags:ABS
PUBLIC callback_table, marker, native_entry
PUBLIC __guard_check_icall_fptr, __guard_dispatch_icall_fptr, _load_config_used

.data
__security_cookie QWORD 02B992DDFA232h
marker QWORD 0
calls DWORD 0
ALIGN 8
trace QWORD 3 DUP (0)

.const
ALIGN 8
callback_table LABEL QWORD
IFDEF OTHER
    QWORD other_callback
ELSEIFDEF INVALID
    QWORD mid_target
ELSE
    QWORD record_callback
ENDIF
    QWORD 0
__guard_check_icall_fptr QWORD guard_check_nop
__guard_dispatch_icall_fptr QWORD guard_dispatch_nop

ALIGN 8
_load_config_used LABEL BYTE
    DWORD 138h                          ; Size
    BYTE 54h DUP (0)
    QWORD __security_cookie             ; 58h SecurityCookie
    QWORD 0, 0                          ; 60h SEHandlerTable, SEHandlerCount
    QWORD __guard_check_icall_fptr      ; 70h GuardCFCheckFunctionPointer
    QWORD __guard_dispatch_icall_fptr   ; 78h GuardCFDispatchFunctionPointer
    QWORD __guard_fids_table            ; 80h GuardCFFunctionTable
    QWORD __guard_fids_count            ; 88h GuardCFFunctionCount
    DWORD __guard_flags                 ; 90h GuardFlags
    BYTE 0A4h DUP (0)

; Every function starts on 16 bytes, so no CFG bitmap block that holds mid_target is marked valid as a
; whole for an unaligned function start.
.code
ALIGN 16
guard_check_nop PROC
    ret
guard_check_nop ENDP

ALIGN 16
guard_dispatch_nop PROC
    jmp rax
guard_dispatch_nop ENDP

; int callback(int a, int b, int c, void* d)
ALIGN 16
record_callback PROC
    inc calls
    mov dword ptr trace, ecx
    mov dword ptr trace + 4, edx
    mov dword ptr trace + 8, r8d
    lea rax, __ImageBase
    sub r9, rax
    mov trace + 16, r9
    lea eax, [rcx + rdx * 2]
    ret
    ALIGN 16
mid_target::
    mov eax, 77h
    ret
    ALIGN 16
record_callback ENDP

; The control: records b and a in the other order and returns a different result.
ALIGN 16
other_callback PROC
    inc calls
    mov dword ptr trace, edx
    mov dword ptr trace + 4, ecx
    mov dword ptr trace + 8, r8d
    lea rax, __ImageBase
    sub r9, rax
    mov trace + 16, r9
    lea eax, [rcx + rdx * 4]
    ret
other_callback ENDP

ALIGN 16
native_entry PROC FRAME
    sub rsp, 40
    .allocstack 40
    .endprolog
    call run_callbacks
    mov dword ptr [rsp + 32], eax
    mov eax, calls
    mov dword ptr trace + 12, eax
    lea rcx, trace
    call qword ptr [__imp_emit_trace]
    mov eax, dword ptr [rsp + 32]
    and eax, 0FFFFh
    lea rdx, guard_dispatch_nop
    cmp rdx, __guard_dispatch_icall_fptr
    je dispatch_kept
    or eax, 10000h
dispatch_kept:
    lea rdx, guard_check_nop
    cmp rdx, __guard_check_icall_fptr
    je check_kept
    or eax, 20000h
check_kept:
    add rsp, 40
    ret
native_entry ENDP
END
