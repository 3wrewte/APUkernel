//
// read_test.S — GPU reads a file via read() then echoes it via write()
//
// Tests the full read() syscall path:
//   1. GPU calls read(fd, output_bo_addr, count) via mailbox
//   2. CPU kernel_read directly into output BO
//   3. GPU calls write(stdout, output_bo_addr, retval) to echo the data
//   4. Exit
//
// s[0:1] = mailbox base
// Mailbox fields:
//   +0xC8: output_addr (u64) — set by kernel
//   +0xD0: fd           (u32) — set by compute_runner before dispatch
//   +0x38: retval       (u64) — set by CPU after read()

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	read_test
	.p2align 8
	.type	read_test,@function

read_test:
	s_waitcnt lgkmcnt(0)

	// v[4:5] = mailbox base
	v_mov_b32_e32 v4, s0
	v_mov_b32_e32 v5, s1

	// --- Load fd from mailbox +0xD0 ---
	v_add_u32_e32 v0, 0xD0, v4
	v_mov_b32_e32 v1, v5
	flat_load_dword v6, v[0:1]          // v6 = fd
	s_waitcnt vmcnt(0)

	// --- Load output_addr from mailbox +0xC8 ---
	v_add_u32_e32 v0, 0xC8, v4
	v_mov_b32_e32 v1, v5
	flat_load_dwordx2 v[8:9], v[0:1]    // v[8:9] = output_addr
	s_waitcnt vmcnt(0)

	// --- Syscall: read(fd, output_addr, 256) ---
	// syscall_nr = 0 at +0x08
	v_add_u32_e32 v0, 0x08, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 0
	flat_store_dword v[0:1], v2

	// arg0 = fd at +0x10
	v_add_u32_e32 v0, 0x10, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, v6                // fd
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg1 = output_addr at +0x18
	v_add_u32_e32 v0, 0x18, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, v8
	v_mov_b32_e32 v3, v9
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg2 = 256 (count) at +0x20
	v_add_u32_e32 v0, 0x20, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 256
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// Publish syscall: state = 1
	s_waitcnt vmcnt(0)
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_sendmsg sendmsg(MSG_INTERRUPT)

	// Wait for CPU to complete read
1:
	buffer_wbinvl1_vol
	flat_load_dword v2, v[4:5]
	s_waitcnt vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz 1b

	// --- Read retval (bytes read) from mailbox +0x38 ---
	v_add_u32_e32 v0, 0x38, v4
	v_mov_b32_e32 v1, v5
	flat_load_dword v6, v[0:1]          // v6 = retval (bytes read)
	s_waitcnt vmcnt(0)

	// If retval <= 0, exit with error
	v_cmp_le_u32 vcc, 0, v6
	s_cbranch_vccz 99f

	// --- Syscall: write(stdout, output_addr, retval) ---
	// syscall_nr = 1 at +0x08
	v_add_u32_e32 v0, 0x08, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2

	// arg0 = 1 (stdout) at +0x10
	v_add_u32_e32 v0, 0x10, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg1 = output_addr at +0x18 (data source for write)
	v_add_u32_e32 v0, 0x18, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, v8
	v_mov_b32_e32 v3, v9
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg2 = retval (count) at +0x20
	v_add_u32_e32 v0, 0x20, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, v6                // count = bytes read
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// Publish syscall
	s_waitcnt vmcnt(0)
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_sendmsg sendmsg(MSG_INTERRUPT)

	// Wait for CPU
2:
	buffer_wbinvl1_vol
	flat_load_dword v2, v[4:5]
	s_waitcnt vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz 2b

99:
	// Exit
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 3
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_endpgm

.Lend_read_test:
	.size read_test, .Lend_read_test - read_test
