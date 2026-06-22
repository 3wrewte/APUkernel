//
// trap_return.S — minimal GFX9 trap handler for MSG_INTERRUPT experiments.
//
// On trap entry, ttmp0/ttmp1 contain the return PC/status pair used by
// s_rfe_b64. This handler immediately returns to the interrupted wavefront.

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	trap_return
	.p2align 8
	.type	trap_return,@function

trap_return:
	s_mov_b64 s[0:1], ttmp[0:1]
	s_rfe_b64 s[0:1]

.Lend_trap_return:
	.size trap_return, .Lend_trap_return - trap_return
