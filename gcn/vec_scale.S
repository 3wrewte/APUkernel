//
// vec_scale.S — GPU vector scaling: output[i] = input[i] * 2
//
// Tests the input/output BO data path:
//   1. Read input/output GPU addresses from mailbox
//   2. Each of 64 threads processes one element
//   3. Writes result message via syscall, then exits
//
// s[0:1] = mailbox base address
// v0     = thread local ID (0..63)
//
// Mailbox fields used:
//   +0xC0: input_addr  (u64) — set by kernel before dispatch
//   +0xC8: output_addr (u64)

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	vec_scale
	.p2align 8
	.type	vec_scale,@function

vec_scale:
	s_waitcnt lgkmcnt(0)

	// v8 = thread_id (save)
	v_mov_b32_e32 v8, v0

	// v[4:5] = mailbox base
	v_mov_b32_e32 v4, s0
	v_mov_b32_e32 v5, s1

	// Load input_addr from mailbox+0xC0 into v[6:7]
	v_add_u32_e32 v0, 0xC0, v4
	v_mov_b32_e32 v1, v5
	flat_load_dwordx2 v[6:7], v[0:1]
	s_waitcnt vmcnt(0)

	// Load output_addr from mailbox+0xC8 into v[10:11]
	v_add_u32_e32 v0, 0xC8, v4
	v_mov_b32_e32 v1, v5
	flat_load_dwordx2 v[10:11], v[0:1]
	s_waitcnt vmcnt(0)

	// v9 = thread_id * 4 (byte offset)
	v_lshlrev_b32_e32 v9, 2, v8

	// Load input[tid]
	v_add_u32_e32 v0, v9, v6
	v_mov_b32_e32 v1, v7
	flat_load_dword v2, v[0:1]
	s_waitcnt vmcnt(0)

	// v2 *= 2
	v_lshlrev_b32_e32 v2, 1, v2

	// Store output[tid]
	v_add_u32_e32 v0, v9, v10
	v_mov_b32_e32 v1, v11
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)

	// --- Syscall: write("GPU OK\n") ---
	v_add_u32_e32 v0, 0x40, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 0x4e555047
	flat_store_dword v[0:1], v2
	v_add_u32_e32 v0, 0x44, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 0x000a4b4f
	flat_store_dword v[0:1], v2

	v_add_u32_e32 v0, 0x08, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2

	v_add_u32_e32 v0, 0x10, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	v_add_u32_e32 v0, 0x20, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 8
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	s_waitcnt vmcnt(0)
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_sendmsg sendmsg(MSG_INTERRUPT)

1:
	buffer_wbinvl1_vol
	flat_load_dword v2, v[4:5]
	s_waitcnt vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz 1b

	// Exit
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 3
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_endpgm

.Lend_vec_scale:
	.size vec_scale, .Lend_vec_scale - vec_scale
