//
// mailbox.S — GCN-side mailbox communication primitives for HeteroKern
//
// Provides low-level GCN assembly helpers for communicating with
// the x86 kernel via the shared mailbox protocol.  All functions
// receive the mailbox base address as a 64-bit flat pointer in
// v[0:1] and use v_add_i32_e32 for small offset arithmetic
// (small offsets never overflow the low 32 bits on page-aligned
// pointers, so carry propagation is unnecessary).
//
// Mailbox struct layout (128 bytes, 128-byte aligned):
//   +0x00: state       (uint32_t)
//   +0x04: syscall_nr  (uint32_t)
//   +0x08: args[0..5]  (uint64_t * 6 = 48 bytes)
//   +0x38: retval      (uint64_t)
//   +0x40: error_code  (uint64_t)
//   +0x48: fault_addr  (uint64_t)
//   +0x50: fault_type  (uint32_t)
//   +0x54: reserved[44]

	.amdgcn_target "amdgcn-amd-amdhsa--gfx700"

//
// mailbox_signal(mailbox_ptr)
//   v[0:1] = flat address of mailbox base
// Writes state=1 at offset 0.
//
	.globl	mailbox_signal
	.p2align 8
	.type	mailbox_signal,@function

mailbox_signal:
	v_mov_b32_e32 v2, 1
	flat_store_dword v[0:1], v2
	s_endpgm
.Lend_mb_signal:
	.size	mailbox_signal, .Lend_mb_signal - mailbox_signal

//
// mailbox_set_state(mailbox_ptr, state_value)
//   v[0:1] = mailbox address
//   v2     = state value
//
	.globl	mailbox_set_state
	.p2align 8
	.type	mailbox_set_state,@function

mailbox_set_state:
	flat_store_dword v[0:1], v2
	s_endpgm
.Lend_mb_set_state:
	.size	mailbox_set_state, .Lend_mb_set_state - mailbox_set_state

//
// mailbox_read_retval(mailbox_ptr) -> retval (in v[0:1])
// Reads 64-bit retval from mailbox offset 0x38.
//
	.globl	mailbox_read_retval
	.p2align 8
	.type	mailbox_read_retval,@function

mailbox_read_retval:
	v_add_i32_e32 v0, vcc, 0x38, v0
	flat_load_dwordx2 v[2:3], v[0:1]
	s_waitcnt     vmcnt(0)
	v_mov_b32_e32 v0, v2
	v_mov_b32_e32 v1, v3
	s_endpgm
.Lend_mb_read_retval:
	.size	mailbox_read_retval, .Lend_mb_read_retval - mailbox_read_retval

//
// mailbox_poll_until_zero(mailbox_ptr)
// Busy-waits until state field (offset 0) becomes 0.
//
	.globl	mailbox_poll_until_zero
	.p2align 8
	.type	mailbox_poll_until_zero,@function

mailbox_poll_until_zero:
.Lpoll_loop:
	flat_load_dword v2, v[0:1]
	s_waitcnt     vmcnt(0)
	v_cmp_eq_u32_e32 vcc, 0, v2
	s_cbranch_vccz .Lpoll_loop
	s_endpgm
.Lend_mb_poll:
	.size	mailbox_poll_until_zero, .Lend_mb_poll - mailbox_poll_until_zero

//
// mailbox_write_syscall(mailbox_ptr, nr)
//   v[0:1] = mailbox address
//   v2     = syscall number
// Writes syscall_nr at offset 4, then sets state=1.
//
	.globl	mailbox_write_syscall
	.p2align 8
	.type	mailbox_write_syscall,@function

mailbox_write_syscall:
	v_mov_b32_e32 v3, v0    // save base low
	v_mov_b32_e32 v4, v1    // save base high

	// syscall_nr at offset 4
	v_add_i32_e32 v0, vcc, 4, v0
	flat_store_dword v[0:1], v2

	// state at offset 0
	v_mov_b32_e32 v2, 1
	flat_store_dword v[3:4], v2
	s_endpgm
.Lend_mb_write_syscall:
	.size	mailbox_write_syscall, .Lend_mb_write_syscall - mailbox_write_syscall

//
// mailbox_write_args(mailbox_ptr)
// Writes 6 syscall arguments (args[0..5]) at offsets
// 0x08, 0x10, 0x18, 0x20, 0x28, 0x30.
//
//   v[0:1]   = mailbox address
//   v[4:5]   = arg0 (64-bit)
//   v[6:7]   = arg1
//   v[8:9]   = arg2
//   v[10:11] = arg3
//   v[12:13] = arg4
//   v[14:15] = arg5
//
	.globl	mailbox_write_args
	.p2align 8
	.type	mailbox_write_args,@function

mailbox_write_args:
	v_mov_b32_e32  v2, v0          // save base lo
	v_mov_b32_e32  v3, v1          // save base hi

	v_add_i32_e32  v0, vcc, 0x08, v2
	flat_store_dwordx2 v[0:1], v[4:5]
	v_add_i32_e32  v0, vcc, 0x10, v2
	flat_store_dwordx2 v[0:1], v[6:7]
	v_add_i32_e32  v0, vcc, 0x18, v2
	flat_store_dwordx2 v[0:1], v[8:9]
	v_add_i32_e32  v0, vcc, 0x20, v2
	flat_store_dwordx2 v[0:1], v[10:11]
	v_add_i32_e32  v0, vcc, 0x28, v2
	flat_store_dwordx2 v[0:1], v[12:13]
	v_add_i32_e32  v0, vcc, 0x30, v2
	flat_store_dwordx2 v[0:1], v[14:15]

	s_waitcnt      vmcnt(0)
	s_endpgm
.Lend_mb_write_args:
	.size	mailbox_write_args, .Lend_mb_write_args - mailbox_write_args

//
// mailbox_report_fault(mailbox_ptr, fault_addr, fault_type)
//   v[0:1] = mailbox address
//   v[2:3] = fault address (64-bit flat)
//   v4     = fault type (0=read, 1=write, 2=exec)
//
	.globl	mailbox_report_fault
	.p2align 8
	.type	mailbox_report_fault,@function

mailbox_report_fault:
	v_mov_b32_e32  v5, v0          // save base lo
	v_mov_b32_e32  v6, v1          // save base hi

	// fault_addr at offset 0x48
	v_add_i32_e32  v0, vcc, 0x48, v5
	flat_store_dwordx2 v[0:1], v[2:3]

	// fault_type at offset 0x50
	v_add_i32_e32  v0, vcc, 0x50, v5
	flat_store_dword v[0:1], v4

	s_waitcnt      vmcnt(0)

	// state = 2 at offset 0
	v_mov_b32_e32  v2, 2
	flat_store_dword v[5:6], v2
	s_endpgm
.Lend_mb_report_fault:
	.size	mailbox_report_fault, .Lend_mb_report_fault - mailbox_report_fault
