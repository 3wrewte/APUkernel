# GFX7 (Kaveri) vs GFX9 (Vega) — SVM and Its Implications for HeteroKern

## Summary

HeteroKern targets **GFX7 (Kaveri)** by choice, despite the availability of
newer APUs with SVM (Shared Virtual Memory). This document catalogs what SVM
would give us "for free" and why we are choosing to solve these problems
manually on GFX7 instead.

## What SVM (GFX9/Vega) Provides

| Feature | GFX7 (CIK, no SVM) | GFX9 (Vega, SVM) |
|---------|-------------------|-------------------|
| CPU↔GPU VA sharing | Separate page tables; must map explicitly | Single page table, `malloc()` result valid on both |
| `create_queue` ioctl | Fails unless ring buffers are mapped at specific GPU VAs | Pass CPU pointer directly |
| Page fault handling | Must manually route GPU faults via KFD trap handler to x86 | XNACK: hardware retries faulting access; kernel handles transparently |
| `mmap` semantics | CPU VA ≠ GPU VA; explicit `MAP_MEMORY_TO_GPU` required per buffer | `mmap` creates a mapping visible to both CPU and GPU |
| Pointer passing between CPU and GPU threads | Must translate addresses; shared data needs explicit GPU VM mapping | Direct pointer sharing; no translation layer needed |
| Scratch/stack management for GCN threads | Must pre-allocate and map scratch backing store | Managed by driver automatically |
| `amdgpu_gem_va` ioctl | Required for every GPU VM mapping | Not needed; VA space is shared |

## What SVM Hides (and Why We Want to See It)

On GFX9, the kernel absorbs complexity that is central to HeteroKern's
mission:

1. **GPU page table management** — On Kaveri, we must explicitly call
   `amdgpu_amdkfd_gpuvm_map_memory_to_gpu()` for every GCN thread's stack,
   heap, and mailbox. This is tedious but teaches us the GPU MMU model
   that will be essential when extending to discrete GPUs as NUMA nodes.

2. **XNACK page fault handling** — Vega retries GPU page faults in hardware.
   On Kaveri, we must register a KFD trap handler and route faults through
   the mailbox protocol ourselves. This is the exact mechanism needed for
   a custom OS kernel that wants to handle GPU faults intelligently.

3. **Explicit VA allocation** — Without SVM, we must choose GPU virtual
   addresses for every allocation. On a discrete GPU with its own VRAM,
   this VA management is unavoidable even with SVM. Solving it now on
   Kaveri means the discrete-GPU path will reuse the same allocator.

4. **Queue creation internals** — The `create_queue` ioctl on GFX7 requires
   ring buffers at GPU VAs that match their CPU-side access addresses.
   Working around this (by allocating at explicit GPU VAs) forces us to
   understand the HSA queue ABI at the hardware level—knowledge that is
   essential for implementing a kernel-side scheduler that manages GCN
   wavefront dispatch directly.

## Development Strategy

We proceed on Kaveri with the understanding that:

- **Every GPU VM operation is explicit.** The `kfd_queue_acquire_buffers`
  function calls `amdgpu_vm_bo_lookup_mapping(vm, user_addr)`, which
  requires the passed VA to be mapped in the GPU page tables. On Kaveri,
  this means we must allocate buffers at specific GPU VAs (via
  `KFD_IOC_ALLOC_MEMORY_OF_GPU` with `va_addr` set) and pass those
  same VAs to all subsequent KFD calls.

- **The kernel module approach** (bypassing ioctls via kallsyms) gives
  us direct control over GPU VM mappings and queue creation. This is
  the path we follow for Phase H and beyond.

- **Porting to Vega later** is a simplification pass: remove explicit
  `MAP_MEMORY_TO_GPU` calls, delete GPU VA management, replace trap
  routing with XNACK. The architecture designed for Kaveri degrades
  gracefully to Vega as a special case.

## GFX7-specific Technique: GPU VA Allocation

On Kaveri, the correct idiom for GPU-visible memory that must be
referenced by both CPU and KFD kernel code:

```
1. Get process apertures (AMDKFD_IOC_GET_PROCESS_APERTURES_NEW)
   → learn GPU VM range: base=0x10000, limit=0xfffffffff

2. Allocate buffer at a chosen GPU VA within the aperture range:
   alloc.va_addr = chosen_gpu_va  (e.g., 0x100000)
   alloc.flags   = GTT | WRITABLE
   → KFD creates BO; stored with mem->va = chosen_gpu_va

3. Map to GPU VM (critical step):
   AMDKFD_IOC_MAP_MEMORY_TO_GPU
   → GPU page tables now have: GPU_VA(chosen_gpu_va) → physical pages

4. mmap via DRM fd to get CPU VA:
   ptr = mmap(..., drm_fd, alloc.mmap_offset)
   → CPU VA (e.g., 0x7f...) is completely different from GPU VA

5. Pass the GPU VA, NOT the CPU VA, to create_queue:
   ring_base_address = chosen_gpu_va  (e.g., 0x100000)
   → kfd_queue_buffer_get finds it in GPU page tables → success
```

## When Vega Would Be "Easier"

For reference, on Vega the above collapses to:

```
1. malloc(N)  → returns pointer P
2. hsaKmtAllocMemory with va_addr=P → maps P in GPU VM automatically
3. Pass P to create_queue → works immediately
```

The trade-off is clear: Kaveri requires ~200 lines of GPU VA management
that Vega hides, but the understanding gained applies to discrete GPUs,
which are the project's ultimate NUMA target.
