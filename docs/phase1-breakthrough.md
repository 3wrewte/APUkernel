# Phase 1 Breakthrough: GCN Wave Dispatch on GFX9 (Raven Ridge / 2200G)

**Date**: 2026-06-15
**Milestone**: **GCN WAVEFRONT EXECUTED!** Magic value 0x21544148 written to GPU memory and read back by CPU.

## Summary

After extensive debugging across two dispatch paths, we achieved:
1. **PM4 NOP dispatch**: CP processed NOP packet, rptr 0→1 (CP ALIVE!)
2. **Full PM4 command stream**: CP processed up to 35 dwords of SET_SH_REG + DISPATCH_DIRECT, rptr=WPTR
3. **Compute registers programmed**: COMPUTE_PGM_LO, RSRC1/RSRC2, USER_DATA, VMID, DIM, NUM_THREAD all correctly set via PM4 SET_SH_REG packets (verified by post-dispatch readback)
4. **GCN shader assembled for gfx902**: Verified correct with `llvm-objdump --disassemble --mcpu=gfx902` on dev machine
5. **Wavefront executed via kernel compute ring IB**: `result=0x21544148 >>> MAGIC MATCH! <<<`

### The Two Dispatch Paths

| Aspect | KFD Queue Path (FAILED) | Kernel Compute Ring IB (WORKED) |
|--------|------------------------|--------------------------------|
| Mechanism | PACKET3_COMPUTE in KFD queue ring buffer | PACKET3 in IB, scheduled on `compute_ring[0]` |
| VMID | 8 (KFD user process VM) | 0 (kernel VM) |
| CP microengine | CPC (Compute Path Complex) | ME (Micro Engine) |
| Queue mapping | hqd_load only, no KIQ MAP_QUEUES | Permanently mapped at init |
| Result buffer | KFD GPUVA (0x2600000) | IB data area (ib.gpu_addr + offset) |
| Code path | Manual ring buffer + hack WPTR | amdgpu_ib_schedule + dma_fence_wait |

**Root cause of KFD path failure**: The CP processed all PM4 packets (rptr=WPTR), all compute registers read back correctly, DISPATCH_INITIATOR=0x1 confirmed, STATIC_THREAD_MGMT_SE0/SE1=0xffffffff, no VM faults — but SPI never created waves (`SPI_CSQ_WF_ACTIVE_STATUS=0x0`, `SPI_CSQ_WF_ACTIVE_COUNT_0=0x0`). The MEC firmware silently drops DISPATCH_DIRECT from an unmapped queue. The kernel compute ring bypasses this by using the permanently-mapped kernel ring path.

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

### 10. GCN shader instruction encoding differs between gfx700 and gfx902

The `hello_gcn.hsaco` was originally assembled for gfx700. gfx902 (GFX9) uses
different instruction encodings. Key differences identified:

| Feature | gfx700 (CIK) | gfx902 (GFX9) |
|---------|-------------|----------------|
| s_load_dwordx2 | 32-bit SMRD (1 dword) | 64-bit SMEM (2 dwords) |
| s_waitcnt | lgkmcnt only (0xBF8C007F) | vmcnt + lgkmcnt (0xBF8CC07F) |
| v_mov_b32_e32 v0, s8 | VOP2: 0x7E000208 | VOP2: 0x7E000208 (same, src=SGPR8) |

The direct shader (which uses USER_DATA as write target, eliminating
kernarg indirection) was assembled on dev machine (192.168.1.8) with:
```
clang --target=amdgcn-amd-amdhsa -mcpu=gfx902 -c hello_direct.S
ld.lld -shared hello_direct.o -o hello_direct.hsaco
```

Verified with `llvm-objdump --disassemble --mcpu=gfx902`:
```
hello_direct:
    s_waitcnt lgkmcnt(0)         // BF8CC07F
    v_mov_b32_e32 v0, s0         // 7E000200
    v_mov_b32_e32 v1, s1         // 7E020201
    v_mov_b32_e32 v2, 0x21544148 // 7E0402FF 21544148
    flat_store_dword v[0:1], v2  // DC700000 00000200
    s_endpgm                     // BF810000
```
Total: 8 dwords (32 bytes). USER_SGPR=2 → s[0:1] = COMPUTE_USER_DATA_0/1.

### 11. Kernel compute ring IB dispatch (the working path)

The gfx_v9_0.c GPR init test (`gfx_v9_0_do_edc_gpr_workarounds`) uses a
proven pattern for compute dispatch through kernel compute rings:

1. Use `adev->gfx.compute_ring[0]` (kernel-managed, permanently mapped)
2. Allocate IB via `amdgpu_ib_get(adev, NULL, total_size, AMDGPU_IB_POOL_DIRECT, &ib)`
3. Copy shader code into IB data area (ib.ptr[shader_offset:])
4. Build PM4 command stream in ib.length_dw area using `PACKET3()` (NOT PACKET3_COMPUTE)
5. SET_SH_REG for: RESOURCE_LIMITS, NUM_THREAD_X/Y/Z, PGM_RSRC1/2, STATIC_THREAD_MGMT_SE0/1, PGM_LO/HI, USER_DATA_0/1
6. DISPATCH_DIRECT with COMPUTE_SHADER_EN=1
7. EVENT_WRITE (CS_PARTIAL_FLUSH) for fence completion
8. `amdgpu_ib_schedule(kring, 1, &ib, NULL, &f)` → submit
9. `dma_fence_wait_timeout(f, false, msecs_to_jiffies(10000))` → wait
10. Read result from ib.ptr[result_offset / 4]

**Result**: `result=0x21544148 >>> MAGIC MATCH! <<<`

The result buffer is placed in the IB data area itself (ib.gpu_addr + result_offset),
so it's accessible through VMID 0's page tables (no KFD GPUVA needed).

### 12. PACKET3 vs PACKET3_COMPUTE semantics

```c
#define PACKET3(op, n)  ((3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16))
#define PACKET3_COMPUTE(op, n) (PACKET3(op, n) | 1 << 1)
```

| Usage | Packet macro | CP parser |
|-------|-------------|-----------|
| IB content on dedicated compute ring | PACKET3() | ME (Micro Engine) |
| Direct ring buffer on KFD compute queue | PACKET3_COMPUTE() | CPC (Compute Path Complex) |
| KFD compute queue (our failed attempt) | PACKET3_COMPUTE() | CPC — wave dispatch silently dropped |

The CPC path requires an explicit KIQ MAP_QUEUES step that we were missing.
The ME path is already mapped and used by the kernel's own compute ring
init path.

### 13. Why SPI never created waves on KFD queue path

Confirmed by reading `mmSPI_CSQ_WF_ACTIVE_STATUS = 0x0` and
`mmSPI_CSQ_WF_ACTIVE_COUNT_0 = 0x0` after dispatch. The CP processed
the DISPATCH_DIRECT (rptr advanced past it), compute registers were
correctly programmed, but the dispatch request never reached the SPI.

Hypothesis: MEC firmware requires the queue to be explicitly "mapped"
via a KIQ MAP_QUEUES packet before DISPATCH_DIRECT packets are forwarded
to the SPI. Without this, the MEC silently consumes the dispatch.
The kernel compute ring bypasses this because it was permanently mapped
during driver initialization via KIQ.

## Working IB Content (31 dwords)

Built in kernel module (`heteroken.c`, build #81):

```
SET_SH_REG  COMPUTE_RESOURCE_LIMITS = 0
SET_SH_REG  COMPUTE_NUM_THREAD_X/Y/Z = 64, 1, 1
SET_SH_REG  COMPUTE_PGM_RSRC1 = 0x200040, RSRC2 = 0x4 (USER_SGPR=2)
SET_SH_REG  COMPUTE_STATIC_THREAD_MGMT_SE0/SE1 = 0xFFFFFFFF
SET_SH_REG  COMPUTE_PGM_LO/HI = (ib.gpu_addr + shader_offset) >> 8
SET_SH_REG  COMPUTE_USER_DATA_0/1 = ib.gpu_addr + result_offset
DISPATCH_DIRECT  (1,1,1) x 64 threads, COMPUTE_SHADER_EN=1
EVENT_WRITE   CS_PARTIAL_FLUSH
```

Key: shader code and result buffer are BOTH in the IB data area. The shader
receives the result address directly in s[0:1] via USER_DATA_0/1, eliminating
the kernarg indirection.

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

## Phase 2: Mailbox Protocol + IH Interrupt Path (2026-06-15)

### 14. Mailbox shader for gfx902 (26 dwords)

A new shader `hello_mailbox.S` writes 5 structured dwords sequentially:
```
s_waitcnt lgkmcnt(0)                    // BF8CC07F
v_mov_b32_e32 v0, s0; v1, s1           // v[0:1] = mailbox GPU VA
v_mov_b32_e32 v2, 0x21544148            // magic → mailbox[0]
flat_store_dword; v_add_u32_e32 v0, 4   // v0 += 4
v_mov_b32 v2, 1; flat_store             // state=1 → mailbox[4]
v_mov_b32 v2, 42; flat_store            // syscall=42 → mailbox[8]
v_mov_b32 v2, 0xDEADBEEF; flat_store   // arg0 → mailbox[12]
v_mov_b32 v2, 0xCAFEBABE; flat_store   // arg1 → mailbox[16]
s_endpgm
```

Key encoding: `v_add_u32_e32 v0, 4, v0` = `0x68000084` (VOP2, inline constant 4).
Built on dev machine, `llvm-objdump` verified.

### 15. Persistent mailbox BO

`amdgpu_bo_create_kernel(adev, size, PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT, ...)`
allocates a GTT BO accessible from both CPU (via `mb_cpu` pointer) and GPU
(via `mb_gpu_addr` passed as COMPUTE_USER_DATA_0/1). This BO survives IB
lifetime — critical for async dispatch where IB is freed before result read.

### 16. IH interrupt-driven completion

Replaced `dma_fence_wait_timeout()` (polling) with `dma_fence_add_callback()`:
```
amdgpu_ib_schedule → fence created
dma_fence_add_callback(fence, &cb, hk_fence_callback)
  → callback fires from IRQ context when IB completes
  → callback reads mailbox, calls complete()
wait_for_completion()   // blocks until IRQ fires
```

Verified: callback runs in **IRQ context** (`in_irq() == true`), confirming
the full GPU→CPU interrupt path: GCN wavefront → EVENT_WRITE → IH → fence
handler → our callback.

## Phase 3: GCN Task Lifecycle (2026-06-16)

### 17. GCN task context (`struct hk_gcn_ctx`)

Per-task GPU execution context:
```c
struct hk_gcn_ctx {
    struct amdgpu_device *adev;
    struct amdgpu_bo     *mb_bo;        // persistent mailbox
    void                 *mb_cpu;        // CPU mapping
    u64                   mb_gpu_addr;   // GPU VA for shader
    struct task_struct    *task;          // owning task
    struct dma_fence_cb   fence_cb;      // IH → wake_up_process
    struct dma_fence     *fence;
};
```

### 18. Sleep/wake dispatch (`hk_dispatch_ctx`)

Replaces polling `wait_for_completion()` with task sleep:
```
set_current_state(TASK_UNINTERRUPTIBLE)
hk_dispatch_ctx(ctx, shader, size)
  → amdgpu_ib_schedule → dma_fence_add_callback(fence, &ctx->fence_cb, hk_gcn_wakeup)
  → schedule()           // task sleeps
  // ... GPU executes ...
  // IH IRQ → hk_gcn_wakeup → wake_up_process(ctx->task)
  // task resumes after schedule()
__set_current_state(TASK_RUNNING)
// read mailbox on CPU
```

This is the core GCN task lifecycle pattern — same as what clone3(CLONE_GCN)
will use, just with a kthread instead of a user task.

### 19. GCN kthread demo

`/sys/kernel/heteroken/spawn` — creates kthread `hk-gcn-0 [737]`
`/sys/kernel/heteroken/stop`  — wakes thread + kthread_stop() + cleanup

Thread lifecycle (verified 50+ iterations):
```
hk-gcn-0 [737] born
  loop:
    hk-gcn-0 [737] → GPU (sleep)
    *** IH → wake hk-gcn-0 [737] (IRQ context: yes) ***
    hk-gcn-0 [737] ← GPU (wake)
    GCN task mailbox: magic=0x21544148 state=1 syscall=42 args=[0xdeadbeef,0xcafebabe]
    GCN task would execute syscall 42
  stopping GCN task hk-gcn-0 [737]
  hk-gcn-0 [737] exiting
  GCN task stopped
```

Each iteration: ~12ms (IB submit → IH wake). Mailbox data correct every time.

## Current Architecture (Phase 3)

```
heteroken.c  (in-tree, drivers/gpu/drm/amd/amdkfd/)
├── Device discovery: hk_get_adev() — open /dev/kfd, find PDD, return adev
├── Dispatch layer:  hk_dispatch_ctx() — kernel compute ring + IB + fence callback
│   └── (swappable → KFD queue when MAP_QUEUES is solved)
├── GCN task life:   hk_gcn_thread() — sleep → GPU → IH → wake → CPU → repeat
├── Sync dispatch:   run_store — dma_fence_wait_timeout (P2 compat)
└── sysfs:           spawn / stop nodes for kthread management
```

## Next Steps

1. **P3b: clone3 hook** — add `CLONE_GCN` flag to `include/uapi/linux/sched.h`,
   hook `kernel/fork.c:copy_process()`, register `hk_clone_gcn_callback` from
   heteroken module. The kthread lifecycle already proves the pattern works.

2. **P4: CoW semantics** — verify that GCN task with dup'd mm triggers
   IOMMUv2 PPR on GPU page fault, `handle_mm_fault()` resolves it.

3. **P2b: KIQ MAP_QUEUES** — revisit KFD queue path by adding explicit
   KIQ MAP_QUEUES step, enabling per-process VMID (8) dispatch.
