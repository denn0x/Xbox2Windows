s_mov_b32 s12, s7
s_mov_b32 s13, 0
s_load_dwordx4 s[20:23], s[12:13], 0x30
s_load_dwordx8 s[24:31], s[12:13], 0x40
v_lshl_add_u32 v8, s10, 3, v0
v_lshl_add_u32 v9, s11, 3, v1
v_mov_b32_e32 v10, 0
v_mov_b32_e32 v12, v9
v_lshlrev_b32_e32 v13, 4, v8
s_waitcnt lgkmcnt(0)
image_msaa_load v[8:11], v[8:10], s[24:31] dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA unorm
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[8:11], v[12:13], s[20:23], 0 idxen offen
s_endpgm
