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

Even with correct MQD register values (`compute_pgm_lo/hi`, `rsrc1/2`,
`user_data`), the GCN kernel did not execute.

**Root cause**: `kfd_mqd_manager_cik.c` never sets `compute_pgm_lo/hi` in
`init_mqd` or `__update_mqd`. These registers remain 0 (from `memset`).
The KFD driver does not configure the shader program through the MQD on CIK.

On CIK, the CP reads the shader from the MQD when processing a
`DISPATCH_DIRECT` packet. Since the MQD's shader pointer is 0, the CP
fetches instructions from GPU address 0, which contains:
```
0x0000: NOP (0x00000000 = s_nop)
0x0004: s_endpgm (0xBF810000? No — actually address 0 is typically zeros)
```
The "shader" at address 0 either does nothing or causes a GPU fault.

**Required fix**: Instead of writing `DISPATCH_DIRECT` directly, the
dispatch must use `PACKET3_INDIRECT_BUFFER` + a GPU buffer (IB) containing:
1. `SET_SH_REG` packets to configure compute shader registers
2. `DISPATCH_DIRECT` to launch

This requires:
- CIK register addresses for `COMPUTE_PGM_LO/HI`, `COMPUTE_PGM_RSRC1/2`,
  `COMPUTE_USER_DATA_0-1`, and `COMPUTE_DISPATCH_INITIATOR`
- `PACKET3_SET_SH_REG` packet format
- Knowledge of `PACKET3_INDIRECT_BUFFER` format for CIK compute queues

## Working: Complete Infrastructure

Despite the GCN execution block, the entire infrastructure pipeline works:

```
filp_open("/dev/kfd") → kfd_create_process
  → kfd_process_device_data_by_id
  → kfd_process_device_init_vm
  → kfd_bind_process_to_device
  → amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu × 7 (ring/wptr/rptr/eop/kern/result/args)
  → amdgpu_amdkfd_gpuvm_map_memory_to_gpu × 7
  → Build queue_properties (verified offsets)
  → kfd_alloc_process_doorbells
  → pqm_create_queue → QUEUE id=0
  → bo_kmap(ring_cpu) + bo_kmap(wptr_cpu)
  → Write PM4 DISPATCH_DIRECT to ring buffer
  → Update MQD registers (correct offsets, verified)
  → amdgpu_bo_kptr(doorbell_bo) → ring doorbell
  → RC=0, module loads cleanly
```

## Next Steps

1. Research `PACKET3_INDIRECT_BUFFER` and `PACKET3_SET_SH_REG` for CIK
2. Find CIK register addresses for compute shader configuration
3. Build an Indirect Buffer (IB) with shader setup + dispatch
4. Write INDIRECT_BUFFER PM4 packet to ring buffer
5. Verify magic value appears in result buffer
