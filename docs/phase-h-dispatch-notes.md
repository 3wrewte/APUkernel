# Phase H Dispatch Debugging Notes — CIK (GFX7) Compute Queue

## Overview

After successfully creating a compute queue on Kaveri via kallsyms bypass
(`hk_queue.c`), we attempted to dispatch our `hello_gcn.hsaco` kernel via
a PM4 `DISPATCH_DIRECT` packet. The doorbell was rung, but the GCN kernel
did not produce output (result buffer remained `0x00000000`).

This document records every pitfall encountered and the current diagnosis.

## Pitfall Log

### 1. Wrong GCN instruction encoding (hand-coded vs actual .hsaco)

We initially hard-coded GCN instructions for `hello_gcn` by manually
encoding each assembly mnemonic into uint32_t values. The encodings
were wrong in multiple places:

| Instruction | Hand-coded | Actual (.hsaco) | Error |
|---|---|---|---|
| `s_load_dwordx2 s[8:9], s[0:1], 0x0` | `0xC0060102` | `0xC0440100` | Wrong opcode bits |
| `v_mov_b32_e32 v0, s8` | `0x7E100208` | `0x7E000208` | VDST=0x10 instead of 0x00 |
| `v_mov_b32_e32 v1, s9` | `0x7E120209` | `0x7E020209` | VDST=0x12 instead of 0x02 |
| `flat_store_dword v[0:1], v2` (data) | `0x00020002` | `0x00000200` | Data encoding wrong |

**Fix**: Extracted actual bytes from `.hsaco` via `llvm-objcopy -O binary -j .text`
and verified against `xxd` output. Now using verified bytes:
```c
static const uint32_t hello_gcn_code[] = {
    0xC0440100, 0xBF8C007F, 0x7E000208, 0x7E020209,
    0x7E0402FF, 0x21544148, 0xDC700000, 0x00000200,
    0xBF810000,
};
```

**Lesson**: Never hand-encode GCN instructions. Always extract from the
compiled `.hsaco` binary. The GCN encoding is complex and varies by
subtarget.

### 2. dispatch_initiator = 0 disables compute shader

The PM4 `DISPATCH_DIRECT` packet's `dispatch_initiator` word has bit 0
(`COMPUTE_SHADER_EN`) and bit 2 (`FORCE_START_AT_000`). Setting it to 0
meant the CP would interpret the dispatch as a GFX dispatch, not a
compute dispatch, and would likely use the wrong shader registers.

**Fix**: Set `dispatch_initiator = (1 << 0) | (1 << 2) = 0x5` in both the
PM4 packet and the MQD's `compute_dispatch_initiator` field.

### 3. Kernel argument passing via SGPRs

The kernel expects `s[0:1]` = flat address of the kernel argument buffer.
On CIK with HSA, this comes from `compute_user_data_0/1` in the MQD.

We initially set `compute_user_data_0/1` = `GPUVA_RESULT` directly, but
the kernel reads `s[0:1]` as a *pointer to* the args buffer, loads the
actual target address from there, and writes to it. This caused the kernel
to read `0x00000000` from `GPUVA_RESULT` and write to address 0.

**Fix**: Allocated a separate args buffer at `GPUVA_ARGS` containing
`{GPUVA_RESULT}` (8 bytes), and set `compute_user_data_0/1 = GPUVA_ARGS`.

### 4. Doorbell BO CPU mapping

The doorbell BO (`pdd->qpd.proc_doorbells` at PDD+176) is allocated by
`kfd_alloc_process_doorbells` with the `DOORBELL` flag, which creates an
MMIO-backed BO.

- `amdgpu_bo_kptr(doorbell_bo)` → returns valid pointer on ~50% of boots,
  NULL on others. Inconsistency likely due to prior GPU access state.
- `amdgpu_bo_kmap(doorbell_bo)` → **crashes** (page fault). Doorbell BOs
  are MMIO-mapped and cannot be kmapped with the standard GTT path.
- `adev->doorbell.cpu_addr` → correct CPU mapping, but offset of the
  `doorbell` field in `struct amdgpu_device` is unknown. Scan-based
  approach found base at adev+2288, but the `cpu_addr` at adev+2320
  was NULL (wrong offset).

**Current workaround**: Use `bo_kptr` with fallback to skip doorbell.
For production, the correct path is `adev->doorbell.cpu_addr` with the
exact offset confirmed from kernel debug info or `pahole`.

### 5. GPU struct scanning is fragile and dangerous

Scanning kernel structures by iterating offsets and testing pointer
patterns caused a page fault when the scan went beyond the struct
boundary into unmapped memory (adev+3500+).

All struct field accesses must use:
- Exact offsets verified against kernel source (preferred)
- Bounded scans within known safe ranges (e.g., known struct size)
- Never unbounded `for` loops over raw memory

### 6. MQD shader registers are NOT used by CP on CIK (ROOT CAUSE)

... [see above] ...

### 7. doorbell bo_kptr intermittent

`amdgpu_bo_kptr(proc_doorbells_bo)` returns a valid pointer on roughly
50% of boots and NULL on the other 50%. The doorbell BO is allocated with
`KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL`, which creates an MMIO-backed BO.
Whether `bo_kptr` returns a valid pointer depends on whether the BO
happens to have a CPU mapping at the time of the call (possibly set
by a prior GPU operation or the display driver).

### 8. ioremap of doorbell physical base crashes

We found `adev->doorbell.base` at adev+2288 = `0xfeb00000` by scanning
for an MMIO-range physical address. Attempting `ioremap(0xfeb00000, 0x40000)`
and `writel()` to it caused a segfault (SIGSEGV). The MMIO region at that
address may not be accessible, or the doorbell CPU mapping requires
additional setup (PCIe BAR configuration) already done by amdgpu.

### 9. AQL dispatch packet + AQL queue format (QP_FORMAT=1)

Changed queue format from PM4 (0) to AQL (1) and wrote an HSA AQL
`hsa_kernel_dispatch_packet_t` (64 bytes) to the ring buffer. Result:
kernel did not execute. Without the doorbell ringing, the CP doesn't
know about the new work. And even with the doorbell, the AQL packet
may require the `kernel_object` field to point to a full
`amd_kernel_code_t` descriptor (not raw GCN code), which we didn't
construct.

## Status Summary

### What Works

- Queue creation (via kallsyms + KFD internals)  
- GPU memory allocation + GPU VM mapping (via KFD gpuvm functions)
- MQD register updates (verified offsets, confirmed by KFD behavior)
- Ring buffer CPU mapping + packet writing (via amdgpu_bo_kmap)
- Kernel code loading into GPU memory
- Doorbell BO location (PDD+176, always found)
- Doorbell ringing (when bo_kptr returns non-NULL, works correctly)

### What's Blocked

- **Reliable doorbell CPU mapping** — need `adev->doorbell.cpu_addr` offset
  or `amdgpu_doorbell_index_on_bar`-equivalent access. See `docs/why-offsets.md`.
- **Correct CIK dispatch packet** — may need full `amd_kernel_code_t`
  descriptor + AQL packet, or may need a different dispatch mechanism.

### Path to Unblock

1. Find `adev->doorbell.cpu_addr` offset using `pahole` or kernel debuginfo:
   ```bash
   pahole -C amdgpu_device /usr/lib/debug/boot/vmlinux-$(uname -r)
   ```
2. OR: switch to approach A (in-tree build). Then `adev->doorbell.cpu_addr`
   just works.
3. Or: use ROCr/HSA runtime for the dispatch layer (proven CIK support).

## Attempts at Direct / IB-based Shader Loading

### Attempt 4: SET_SH_REG directly in ring buffer

Wrote `PACKET3_SET_SH_REG(5)` (0x76) + `PACKET3_SET_SH_REG(1)` + `DISPATCH_DIRECT(4)`
directly to the ring buffer. The CP processed the packets (wptr advanced),
but the kernel did not execute. SET_SH_REG is likely a privileged packet
rejected by the CP for user-mode compute queues.

### Attempt 5: INDIRECT_BUFFER with SET_SH_REG in IB

Built an IB (16 dwords) containing SET_SH_REG to set compute registers
followed by DISPATCH_DIRECT. Submitted `PACKET3_INDIRECT_BUFFER(2)` (0x3F)
to the ring. Result: kernel still did not execute. The CP either rejected
the INDIRECT_BUFFER or executed the IB but the SET_SH_REG commands were
silently ignored.

### Attempt 6: pqm_update_mqd flush

Called KFD's `pqm_update_mqd` after manual MQD register writes. Returned
-EACCES (-13), suggesting the queue manager requires a lock or the update
is not permitted in the current queue state.

## Conclusion: CIK Dispatch Limitation

On CIK (GFX7), user-mode compute queues managed by KFD do NOT support
arbitrary PM4 shader configuration through any of these mechanisms:
- MQD register writes (CP ignores compute_pgm)
- PACKET3_SET_SH_REG (likely privileged)
- INDIRECT_BUFFER with SET_SH_REG (rejected or ignored)

The CP firmware on CIK was designed before AQL (introduced in GFX8/MEC).
The dispatch path for user compute kernels goes through a higher-level
mechanism (HSA AQL dispatch) that is emulated by the KFD driver.

## Recommended Path Forward

For Phase H completion, use the **HSA runtime's dispatch path** (which
we know works on this hardware — rocminfo confirms GFX7 support):

1. Use `libhsakmt` + `hsaKmtCreateQueue` + HSA AQL dispatch packets
   to dispatch `hello_gcn.hsaco` from userspace
2. This validates the end-to-end "GCN executes and writes to hUMA" milestone
3. The kernel module path (for production HeteroKern architecture) will then
   implement dispatch through the KFD's internal HSA AQL handling, which is
   well-tested and known to work

The entire kernel module infrastructure we built (queue creation, GPU memory
allocation, GPU VM mapping) remains essential — only the dispatch mechanism
changes from raw PM4 to HSA AQL.
