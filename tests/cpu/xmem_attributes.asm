; Source-built fixture for tools/xmem_attribute_scan.py: one xmem call site per attribute-word
; form and route the scanner handles, including the XMemAllocDefault and XMemFreeDefault alias
; names. The image is only read; none of this code runs.
EXTERN __imp_XMemAlloc:QWORD
EXTERN __imp_XMemFree:QWORD
EXTERN __imp_XMemAllocDefault:QWORD
EXTERN __imp_XMemFreeDefault:QWORD
EXTERN XMemAlloc:PROC

.data
unwritten_word QWORD 0A8004000h  ; writable, no RIP-relative write: still not a constant
varying_word QWORD 05E50000h     ; written by set_word: not a constant
pointer_word DWORD 0A8004000h    ; written through a register in pointer_written: not a constant
guard_flag BYTE 0                ; writable byte the cmove-guarded site tests

CONST SEGMENT
read_only_word QWORD 020004000h  ; read-only section: a constant
CONST ENDS

.code
; Direct IAT call with a 32-bit immediate, in a function with unwind data.
immediate_iat PROC FRAME
    sub rsp, 40
    .allocstack 40
    .endprolog
    mov ecx, 100h
    mov edx, 05E50000h
    call QWORD PTR __imp_XMemAlloc
    add rsp, 40
    ret
immediate_iat ENDP

; rel32 call to the linker-generated thunk.
thunk_call PROC
    mov edx, 0A8004000h
    call XMemAlloc
    ret
thunk_call ENDP

; Word loaded from writable data that no RIP-relative instruction writes. A write through a
; pointer elsewhere could still change it, so it is data, not a constant.
data_unwritten PROC
    mov rdx, unwritten_word
    call QWORD PTR __imp_XMemAlloc
    ret
data_unwritten ENDP

; The word is written through an address held in a register, then loaded RIP-relative into EDX.
; No RIP-relative instruction writes it (lea only takes its address), and the load must stay data.
pointer_written PROC
    lea rax, pointer_word
    mov DWORD PTR [rax], 05E50000h
    mov edx, pointer_word
    call QWORD PTR __imp_XMemAlloc
    ret
pointer_written ENDP

; Word loaded from a read-only section.
read_only_constant PROC
    mov rdx, read_only_word
    call QWORD PTR __imp_XMemAlloc
    ret
read_only_constant ENDP

; Word loaded from data that set_word writes.
data_written PROC
    mov rdx, varying_word
    call QWORD PTR __imp_XMemAlloc
    ret
data_written ENDP

set_word PROC
    mov varying_word, rcx
    ret
set_word ENDP

; Register copy of a physical memory type.
register_copy PROC
    mov r8d, 0E0000000h
    mov rdx, r8
    call QWORD PTR __imp_XMemAlloc
    ret
register_copy ENDP

; Call through a register loaded from the IAT slot.
register_call PROC
    mov rax, __imp_XMemAlloc
    mov edx, 20004000h
    call rax
    ret
register_call ENDP

; Tail jump whose word comes from the caller. The first instruction keeps this from being a
; pure import thunk.
tail_argument PROC
    mov ecx, 10h
    jmp QWORD PTR __imp_XMemFree
tail_argument ENDP

forward_free PROC
    mov edx, 20004000h
    call tail_argument
    ret
forward_free ENDP

; 64-bit immediate with a reserved bit set.
reserved_bit PROC
    mov rdx, 2A8004000h
    call QWORD PTR __imp_XMemAlloc
    ret
reserved_bit ENDP

; xor zeroes the word.
zero_word PROC
    xor edx, edx
    call QWORD PTR __imp_XMemAlloc
    ret
zero_word ENDP

; Word computed from an argument.
computed_word PROC
    lea rdx, [rcx+rcx]
    call QWORD PTR __imp_XMemAlloc
    ret
computed_word ENDP

; Reached by a direct jump: the jump carries the earlier write.
jump_reached PROC
    mov edx, 1
    jmp skip
skip:
    call QWORD PTR __imp_XMemAlloc
    ret
jump_reached ENDP

; Reached only by an indirect jump: the earlier write must not be reported.
indirect_reached PROC
    mov edx, 2
    lea rax, landing
    jmp rax
landing:
    call QWORD PTR __imp_XMemAlloc
    ret
indirect_reached ENDP

; Both branches of an if/else set the same word.
join_same PROC
    test ecx, ecx
    jz other_same
    mov edx, 05E50000h
    jmp done_same
other_same:
    mov edx, 05E50000h
done_same:
    call QWORD PTR __imp_XMemAlloc
    ret
join_same ENDP

; The branches set different words; both must be reported.
join_differ PROC
    test ecx, ecx
    jz other_differ
    mov edx, 05E50000h
    jmp done_differ
other_differ:
    mov edx, 0A8004000h
done_differ:
    call QWORD PTR __imp_XMemAlloc
    ret
join_differ ENDP

; A loop between the definition and the call does not change the word.
loop_constant PROC
    mov edx, 20004000h
    xor eax, eax
again:
    inc eax
    cmp eax, 4
    jb again
    call QWORD PTR __imp_XMemAlloc
    ret
loop_constant ENDP

; A call between the definition and the site may change RDX.
call_clobber PROC
    mov edx, 05E50000h
    call set_word
    call QWORD PTR __imp_XMemAlloc
    ret
call_clobber ENDP

; A non-volatile register keeps its value across a call.
saved_across_call PROC
    mov ebx, 0A8004000h
    call set_word
    mov edx, ebx
    call QWORD PTR __imp_XMemAlloc
    ret
saved_across_call ENDP

; A 32-bit copy keeps only the low 32 bits of a 64-bit constant.
low_half PROC
    mov r8, 1A8004000h
    mov edx, r8d
    call QWORD PTR __imp_XMemAlloc
    ret
low_half ENDP

; A byte write leaves the upper bits of the earlier word in place.
partial_write PROC
    mov edx, 05E50000h
    mov dl, 7
    call QWORD PTR __imp_XMemAlloc
    ret
partial_write ENDP

; A byte xor of a register with itself clears only the low 8 bits: the word stays 05E50000h, so
; the site must not be reported as the constant 0 that xor edx, edx gives.
partial_xor_byte PROC
    mov edx, 05E50007h
    xor dl, dl
    call QWORD PTR __imp_XMemAlloc
    ret
partial_xor_byte ENDP

; The same with a 16-bit xor, which clears only the low 16 bits.
partial_xor_word PROC
    mov edx, 05E51234h
    xor dx, dx
    call QWORD PTR __imp_XMemAlloc
    ret
partial_xor_word ENDP

; Internal pass-through: not exported, so its one caller in the image decides the word.
forward_alloc PROC
    mov edx, 05E50000h
    call internal_passthrough
    ret
forward_alloc ENDP

internal_passthrough PROC
    mov ecx, 30h
    jmp QWORD PTR __imp_XMemAlloc
internal_passthrough ENDP

; Exported pass-through with no caller in the image.
exported_passthrough PROC
    mov ecx, 20h
    jmp QWORD PTR __imp_XMemAlloc
exported_passthrough ENDP

; The callee copies its entry register R8 into RDX. The caller also sets RDX to a different
; word, so the site resolves from the caller's R8 only.
caller_copy PROC
    mov edx, 05E50000h
    mov r8d, 0A8004000h
    call entry_copy
    ret
caller_copy ENDP

entry_copy PROC
    mov rdx, r8
    call QWORD PTR __imp_XMemAlloc
    ret
entry_copy ENDP

; The same through a 32-bit copy: the caller's R8 is 1A8004000h, so the resolved word is the low
; 32 bits. Without the truncation, bit 32 would set memory type 13 (undefined) and era_heap would
; return null.
caller_copy_low PROC
    mov edx, 05E50000h
    mov r8, 1A8004000h
    call entry_copy_low
    ret
caller_copy_low ENDP

entry_copy_low PROC
    mov edx, r8d
    call QWORD PTR __imp_XMemAlloc
    ret
entry_copy_low ENDP

; Direct call through the Default alias. The two allocation names have separate IAT slots even
; though they reach the same allocator, so the site must record the name it imported:
; XMemAllocDefault here, never XMemAlloc.
default_alloc PROC
    mov ecx, 40h
    mov edx, 05E50000h
    call QWORD PTR __imp_XMemAllocDefault
    ret
default_alloc ENDP

; Tail jump through the free alias, with a constant word.
default_free_tail PROC
    mov edx, 0A8004000h
    jmp QWORD PTR __imp_XMemFreeDefault
default_free_tail ENDP

; The shape released images use to choose the word at run time: an immediate, then a conditional
; move from another register guarded by a writable byte. The scanner has no constant for the
; conditional move, so it must report the site as computed rather than report the immediate.
cmove_guarded PROC
    mov edx, 04000000h
    cmp BYTE PTR [guard_flag], 0
    cmove rdx, rbx
    call QWORD PTR __imp_XMemAlloc
    ret
cmove_guarded ENDP
END
