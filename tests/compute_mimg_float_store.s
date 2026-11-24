s_mov_b32 s12, s7
s_mov_b32 s13, 0
s_load_dwordx8 s[24:31], s[12:13], 0x40
s_load_dwordx8 s[32:39], s[12:13], 0x60
v_lshl_add_u32 v8, s10, 2, v0
v_lshl_add_u32 v9, s11, 1, v1
v_mov_b32_e32 v10, 1
s_waitcnt lgkmcnt(0)
image_load_mip v[14:15], v[8:10], s[32:39] dmask:0x5 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
v_mov_b32_e32 v16, 0x40810000
v_mov_b32_e32 v17, 0xbf810001
image_store v[14:17], v[8:9], s[24:31] dmask:0xf dim:SQ_RSRC_IMG_2D
s_endpgm
