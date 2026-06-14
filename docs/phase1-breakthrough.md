# Phase 1 Breakthrough: CP Dispatch on GFX9 (Raven Ridge / 2200G)

**Date**: 2025-06-14 (updated)
**Milestone**: Full PM4 command stream processing; compute dispatch registers programmed via SET_SH_REG

## Summary

After extensive debugging, we achieved:
1. **PM4 NOP dispatch**: CP processed NOP packet, rptr 0→1 (CP ALIVE!)
2. **Full PM4 command stream**: CP processed 32 dwords of SET_SH_REG + DISPATCH_DIRECT, rptr=WPTR
3. **Compute registers programmed**: COMPUTE_PGM_LO, RSRC1/RSRC2, USER_DATA, VMID, DIM, NUM_THREAD all correctly set via PM4 SET_SH_REG packets
4. **GCN code loaded into GPU**: Shader binary uploaded to GPU memory at GPU VA 0x2400000

**Remaining blocker**: The hello_gcn shader was assembled for gfx700 — its instruction encodings are incompatible with gfx902. Need to rebuild for gfx902.

## Timeline of Key Discoveries

### 1. kfd_queue_acquire_buffers fails (-EINVAL)

The in-tree kernel module bypasses the KFD ioctl layer but still needs
`kfd_queue_acquire_buffers()` to register buffers for GPU access. This failed
because the CWSR (Context Save/Restore) buffer was only 12KB but the hardware
requires ~2.7MB (cwsr_size=0x2a9000 + debug_memory_size=0x2800).

**Fix**: Calculate `total_cwsr_size` from topology before allocating:
```c
total_cwsr = (topo_dev->node_props.cwsr_size +
              topo_dev->node_props.debug_memory_size) *
             NUM_XCC(pdd->dev->xcc_mask);
total_cwsr = ALIGN(total_cwsr, PAGE_SIZE);
```

Also critical: `kfd_queue_buffer_get()` requires **exact** size match between
the GPU VM mapping and the expected size. An over-allocation (e.g., 3MB mapping
for a 2.7MB expected size) causes the strict equality check to fail.

### 2. VMID not allocated (HWS vs NO_HWS)

In HWS (Hardware Scheduler) mode, VMID allocation is dynamic and handled by
the CP firmware. Our module's queue got `vmid=0`, which is reserved.

**Fix**: Switch to `amdgpu.sched_policy=2` (KFD_SCHED_POLICY_NO_HWS) for
direct HQD programming. This gives explicit VMID allocation via `allocate_vmid()`.

### 3. SH_MEM_BASES = 0 (CP cannot translate GPU VAs)

Even after `create_queue` set `qpd->sh_mem_bases = 0x10002` in software,
the hardware register read `SH_MEM_BASES = 0x0`. This was because we read
the register through `acquire_queue(pipe, queue)` which selects the wrong
VMID context — SH_MEM registers are per-VMID, accessed via SRBM VMID selection.

The value `0x10002` means: shared_base=1 (LDS base >> 48), private_base=2
(scratch base >> 48). This is correct for V9 GPUs.

### 4. Doorbell / WPTR polling not reaching CP

After ring buffer + doorbell submission, CP did not process the packet.
`CP_HQD_PQ_WPTR_LO` stayed at 0 even after `WDOORBELL32(4096, 1)`.

**Root cause**: The WPTR_POLL_ADDR is set to the GPU VA of the wptr buffer
(0x2000000). The CP reads this address through IOMMU/ATC translation using
the queue's PASID. On our APU, this translation path appears broken — the CP
cannot read the wptr memory via polling, and doorbell MMIO may also not reach
the CP correctly.

**Workaround**: Directly write `CP_HQD_PQ_WPTR_LO` register to set the
write pointer, bypassing both doorbell and WPTR polling:

```c
kgd_gfx_v9_acquire_queue(adev, q->pipe, q->queue, 0);
WREG32_SOC15_RLC(GC, 0, mmCP_HQD_PQ_WPTR_LO, wptr_value);
WREG32_SOC15_RLC(GC, 0, mmCP_HQD_PQ_WPTR_HI, 0);
kgd_gfx_v9_release_queue(adev, 0);
```

**Result**: rptr 0→1 — **CP ALIVE!**

This doorbell/WPTR polling issue was also the blocker on CIK (Kaveri). After
hundreds of attempts across two platforms, we accept that the normal doorbell
path doesn't work from kernel context and adopt the direct WPTR register write
as the dispatch mechanism.

### 5. AQL format queues: WPTR slot-based counting

For AQL-format queues, the MQD sets:
- `SLOT_BASED_WPTR=2`: WPTR counts in 64-byte slots (not dwords)
- `NO_UPDATE_RPTR=1`: CP does not write back rptr
- `CP_HQD_AQL_CONTROL.CONTROL0=1`: enables AQL packet parsing
- `wptr_shift=4` (passed to hqd_load but unused on GFX9)

AQL dispatch failed with rptr stuck at 0. Root cause unclear — may be related
to the same WPTR polling issue. Switched to PM4 format for more control.

### 6. PM4 type 3 packet format (critical bug fixes)

**Bug 1**: Used wrong `PM4_PKT_TYPE3` macro with incorrect bit layout.
The AMD `PACKET3(op, n)` macro puts opcode at bits [15:8], NOT bits [7:0]:
```c
#define PACKET3(op, n)  ((3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16))
```
For compute queues, use `PACKET3_COMPUTE(op, n)` which sets bit 1.

**Bug 2**: The `n` parameter count semantics: `n = data_word_count - 1`.
- NOP with 1 data word: `PACKET3_COMPUTE(PACKET3_NOP, 0)` ← NOT 1!
- SET_SH_REG with offset + 2 register values = 3 data words: `PACKET3_COMPUTE(PACKET3_SET_SH_REG, 2)`
- DISPATCH_DIRECT with 4 data words: `PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3)`

Getting this wrong causes the CP to misparse subsequent packets.

### 7. SET_SH_REG register offset calculation on GFX9

On GFX9 (SOC15), register offsets use `SOC15_REG_OFFSET()` which includes
an IP-block base offset. The SH register offset for PM4 SET_SH_REG is:

```c
sh_off = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO) - PACKET3_SET_SH_REG_START;
```

Where:
- `SOC15_REG_OFFSET(GC, 0, mmXXX)` = `adev->reg_offset[GC_HWIP][0][BASE_IDX] + mmXXX`
- GC IP base offset = 0x2000 (confirmed from `adev->reg_offset[GC_HWIP][0][0]`)
- `PACKET3_SET_SH_REG_START = 0x2C00`

So for `mmCOMPUTE_PGM_LO = 0x0e0c`:
`sh_off = 0x2000 + 0x0e0c - 0x2c00 = 0x20c`

The offset is in **byte units**, not dword units. This matches the GFX8 convention
(mmCOMPUTE_PGM_LO = 0x2e0c = 0x2c00 + 0x20c).

**Key GFX9 compute register SH offsets**:
```
Register              MMIO     SH offset
COMPUTE_DISPATCH_INITIATOR  0x0e00   0x200
COMPUTE_DIM_X               0x0e01   0x201
COMPUTE_DIM_Y               0x0e02   0x202
COMPUTE_DIM_Z               0x0e03   0x203
COMPUTE_START_X             0x0e04   0x204
COMPUTE_NUM_THREAD_X        0x0e07   0x207
COMPUTE_NUM_THREAD_Y        0x0e08   0x208
COMPUTE_NUM_THREAD_Z        0x0e09   0x209
COMPUTE_PGM_LO              0x0e0c   0x20c
COMPUTE_PGM_HI              0x0e0d   0x20d
COMPUTE_PGM_RSRC1           0x0e12   0x212
COMPUTE_PGM_RSRC2           0x0e13   0x213
COMPUTE_VMID                0x0e14   0x214
COMPUTE_RESOURCE_LIMITS     0x0e15   0x215
COMPUTE_USER_DATA_0         0x0e40   0x240
COMPUTE_USER_DATA_1         0x0e41   0x241
```

### 8. Compute registers CANNOT be programmed via MMIO

Writing compute registers (COMPUTE_PGM_LO, etc.) via `WREG32_SOC15()` through
`acquire_queue()` has **no effect** — all registers read back as 0. Compute
registers are dispatch-context registers managed by the CP and must be
programmed via PM4 SET_SH_REG packets in the command stream.

This was confirmed by reading back all compute registers after MMIO writes
vs after PM4 SET_SH_REG — only the PM4 path actually sets the registers.

### 9. COMPUTE_VMID must be set explicitly

After SET_SH_REG + DISPATCH_DIRECT, `COMPUTE_VMID` reads as 0x0 even though
`CP_HQD_VMID = 0x808` (VMID=8, IB_VMID=8). The CP does NOT automatically
inherit the queue's VMID for compute shader memory accesses. Must include:
```c
/* SET_SH_REG: COMPUTE_VMID */
ring[idx++] = PACKET3_COMPUTE(PACKET3_SET_SH_REG, 1);
ring[idx++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_VMID) - PACKET3_SET_SH_REG_START;
ring[idx++] = vmid;  /* = 8 */
```

### 10. GCN shader instruction encoding incompatible between gfx700 and gfx902

The `hello_gcn.hsaco` was assembled for gfx700. The instruction encodings
differ:
- `s_load_dwordx2 s[8:9], s[0:1], 0x0` encoded as `0xc0440100` (gfx700)
- Correct encoding for gfx902: `0x80600008`

This is the **current blocker** — the shader code in GPU memory has wrong
instruction encodings, causing the GPU to execute invalid/faulting code.

## Working PM4 Command Stream

The following PM4 command stream was successfully processed by the CP
(rptr=0x20=32, matching WPTR):

```
[0]  NOP count=0              → verify CP alive
[2]  SET_SH_REG count=2       → COMPUTE_PGM_LO/HI = kernel code address
[6]  SET_SH_REG count=2       → COMPUTE_PGM_RSRC1/RSRC2
[10] SET_SH_REG count=2       → COMPUTE_USER_DATA_0/1 = kernarg pointer
[14] SET_SH_REG count=3       → COMPUTE_DIM_X/Y/Z = 64,1,1
[19] SET_SH_REG count=3       → COMPUTE_NUM_THREAD_X/Y/Z = 64,1,1
[24] SET_SH_REG count=1       → COMPUTE_VMID = 8
[27] DISPATCH_DIRECT count=3  → launch compute shader
```

Post-dispatch register verification:
```
COMPUTE_PGM_LO  = 0x24000 (GPUVA_CODE >> 8 = 0x2400000 >> 8)
COMPUTE_PGM_HI  = 0x0
COMPUTE_PGM_RSRC1 = 0x200040 (SGPRS=1→20 SGPR, DX10_CLAMP=1)
COMPUTE_PGM_RSRC2 = 0x4 (USER_SGPR=2)
COMPUTE_VMID    = 0x8
COMPUTE_USER_DATA_0 = 0x2500000 (kernarg GPU VA)
COMPUTE_USER_DATA_1 = 0x0
```

## Current Dispatch Method (Hack WPTR)

For kernel-initiated dispatches:

1. Write PM4 packets to ring buffer (CPU-mapped via `amdgpu_bo_kmap`)
2. Update the wptr memory value
3. Ring doorbell + write WPTR directly to `CP_HQD_PQ_WPTR_LO` register
4. CP eventually processes the packets (may take 1-3 seconds)

**Important**: The CP does NOT respond immediately. After hack WPTR, it can
take up to 3 seconds for the CP to process all packets (rptr to catch up to
wptr). This was observed consistently: NOP rptr=0 at 500ms, rptr=full at 3s.
The delay may be related to the CP's internal polling cadence.

## GPU Memory Layout

```
GPU VA          Size    Purpose
0x1000000       64KB    Ring buffer (PM4 packets)
0x2000000       4KB     WPTR (write pointer)
0x2001000       4KB     RPTR (read pointer)
0x2002000       ~2.7MB  CWSR (Context Save/Restore Area)
0x2400000       4KB     Code (GCN kernel instructions)
0x2500000       4KB     Kernarg (kernel argument buffer)
0x2600000       4KB     Result (output buffer)
```

All buffers allocated via `amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu()` with
GTT + WRITABLE + PUBLIC flags. Code buffer also has EXECUTABLE flag.
Result and kernarg buffers use WRITABLE without EXECUTABLE.

## Next Steps

1. **Rebuild hello_gcn for gfx902**: The shader instruction encodings must
   match the target ISA. Options:
   - Cross-compile on dev machine (192.168.1.8) with `clang --target=amdgcn-amd-amdhsa -mcpu=gfx902`
   - Manually encode the 8 instructions for gfx902 in the kernel module
   - Use the dev machine's ROCm toolchain to assemble hello_gcn.S

2. **Verify shader execution**: After correct gfx902 encoding, check:
   - Result buffer contains magic 0x21544148 ("HAT!")
   - GRBM_STATUS shows no wave errors
   - SQ_STATUS is clean

3. **P2: Completion signal**: GCN kernel writes signal, CPU receives IH interrupt

4. **Clean up heteroken.c**: Make hack WPTR the standard dispatch path,
   remove debug dumps, add proper error handling
