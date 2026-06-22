//
// trap_handler.S — GCN trap handler skeleton for HeteroKern
//
// When a GCN wavefront encounters an exception (page fault,
// illegal instruction, address error, etc.), the hardware
// jumps to the trap handler address configured via HSA setup.
//
// For Phase B, this is a minimal skeleton — full save/restore
// comes in Phase F (context.c).

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"

	.text

//
// trap_handler — minimal hardware trap handler.
// On entry:
//   ttmp0-ttmp1: trapping PC (48-bit address)
//   ttmp2:       trap information register
//
// Simply halts the wavefront. The x86 kernel detects the halt
// via KFD trap events and handles the fault.
//
	.globl	trap_handler
	.p2align 8
	.type	trap_handler,@function

trap_handler:
	s_mov_b64     exec, 0
	s_sleep       0xffff
.Lend_th:
	.size	trap_handler, .Lend_th - trap_handler

//
// trap_handler_notify — trap handler with mailbox notification.
// On entry:
//   s[14:15] — flat address of this wavefront's mailbox
//              (set by x86 kernel at dispatch time via SGPR init)
//
// Writes trapping PC → mailbox.fault_addr,
//         MAILBOX_FAULT_TYPE_EXEC → mailbox.fault_type,
//         MAILBOX_STATE_TRAP_PENDING → mailbox.state,
// then halts.
//
	.globl	trap_handler_notify
	.p2align 8
	.type	trap_handler_notify,@function

trap_handler_notify:
	// Move mailbox address to VGPRs for flat stores
	v_mov_b32_e32 v0, s14
	v_mov_b32_e32 v1, s15

	// Save trapping PC from ttmp registers
	v_mov_b32_e32 v2, ttmp0
	v_mov_b32_e32 v3, ttmp1

	// fault_addr at mailbox offset 0x48
	v_add_u32_e32 v4, 0x48, v0
	flat_store_dwordx2 v[4:5], v[2:3]

	// fault_type = 2 (exec) at offset 0x50
	v_mov_b32_e32 v2, 2
	v_add_u32_e32 v4, 0x50, v0
	flat_store_dword v[4:5], v2

	s_waitcnt     vmcnt(0)

	// state = 2 (TRAP_PENDING) at offset 0
	v_mov_b32_e32 v2, 2
	flat_store_dword v[0:1], v2
	s_waitcnt     vmcnt(0)

	s_mov_b64     exec, 0
	s_sleep       0xffff
.Lend_thn:
	.size	trap_handler_notify, .Lend_thn - trap_handler_notify
