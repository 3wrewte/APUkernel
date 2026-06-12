# CIK (GFX7) Dispatch Lessons — Why HeteroKern Moves to GFX9+

## Executive Summary

After ~150 test iterations across 3 days, we concluded that **raw PM4/AQL dispatch
on CIK user-mode compute queues is not viable for HeteroKern's goals**. The
fundamental issue is architectural, not a bug in our code:

**CIK has no MEC firmware.** Without Micro-Engine Compute, the CP (Command
Processor) cannot interpret AQL dispatch packets. User-mode queues are limited
to PM4 packets, but PM4 `SET_SH_REG` is privileged and `INDIRECT_BUFFER` is
silently ignored on user queues. The only working dispatch path goes through the
HIQ (Host Interface Queue) with kernel privilege — a path that KFD uses
internally but does not expose through any public interface.

## What We Tried

| # | Method | Result | Root Cause |
|---|--------|--------|-----------|
| 1 | PM4 `DISPATCH_DIRECT` with MQD shader registers set | No execution (rptr stays 0) | CP reads shader from MQD `compute_pgm`, which KFD never sets |
| 2 | `PACKET3_SET_SH_REG` on user queue ring | No effect (no fault either) | Privileged packet, silently dropped on user queues |
| 3 | `PACKET3_INDIRECT_BUFFER` + IB containing SET_SH_REG | No effect | IB execution also drops privileged packets |
| 4 | AQL queue format + `hsa_kernel_dispatch_packet_t` | No execution | No MEC firmware to decode AQL packets |
| 5 | `pqm_update_mqd` to flush MQD changes | Returns `-EACCES` | Queue state prevents MQD update after creation |
| 6 | `kfd_activate_queue` (after `pqm_create_queue`) | Returns `-ETIME` after 9s | Already called internally by `pqm_create_queue` |
| 7 | GPU TLB sync via `amdgpu_amdkfd_gpuvm_sync_memory` | Not tested (machine down) | Needed to flush GPU page table changes |

## The Fundamental Architecture Difference

```
GFX7 CIK (Kaveri)                    GFX9+ (Vega/Raven Ridge)
─────────────────────────────        ─────────────────────────
CP = Command Processor only          CP + MEC (Micro-Engine Compute)
No AQL support                       Full AQL support via MEC firmware
User queue = raw PM4 only            User queue = AQL packets (64-byte)
Shader loading via HIQ privilege     Shader loading from AQL kernel_object
MQD compute_pgm ignored by CP        MEC reads kernel descriptor directly
```

## What Does Work on CIK

Despite the dispatch failure, the **entire infrastructure pipeline is correct**:

1. `kfd_create_process` → process context
2. `kfd_process_device_init_vm` → GPU VM setup
3. `kfd_bind_process_to_device` → GPU binding
4. `amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu` × 7 → buffer allocation at explicit GPU VAs
5. `amdgpu_amdkfd_gpuvm_map_memory_to_gpu` × 7 → GPU page table entries
6. Manual `queue_properties` construction → correct offsets (verified)
7. `kfd_alloc_process_doorbells` → doorbell BO allocation
8. `pqm_create_queue` → **QUEUE CREATED** (id=0, db_off=0)
9. `adev->doorbell.cpu_addr` → **reliable doorbell write** (no more intermittent!)
10. `amdgpu_bo_kmap` → ring buffer CPU access
11. PM4 packet write → ring buffer populated
12. MQD register write → registers updated at correct offsets

All of this code is **directly portable to GFX9+** — the only change needed is
the dispatch mechanism (steps 9-12 above, which will be replaced by AQL packet
submission via MEC).

## Key Technical Discoveries (Portable Knowledge)

### Doorbell Access (Universal)
`adev->doorbell.cpu_addr + doorbell_id` → `writel(value, addr)`.
Works on both CIK and GFX9+. The external module approach (guessing offsets)
was the wrong path; in-tree direct access solved this permanently.

### Queue Properties (Verified Offsets)
All `queue_properties` field offsets were verified on the target kernel
(6.12.86+deb13) via a compile-time `offsetof` module. These are correct and
portable.

### GPU VA Allocation (Required for No-SVM)
On CIK (no SVM), all GPU buffers must be allocated at explicit GPU VAs via
`amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu(va_addr=...)` and explicitly mapped
via `amdgpu_amdkfd_gpuvm_map_memory_to_gpu()`. On GFX9+ (SVM), this becomes
automatic — `malloc()` pointers are valid on both CPU and GPU.

### KFD Internal Function Signatures
All 13 KFD internal functions we resolved via kallsyms in the out-of-tree
module are directly callable from the in-tree module without any kallsyms
hacks. Their signatures are documented in the code.

## Migration Plan to GFX9+

### What Stays

- `heteroken.c` — entire queue creation pipeline (lines 1-200 essentially unchanged)
- GPU buffer allocation + mapping (simplified: va_addr=0 works with SVM)
- Doorbell logic (unchanged)
- Kernel descriptor construction (unchanged)
- sysfs trigger interface (unchanged)

### What Changes (estimated 2-3 days)

- Queue format: PM4 → AQL (already tested, just need to verify on GFX9)
- AQL dispatch: `hsa_kernel_dispatch_packet_t` with `kernel_object` pointing to descriptor → MEC firmware handles the rest
- Remove: MQD manual registers (MEC reads descriptor directly)
- Remove: all CIK-specific PM4 packet construction (SET_SH_REG, INDIRECT_BUFFER)

### Recommended APU

AMD Ryzen 3 2200G (Raven Ridge) or Ryzen 3 3200G (Picasso):
- GFX9 (Vega) architecture
- AM4 socket, DDR4
- Full ROCm support
- SVM + MEC + XNACK
- Second-hand: ~¥100-150 for CPU, ~¥150-200 for AM4 motherboard

## Files Relevant to CIK Work

- `kernel-patches/heteroken.c` — in-tree KFD module (portable core)
- `kernel/hk_queue.c` — out-of-tree kallsyms module (OBSOLETE on GFX9)
- `docs/phase-h-dispatch-notes.md` — detailed dispatch debugging log
- `docs/kernel-hack-notes.md` — why kallsyms bypass was needed
- `docs/why-offsets.md` — offset guessing explanation
- `docs/kernel-patch-plan.md` — plan to fix KFD ioctl for no-SVM

---

*Decision date: 2026-06-05. Moving to GFX9+ APU.*
