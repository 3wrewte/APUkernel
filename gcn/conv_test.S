//
// conv_test.S — 3x3 edge detection convolution on grayscale image
//
// Kernel: [-1,-1,-1, -1,8,-1, -1,-1,-1]
//
// Each thread processes one output pixel.
// Dispatch: (ceil(out_pixels/64), 1, 1) workgroups of 64 threads.
// TGID_X_EN=1 → s2 = workgroup_id_x
//
// s[0:1] = mailbox base
// s2     = workgroup_id_x
// v0     = thread local ID (0..63)
//
// Mailbox fields:
//   +0xC0: input_addr   (u64)
//   +0xC8: output_addr  (u64)
//   +0xD0: width        (u32)
//   +0xD4: height       (u32)
//
// No syscall: pure compute. CPU waits for fence completion.

	.amdgcn_target "amdgcn-amd-amdhsa--gfx902"
	.text
	.globl	conv_test
	.p2align 8
	.type	conv_test,@function

conv_test:
	s_waitcnt lgkmcnt(0)

	// v14 = local thread id (save)
	v_mov_b32_e32 v14, v0

	// v[4:5] = mailbox base
	v_mov_b32_e32 v4, s0
	v_mov_b32_e32 v5, s1

	// Load width (u32) from mailbox+0xD0 → v6
	v_add_u32_e32 v0, 0xD0, v4
	v_mov_b32_e32 v1, v5
	flat_load_dword v6, v[0:1]
	s_waitcnt vmcnt(0)

	// Load input_addr from mailbox+0xC0 → v[8:9]
	v_add_u32_e32 v0, 0xC0, v4
	v_mov_b32_e32 v1, v5
	flat_load_dwordx2 v[8:9], v[0:1]
	s_waitcnt vmcnt(0)

	// Load output_addr from mailbox+0xC8 → v[10:11]
	v_add_u32_e32 v0, 0xC8, v4
	v_mov_b32_e32 v1, v5
	flat_load_dwordx2 v[10:11], v[0:1]
	s_waitcnt vmcnt(0)

	// v12 = out_w = width - 2
	v_add_u32_e32 v12, 0xFFFFFFFE, v6

	// global_id = s2 * 64 + v14
	v_mov_b32_e32 v0, s2
	v_lshlrev_b32_e32 v0, 6, v0
	v_add_u32_e32 v15, v0, v14

	// Compute pixel_x and pixel_y from global_id via masked subtraction loop.
	// All threads execute same iteration count; inactive threads are masked.
	v_mov_b32_e32 v0, 0                 // v0 = pixel_y
	v_mov_b32_e32 v1, v15               // v1 = remaining

1:
	v_cmp_ge_u32 vcc, v1, v12           // remaining >= out_w?
	v_cndmask_b32 v2, 0, v12, vcc       // v2 = out_w if true, 0 if not
	v_sub_u32_e32 v1, v1, v2            // remaining -= v2
	v_cndmask_b32 v2, 0, 1, vcc         // v2 = 1 if true, 0 if not
	v_add_u32_e32 v0, v0, v2            // pixel_y += v2
	s_cbranch_vccnz 1b                  // loop while any thread active
2:
	// v0 = pixel_y, v1 = pixel_x

	// Compute 3x3 convolution
	// sum = -input[y*w+x] - input[y*w+(x+1)] - input[y*w+(x+2)]
	//       -input[(y+1)*w+x] + 8*input[(y+1)*w+(x+1)] - input[(y+1)*w+(x+2)]
	//       -input[(y+2)*w+x] - input[(y+2)*w+(x+1)] - input[(y+2)*w+(x+2)]

	// Helper: load input[py * width + px]
	// py in v0 (or computed), px in v1 (or computed)
	// result in v3

	// Row offsets: y*w, (y+1)*w, (y+2)*w
	v_mul_lo_u32 v16, v0, v6            // v16 = y * width
	v_add_u32_e32 v17, v6, v16          // v17 = (y+1) * width
	v_add_u32_e32 v18, v6, v17          // v18 = (y+2) * width

	// sum = 0
	v_mov_b32_e32 v3, 0

	// dy=0, dx=0: -input[y*w + x]
	v_add_u32_e32 v19, v1, v16
	v_lshlrev_b32_e32 v19, 2, v19       // byte offset
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=0, dx=1: -input[y*w + (x+1)]
	v_add_u32_e32 v19, 1, v1
	v_add_u32_e32 v19, v19, v16
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=0, dx=2: -input[y*w + (x+2)]
	v_add_u32_e32 v19, 2, v1
	v_add_u32_e32 v19, v19, v16
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=1, dx=0: -input[(y+1)*w + x]
	v_add_u32_e32 v19, v1, v17
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=1, dx=1: +8*input[(y+1)*w + (x+1)]
	v_add_u32_e32 v19, 1, v1
	v_add_u32_e32 v19, v19, v17
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_lshlrev_b32_e32 v22, 3, v22       // * 8
	v_add_u32_e32 v3, v3, v22

	// dy=1, dx=2: -input[(y+1)*w + (x+2)]
	v_add_u32_e32 v19, 2, v1
	v_add_u32_e32 v19, v19, v17
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=2, dx=0: -input[(y+2)*w + x]
	v_add_u32_e32 v19, v1, v18
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=2, dx=1: -input[(y+2)*w + (x+1)]
	v_add_u32_e32 v19, 1, v1
	v_add_u32_e32 v19, v19, v18
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// dy=2, dx=2: -input[(y+2)*w + (x+2)]
	v_add_u32_e32 v19, 2, v1
	v_add_u32_e32 v19, v19, v18
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v8
	v_mov_b32_e32 v21, v9
	flat_load_dword v22, v[20:21]
	s_waitcnt vmcnt(0)
	v_sub_u32_e32 v3, v3, v22

	// Store output[pixel_y * out_w + pixel_x] = sum
	v_mul_lo_u32 v19, v0, v12
	v_add_u32_e32 v19, v1, v19
	v_lshlrev_b32_e32 v19, 2, v19
	v_add_u32_e32 v20, v19, v10
	v_mov_b32_e32 v21, v11
	flat_store_dword v[20:21], v3
	s_waitcnt vmcnt(0)

	s_endpgm

.Lend_conv_test:
	.size conv_test, .Lend_conv_test - conv_test
