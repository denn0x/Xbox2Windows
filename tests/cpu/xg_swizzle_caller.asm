; Source-built caller for xg_xs.dll swizzle selection and texture computer (#870, #1032):
;   xg_xs.dll!XGSuggestSwizzleMode
;   xg_xs.dll!XGCreateTextureComputer
;
; Call shape from DirectXTex and issue #498:
;   XGSuggestSwizzleMode(const D3D12_RESOURCE_DESC*, UINT, D3D12_TEXTURE_LAYOUT* out)
;   XGCreateTextureComputer(const D3D12_RESOURCE_DESC*, IXGTextureComputer** out)
;
; The fixture exercises:
; 1. Refusal control: invalid descriptor (Width == 0) rejected with E_INVALIDARG (0x80070057),
;    leaving output sentinel intact.
; 2. Separate output pointer: flag 0 selects suggested mode (0x105) into separate variable,
;    leaving descriptor Layout unchanged.
; 3. Aliased output pointer: passing descriptor's own Layout field as output pointer writes
;    suggested mode in-place, matching separate output.
; 4. Texture computer integration (tiled): feeds resulting descriptor into XGCreateTextureComputer,
;    uploads 147 texels across 3 mips (28x4, 14x2, 7x1), detiles them back with 100% round-trip match,
;    and verifies exactly 139 texels disagree when comparing raw tiled storage against linear layout
;    (synthetic Morton bit-order oracle as fixture policy; see #1032 for full AddrLib reference).
; 5. Linear layout control: verifies linear descriptor copies texels linearly with 0 disagreements.
; 6. Computer refusal control: XGCreateTextureComputer rejects invalid descriptor with E_INVALIDARG,
;    leaving computer pointer intact.
; 7. Returns 0x870 on success (Refs #870, #1032).

EXTERN __imp_XGSuggestSwizzleMode:QWORD
EXTERN __imp_XGCreateTextureComputer:QWORD
PUBLIC native_entry

.data
ALIGN 8
; D3D12_RESOURCE_DESC (56 bytes / 38h):
;   0x00: Dimension (3 = TEXTURE2D), Pad (0)
;   0x08: Alignment (0)
;   0x10: Width (28)
;   0x18: Height (4)
;   0x1C: DepthOrArraySize (1)
;   0x1E: MipLevels (3)
;   0x20: Format (28 = DXGI_FORMAT_R8G8B8A8_UNORM)
;   0x24: SampleDesc.Count (1), SampleDesc.Quality (0)
;   0x2C: Layout (0 = UNKNOWN)
;   0x30: Flags (0 = NONE), Pad (0)
test_desc:
dd 3, 0                 ; Dimension = TEXTURE2D (3)
dq 0                    ; Alignment = 0
dq 28                   ; Width = 28
dd 4                    ; Height = 4
dw 1                    ; DepthOrArraySize = 1
dw 3                    ; MipLevels = 3
dd 28                   ; Format = R8G8B8A8_UNORM (28)
dd 1, 0                 ; SampleDesc: Count=1, Quality=0
dd 0                    ; Layout = UNKNOWN (0)
dd 0, 0                 ; Flags = 0, Pad

invalid_desc:
dd 3, 0                 ; Dimension = TEXTURE2D (3)
dq 0                    ; Alignment = 0
dq 0                    ; Width = 0 (INVALID)
dd 4                    ; Height = 4
dw 1                    ; DepthOrArraySize = 1
dw 3                    ; MipLevels = 3
dd 28                   ; Format = R8G8B8A8_UNORM (28)
dd 1, 0                 ; SampleDesc: Count=1, Quality=0
dd 0                    ; Layout = UNKNOWN (0)
dd 0, 0                 ; Flags = 0, Pad

linear_desc:
dd 3, 0                 ; Dimension = TEXTURE2D (3)
dq 0                    ; Alignment = 0
dq 28                   ; Width = 28
dd 4                    ; Height = 4
dw 1                    ; DepthOrArraySize = 1
dw 3                    ; MipLevels = 3
dd 28                   ; Format = R8G8B8A8_UNORM (28)
dd 1, 0                 ; SampleDesc: Count=1, Quality=0
dd 1                    ; Layout = ROW_MAJOR (1)
dd 0, 0                 ; Flags = 0, Pad

ALIGN 8
separate_layout dd 0
pad_layout      dd 0
pComputer       dq 0

; 147 texels: 112 (mip0) + 28 (mip1) + 7 (mip2)
ALIGN 8
linear_src      dd 147 dup(0)
linear_dst      dd 147 dup(0)
ALIGN 8
tiled_storage   db 1024 dup(0)
linear_storage  db 1024 dup(0)

.code
native_entry PROC FRAME
    sub rsp, 58h
    .allocstack 58h
    .endprolog

    ; Initialize linear_src test pattern: texel[i] = i + 100h
    lea rcx, [linear_src]
    xor edx, edx
init_pattern_loop:
    lea eax, [edx + 100h]
    mov dword ptr [rcx + rdx*4], eax
    inc edx
    cmp edx, 147
    jb init_pattern_loop

    ; -------------------------------------------------------------
    ; Step 1: Refusal control - XGSuggestSwizzleMode with invalid desc
    ; Must return E_INVALIDARG (0x80070057) and preserve sentinel.
    ; -------------------------------------------------------------
    mov dword ptr [separate_layout], 0DEADBEEFh
    lea rcx, [invalid_desc]
    xor edx, edx        ; flags = 0
    lea r8, [separate_layout]
    call qword ptr [__imp_XGSuggestSwizzleMode]
    cmp eax, 80070057h  ; E_INVALIDARG
    jne fail_refusal_code
    cmp dword ptr [separate_layout], 0DEADBEEFh
    jne fail_refusal_clobber

    ; -------------------------------------------------------------
    ; Step 2: Separate output pointer with flag 0
    ; Must return S_OK, write 0x105 to separate_layout, and leave
    ; test_desc.Layout as 0 (UNKNOWN).
    ; -------------------------------------------------------------
    mov dword ptr [separate_layout], 0
    mov dword ptr [test_desc + 2Ch], 0 ; Layout = UNKNOWN
    lea rcx, [test_desc]
    xor edx, edx        ; flags = 0
    lea r8, [separate_layout]
    call qword ptr [__imp_XGSuggestSwizzleMode]
    test eax, eax
    jnz fail_separate_call
    cmp dword ptr [separate_layout], 105h
    jne fail_separate_mode
    cmp dword ptr [test_desc + 2Ch], 0
    jne fail_separate_clobber

    ; -------------------------------------------------------------
    ; Step 3: Aliased output pointer (&test_desc.Layout) with flag 0
    ; Must write 0x105 directly into test_desc.Layout, matching separate.
    ; -------------------------------------------------------------
    lea rcx, [test_desc]
    xor edx, edx        ; flags = 0
    lea r8, [test_desc + 2Ch] ; aliased pointer to Layout field
    call qword ptr [__imp_XGSuggestSwizzleMode]
    test eax, eax
    jnz fail_aliased_call
    mov eax, dword ptr [test_desc + 2Ch]
    cmp eax, dword ptr [separate_layout]
    jne fail_aliased_mismatch

    ; -------------------------------------------------------------
    ; Step 4: Tiled computer creation and copy / detile verification
    ; -------------------------------------------------------------
    mov qword ptr [pComputer], 0
    lea rcx, [test_desc]
    lea rdx, [pComputer]
    call qword ptr [__imp_XGCreateTextureComputer]
    test eax, eax
    jnz fail_create_comp
    mov r12, qword ptr [pComputer]
    test r12, r12
    jz fail_create_comp

    ; Zero out tiled_storage and linear_dst
    lea rdi, [tiled_storage]
    xor eax, eax
    mov ecx, 256
    rep stosd
    lea rdi, [linear_dst]
    mov ecx, 147
    rep stosd

    ; Subresource 0 (Mip 0: 28x4, rowPitch = 28*4 = 112)
    ; CopyIntoSubresource(dst=tiled_storage, plane=0, sub=0, src=linear_src, rowPitch=112, slicePitch=0)
    mov rcx, r12
    lea rdx, [tiled_storage]
    xor r8d, r8d        ; plane = 0
    xor r9d, r9d        ; sub = 0
    lea rax, [linear_src]
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 112 ; 28 * 4
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h] ; CopyIntoSubresource (slot 7, offset 56 / 0x38)
    test eax, eax
    jnz fail_copy_into

    ; Subresource 1 (Mip 1: 14x2, rowPitch = 14*4 = 56)
    mov rcx, r12
    lea rdx, [tiled_storage]
    xor r8d, r8d        ; plane = 0
    mov r9d, 1          ; sub = 1
    lea rax, [linear_src]
    add rax, 448
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 56 ; 14 * 4
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h]
    test eax, eax
    jnz fail_copy_into

    ; Subresource 2 (Mip 2: 7x1, rowPitch = 7*4 = 28)
    mov rcx, r12
    lea rdx, [tiled_storage]
    xor r8d, r8d        ; plane = 0
    mov r9d, 2          ; sub = 2
    lea rax, [linear_src]
    add rax, 560
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 28 ; 7 * 4
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h]
    test eax, eax
    jnz fail_copy_into

    ; Now detile back via CopyFromSubresource into linear_dst
    ; Subresource 0 -> linear_dst
    mov rcx, r12
    lea rdx, [linear_dst]
    xor r8d, r8d
    xor r9d, r9d
    lea rax, [tiled_storage]
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 112
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 40h] ; CopyFromSubresource (slot 8, offset 64 / 0x40)
    test eax, eax
    jnz fail_copy_from

    ; Subresource 1 -> linear_dst + 448
    mov rcx, r12
    lea rdx, [linear_dst]
    add rdx, 448
    xor r8d, r8d
    mov r9d, 1
    lea rax, [tiled_storage]
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 56
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 40h]
    test eax, eax
    jnz fail_copy_from

    ; Subresource 2 -> linear_dst + 560
    mov rcx, r12
    lea rdx, [linear_dst]
    add rdx, 560
    xor r8d, r8d
    mov r9d, 2
    lea rax, [tiled_storage]
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 28
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 40h]
    test eax, eax
    jnz fail_copy_from

    ; Check 100% round-trip: linear_dst == linear_src (147 texels)
    lea r8, [linear_dst]
    lea r9, [linear_src]
    xor edx, edx
roundtrip_check_loop:
    mov eax, dword ptr [r8 + rdx*4]
    cmp eax, dword ptr [r9 + rdx*4]
    jne fail_roundtrip
    inc edx
    cmp edx, 147
    jb roundtrip_check_loop

    ; Count disagreements between raw tiled_storage and linear_src:
    ; Exactly 139 must disagree! (8 agree).
    xor ebx, ebx        ; mismatch counter
    lea r8, [tiled_storage]
    lea r9, [linear_src]
    ; Mip 0: 112 texels at tiled_storage + 0
    xor edx, edx
m0_disagree_loop:
    mov eax, dword ptr [r8 + rdx*4]
    cmp eax, dword ptr [r9 + rdx*4]
    je m0_same
    inc ebx
m0_same:
    inc edx
    cmp edx, 112
    jb m0_disagree_loop

    ; Mip 1: 28 texels at tiled_storage + 512
    xor edx, edx
m1_disagree_loop:
    mov eax, dword ptr [r8 + 512 + rdx*4]
    cmp eax, dword ptr [r9 + 448 + rdx*4]
    je m1_same
    inc ebx
m1_same:
    inc edx
    cmp edx, 28
    jb m1_disagree_loop

    ; Mip 2: 7 texels at tiled_storage + 640
    xor edx, edx
m2_disagree_loop:
    mov eax, dword ptr [r8 + 640 + rdx*4]
    cmp eax, dword ptr [r9 + 560 + rdx*4]
    je m2_same
    inc ebx
m2_same:
    inc edx
    cmp edx, 7
    jb m2_disagree_loop

    ; Must have exactly 139 disagreements (tiling oracle check)
    cmp ebx, 139
    jne fail_disagree_count

    ; Release tiled computer
    mov rcx, r12
    mov rax, qword ptr [r12]
    call qword ptr [rax + 8] ; Release (slot 1)

    ; -------------------------------------------------------------
    ; Step 5: Linear layout control
    ; With Layout = 1 (ROW_MAJOR), copying into storage produces
    ; 0 disagreements against linear_src (100% linear match).
    ; -------------------------------------------------------------
    mov qword ptr [pComputer], 0
    lea rcx, [linear_desc]
    lea rdx, [pComputer]
    call qword ptr [__imp_XGCreateTextureComputer]
    test eax, eax
    jnz fail_create_linear
    mov r12, qword ptr [pComputer]
    test r12, r12
    jz fail_create_linear

    lea rdi, [linear_storage]
    xor eax, eax
    mov ecx, 256
    rep stosd

    ; Subresource 0
    mov rcx, r12
    lea rdx, [linear_storage]
    xor r8d, r8d
    xor r9d, r9d
    lea rax, [linear_src]
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 112
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h]
    test eax, eax
    jnz fail_linear_copy

    ; Subresource 1
    mov rcx, r12
    lea rdx, [linear_storage]
    xor r8d, r8d
    mov r9d, 1
    lea rax, [linear_src]
    add rax, 448
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 56
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h]
    test eax, eax
    jnz fail_linear_copy

    ; Subresource 2
    mov rcx, r12
    lea rdx, [linear_storage]
    xor r8d, r8d
    mov r9d, 2
    lea rax, [linear_src]
    add rax, 560
    mov qword ptr [rsp + 20h], rax
    mov qword ptr [rsp + 28h], 28
    mov qword ptr [rsp + 30h], 0
    mov rax, qword ptr [r12]
    call qword ptr [rax + 38h]
    test eax, eax
    jnz fail_linear_copy

    ; Verify 0 disagreements for linear storage
    lea r8, [linear_storage]
    lea r9, [linear_src]
    xor ebx, ebx
    xor edx, edx
lin_m0_loop:
    mov eax, dword ptr [r8 + rdx*4]
    cmp eax, dword ptr [r9 + rdx*4]
    je lin_m0_same
    inc ebx
lin_m0_same:
    inc edx
    cmp edx, 112
    jb lin_m0_loop

    xor edx, edx
lin_m1_loop:
    mov eax, dword ptr [r8 + 512 + rdx*4]
    cmp eax, dword ptr [r9 + 448 + rdx*4]
    je lin_m1_same
    inc ebx
lin_m1_same:
    inc edx
    cmp edx, 28
    jb lin_m1_loop

    xor edx, edx
lin_m2_loop:
    mov eax, dword ptr [r8 + 640 + rdx*4]
    cmp eax, dword ptr [r9 + 560 + rdx*4]
    je lin_m2_same
    inc ebx
lin_m2_same:
    inc edx
    cmp edx, 7
    jb lin_m2_loop

    test ebx, ebx
    jnz fail_linear_disagree ; must have 0 disagreements

    mov rcx, r12
    mov rax, qword ptr [r12]
    call qword ptr [rax + 8] ; Release

    ; -------------------------------------------------------------
    ; Step 6: Refusal control for XGCreateTextureComputer
    ; Must return E_INVALIDARG and leave sentinel untouched.
    ; -------------------------------------------------------------
    mov rax, 0DEADBEEFCAFEBABEh
    mov qword ptr [pComputer], rax
    lea rcx, [invalid_desc]
    lea rdx, [pComputer]
    call qword ptr [__imp_XGCreateTextureComputer]
    cmp eax, 80070057h  ; E_INVALIDARG
    jne fail_create_refusal
    mov rax, 0DEADBEEFCAFEBABEh
    cmp qword ptr [pComputer], rax
    jne fail_create_clobber

    ; -------------------------------------------------------------
    ; Step 7: Success exit code 0x870 (Refs #870)
    ; -------------------------------------------------------------
    mov eax, 870h
    add rsp, 58h
    ret

; Failure exits
fail_refusal_code:
    mov eax, 0E01h
    add rsp, 58h
    ret

fail_refusal_clobber:
    mov eax, 0E01h      ; Caught specifically by mutant clobber
    add rsp, 58h
    ret

fail_separate_call:
    mov eax, 0E10h
    add rsp, 58h
    ret

fail_separate_mode:
    mov eax, 0E11h
    add rsp, 58h
    ret

fail_separate_clobber:
    mov eax, 0E12h
    add rsp, 58h
    ret

fail_aliased_call:
    mov eax, 0E20h
    add rsp, 58h
    ret

fail_aliased_mismatch:
    mov eax, 0E02h      ; Aliased layout did not match separate layout
    add rsp, 58h
    ret

fail_create_comp:
    mov eax, 0E30h
    add rsp, 58h
    ret

fail_copy_into:
    mov eax, 0E31h
    add rsp, 58h
    ret

fail_copy_from:
    mov eax, 0E32h
    add rsp, 58h
    ret

fail_roundtrip:
    mov eax, 0E03h      ; Texel round-trip mismatch
    add rsp, 58h
    ret

fail_disagree_count:
    mov eax, 0E04h      ; Tiled storage disagree count mismatch (expected 139)
    add rsp, 58h
    ret

fail_create_linear:
    mov eax, 0E40h
    add rsp, 58h
    ret

fail_linear_copy:
    mov eax, 0E41h
    add rsp, 58h
    ret

fail_linear_disagree:
    mov eax, 0E05h      ; Linear storage disagreement mismatch (expected 0)
    add rsp, 58h
    ret

fail_create_refusal:
    mov eax, 0E06h
    add rsp, 58h
    ret

fail_create_clobber:
    mov eax, 0E06h
    add rsp, 58h
    ret

native_entry ENDP
END
