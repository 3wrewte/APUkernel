//
// sendmsg_exit.S — prove whether execution continues after MSG_INTERRUPT.
//
// If s_sendmsg sendmsg(MSG_INTERRUPT) returns normally, this shader writes
// mailbox.state=3 and exits. If it traps/stalls, the ioctl path times out.

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	sendmsg_exit
	.p2align 8
	.type	sendmsg_exit,@function

sendmsg_exit:
	s_waitcnt lgkmcnt(0)
	v_mov_b32_e32 v0, s0
	v_mov_b32_e32 v1, s1

	s_sendmsg sendmsg(MSG_INTERRUPT)

	v_mov_b32_e32 v2, 3
	flat_store_dword v[0:1], v2
	s_waitcnt vmcnt(0)
	s_endpgm

.Lend_sendmsg_exit:
	.size sendmsg_exit, .Lend_sendmsg_exit - sendmsg_exit
