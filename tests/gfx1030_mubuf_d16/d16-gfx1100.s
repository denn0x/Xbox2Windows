// SPDX-License-Identifier: MIT
.amdgcn_target "amdgcn-amd-amdhsa--gfx1100"
.text
.p2align 8
.globl store_d16
.type store_d16,@function
store_d16:
    // Each workgroup contains one wave32. v0 is the work-item ID within it.
    s_lshl_b32 s4, s2, 5
    v_add_nc_u32 v0, s4, v0
    s_load_dwordx2 s[0:1], s[0:1], 0x0
    s_waitcnt lgkmcnt(0)
    // Linear stride 8, identity selectors, format 55 (RGBA16 UINT),
    // OOB_SELECT 1 with 64 record indices.
    s_and_b32 s1, s1, 0xffff
    s_or_b32 s1, s1, 0x00080000
    s_mov_b32 s2, 64
    s_mov_b32 s3, 0x10037fac
    v_xor_b32 v1, 0x24681357, v0
    v_xor_b32 v2, 0xca5e389b, v0
.ifdef CONTROL_XYZW
    buffer_store_d16_format_xyzw v[1:2], v0, s[0:3], 0 idxen
.else
    buffer_store_d16_format_xyz v[1:2], v0, s[0:3], 0 idxen
.endif
    s_waitcnt_vscnt null, 0
    s_endpgm
.size store_d16, .-store_d16

.rodata
.p2align 6
.amdhsa_kernel store_d16
    .amdhsa_group_segment_fixed_size 0
    .amdhsa_private_segment_fixed_size 0
    .amdhsa_kernarg_size 8
    .amdhsa_user_sgpr_kernarg_segment_ptr 1
    .amdhsa_system_sgpr_workgroup_id_x 1
    .amdhsa_system_vgpr_workitem_id 0
    .amdhsa_next_free_vgpr 3
    .amdhsa_next_free_sgpr 5
    .amdhsa_wavefront_size32 1
.end_amdhsa_kernel

.amdgpu_metadata
---
amdhsa.version: [1, 0]
amdhsa.kernels:
  - .name: store_d16
    .symbol: store_d16.kd
    .kernarg_segment_size: 8
    .group_segment_fixed_size: 0
    .private_segment_fixed_size: 0
    .kernarg_segment_align: 8
    .wavefront_size: 32
    .sgpr_count: 5
    .vgpr_count: 3
    .max_flat_workgroup_size: 32
    .reqd_workgroup_size: [32, 1, 1]
    .args:
      - .name: output
        .size: 8
        .offset: 0
        .value_kind: global_buffer
        .address_space: global
...
.end_amdgpu_metadata
