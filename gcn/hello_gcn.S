//
// hello_gcn.S — minimal GCN kernel for HeteroKern Phase B
//
// Dispatched by x86 kernel module via HSA queue.
// Receives a 64-bit flat address as the first (and only) kernel argument.
// Writes the magic value 0x21544148 ("HAT!") to that address,
// then terminates cleanly.
//
// Build: clang --target=amdgcn-amd-amdhsa -mcpu=gfx902 -c -o hello_gcn.o hello_gcn.S
// Link:  ld.lld -shared hello_gcn.o -o hello_gcn.hsaco
//
// This is the primitive that the entire mailbox protocol is built on:
// prove a GCN kernel can write to a memory location the x86 side reads.

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	hello_gcn
	.p2align 8
	.type	hello_gcn,@function

hello_gcn:
	//
	// On entry (HSA ABI):
	//   s[0:1] — flat address of kernel argument buffer
	//   s[2:3] — reserved (kernel pointer for init WG)
	//   s[4:5] — dispatch packet pointer
	//   s[6:7] — queue pointer
	//   s8     — workgroup id X
	//   s9     — workgroup id Y
	//   s10    — workgroup id Z
	//   VGPRs  — zeroed (workitem id)
	//
	// First explicit argument (target_addr) is at offset 0 in arg buffer.
	// The argument buffer is a flat (hUMA) address.

	s_load_dwordx2 s[8:9], s[0:1], 0x0    // load 64-bit target address from arg[0]
	s_waitcnt     lgkmcnt(0)               // wait for scalar memory load to finish

	v_mov_b32_e32 v0, s8                   // target addr lo → v0
	v_mov_b32_e32 v1, s9                   // target addr hi → v1

	v_mov_b32_e32 v2, 0x21544148           // magic "HAT!" in little-endian

	flat_store_dword v[0:1], v2            // write magic to target address

	s_endpgm                                 // terminate wavefront
.Lfunc_end_hello_gcn:
	.size	hello_gcn, .Lfunc_end_hello_gcn - hello_gcn

//
// Second symbol: write to a hardcoded offset from the kernel args pointer.
// This is useful when the target address is known at compile time relative
// to the dispatch buffer (e.g., mailbox is always at args[1]).
//

	.globl	hello_gcn_rel
	.p2align 8
	.type	hello_gcn_rel,@function

hello_gcn_rel:
	s_load_dwordx2 s[8:9], s[0:1], 0x8    // load target address from arg[1] (offset 8)
	s_waitcnt     lgkmcnt(0)

	v_mov_b32_e32 v0, s8
	v_mov_b32_e32 v1, s9

	v_mov_b32_e32 v2, 42                   // simple integer

	flat_store_dword v[0:1], v2

	s_endpgm
.Lfunc_end_hello_gcn_rel:
	.size	hello_gcn_rel, .Lfunc_end_hello_gcn_rel - hello_gcn_rel
