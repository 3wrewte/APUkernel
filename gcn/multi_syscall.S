//
// multi_syscall.S — prove multiple syscalls from one live wavefront
//
// s[0:1] receives mailbox BO flat address via COMPUTE_USER_DATA_0/1.
// The shader calls write(stdout, msg, len) three times with different
// messages, then exits. Register state and PC survive across all
// s_sendmsg / trap-return cycles.
//
// Mailbox layout (matches heteroken.c ioctl):
//   +0x00: state       (1=syscall_pending, 3=exit)
//   +0x08: syscall_nr  (1=write)
//   +0x10: arg0        (fd=1 stdout)
//   +0x20: arg2        (count)
//   +0x40: data[]      (string buffer)

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	multi_syscall
	.p2align 8
	.type	multi_syscall,@function

// Helper macro: write a short string to mailbox data area at +0x40
// then request write() syscall and wait for CPU to clear state.
// Input: immediate label string loaded inline
// Clobbers: v0-v7, s[0:1] preserved in v[4:5]
.macro DO_WRITE str_lo, str_hi, len
	// Write 8 bytes of string data to mailbox + 0x40
	v_add_u32_e32 v0, 0x40, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, \str_lo
	flat_store_dword v[0:1], v2
	v_add_u32_e32 v0, 0x44, v4
	v_mov_b32_e32 v2, \str_hi
	flat_store_dword v[0:1], v2

	// syscall_nr = 1 (SYS_write)
	v_add_u32_e32 v0, 0x08, v4
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2

	// arg0 = 1 (stdout fd)
	v_add_u32_e32 v0, 0x10, v4
	v_mov_b32_e32 v2, 1
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// arg2 = len
	v_add_u32_e32 v0, 0x20, v4
	v_mov_b32_e32 v2, \len
	v_mov_b32_e32 v3, 0
	flat_store_dwordx2 v[0:1], v[2:3]

	// Publish: state = 1
	s_waitcnt vmcnt(0)
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)

	// Notify CPU
	s_sendmsg sendmsg(MSG_INTERRUPT)

	// Wait for CPU to clear state
1:
	buffer_wbinvl1_vol
	flat_load_dword v2, v[4:5]
	s_waitcnt vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz 1b
.endm

multi_syscall:
	s_waitcnt lgkmcnt(0)
	v_mov_b32_e32 v4, s0
	v_mov_b32_e32 v5, s1

	// Syscall 0: "call 0\n\n" = 0x6c6c6163, 0x0a3020 + padding
	DO_WRITE 0x6c6c6163, 0x000a3020, 8

	// Syscall 1: "call 1\n\n"
	DO_WRITE 0x6c6c6163, 0x000a3120, 8

	// Syscall 2: "call 2\n\n"
	DO_WRITE 0x6c6c6163, 0x000a3220, 8

	// Exit
	v_mov_b32_e32 v0, v4
	v_mov_b32_e32 v1, v5
	v_mov_b32_e32 v2, 3
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_endpgm

.Lend_multi_syscall:
	.size multi_syscall, .Lend_multi_syscall - multi_syscall
