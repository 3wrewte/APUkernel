//
// sendmsg_live.S — live-wavefront syscall proof for gfx902
//
// s[0:1] receives the mailbox BO flat address via COMPUTE_USER_DATA_0/1.
// The shader requests write(1, "hello from CU\n", 14), sends a message to
// the CP, then stays alive and polls mailbox.state until the CPU clears it.
// Only final program exit uses s_endpgm.
//
// Mailbox layout used by kernel-patches/heteroken.c ioctl path:
//   +0x00: state       (0=idle, 1=syscall_pending, 3=exit)
//   +0x08: syscall_nr  (uint32_t)
//   +0x10: arg0        (uint64_t fd)
//   +0x20: arg2        (uint64_t count)
//   +0x38: retval      (uint64_t)
//   +0x40: data        (string buffer)

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	sendmsg_live
	.p2align 8
	.type	sendmsg_live,@function

sendmsg_live:
	s_waitcnt lgkmcnt(0)

	// Save mailbox base in v[4:5].
	v_mov_b32_e32 v4, s0
	v_mov_b32_e32 v5, s1

	// data[0..15] = "hello from CU\n\0" at mailbox + 0x40.
	v_add_u32_e32 v0, 0x40, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 0x6c6c6568
	flat_store_dword v[0:1], v2

	v_add_u32_e32 v0, 0x44, v4
	v_mov_b32_e32 v2, 0x7266206f
	flat_store_dword v[0:1], v2

	v_add_u32_e32 v0, 0x48, v4
	v_mov_b32_e32 v2, 0x43206d6f
	flat_store_dword v[0:1], v2

	v_add_u32_e32 v0, 0x4c, v4
	v_mov_b32_e32 v2, 0x00000a55
	flat_store_dword v[0:1], v2

	// syscall_nr = 1 (write) at +0x08.
	v_add_u32_e32 v0, 0x08, v4
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2

	// arg0 = 1 (stdout) at +0x10.
	v_add_u32_e32 v0, 0x10, v4
	v_mov_b32_e32 v2, 1
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg2 = 14 at +0x20.
	v_add_u32_e32 v0, 0x20, v4
	v_mov_b32_e32 v2, 14
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// Publish syscall request after all fields are visible.
	s_waitcnt vmcnt(0)
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)

	// Wake/notify the host without terminating this wavefront.
	s_sendmsg sendmsg(MSG_INTERRUPT)

.Lwait_retval:
	buffer_wbinvl1_vol
	flat_load_dword v2, v[4:5]
	s_waitcnt vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz .Lwait_retval

	// Mark normal exit and terminate. The CPU waits for the final fence here.
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 3
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_endpgm

.Lend_sendmsg_live:
	.size sendmsg_live, .Lend_sendmsg_live - sendmsg_live
