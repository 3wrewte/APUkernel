# Why We Guess Struct Offsets Instead of Using Kernel Headers

## The Problem

HeteroKern's kernel module (`kernel/hk_queue.c`) calls KFD internal
functions (`pqm_create_queue`, `kfd_alloc_process_doorbells`, etc.)
via kallsyms. These functions take parameters whose types are defined
in `drivers/gpu/drm/amd/amdkfd/kfd_priv.h` and related headers.

These headers are **not shipped in the Debian `linux-headers` package**.
They exist only in the full kernel source tree (`kernel-src/`), which
is a different branch than the running kernel (6.12.0 vs 6.12.86+deb13).

## Three Layers of the Problem

### Layer 1: KFD ioctl doesn't work on GFX7 (not a docs issue)

GFX7 lacks SVM (Shared Virtual Memory). The `AMDKFD_IOC_CREATE_QUEUE`
ioctl fails because `kfd_queue_acquire_buffers()` looks up a CPU virtual
address in the GPU page tables—which don't have that mapping on no-SVM
GPUs. This is an architecture limitation, not missing documentation.

See `docs/kernel-patch-plan.md` for a fix proposal.

### Layer 2: To bypass the ioctl, we call static KFD functions

KFD internal functions (`pqm_create_queue`, `kfd_alloc_process_doorbells`,
`kfd_bind_process_to_device`, etc.) are `static` within the amdgpu driver.
They are not exported to modules. We find them via kallsyms and call them
directly.

But their parameter types (`struct kfd_process_device *`,
`struct queue_properties *`, `struct amdgpu_device *`, etc.) are defined
in internal headers that external modules cannot include.

### Layer 3: Two ways to get struct definitions

| Approach | How | Pro | Con |
|----------|-----|-----|-----|
| **A. In-tree** | Put `heteroken.c` in `drivers/gpu/drm/amd/amdkfd/` and build as part of the kernel | `#include` all headers normally. Compiler computes offsets. `pdd->qpd.proc_doorbells` just works. | Must recompile kernel (~1 hour) on each code change |
| **B. Out-of-tree** | External module + kallsyms. Mirror struct layouts and compute field offsets by hand. | 30-second edit→test cycle | Fragile: every kernel upgrade requires re-validating offsets |

We chose **B** for faster iteration during Phase H development.
Every field accessed via `*(void **)((char *)ptr + N)` requires knowing
the byte offset N, which depends on the struct's exact memory layout.

## Why Offsets Change

The byte offset of a field depends on:

1. **Sizes of preceding fields** — which depend on kernel config:
   - `sizeof(struct mutex)` → 32 bytes normally, 72 with `CONFIG_DEBUG_MUTEXES`
   - `sizeof(spinlock_t)` → 4 bytes normally, 8 with `CONFIG_DEBUG_LOCKDEP`
   
2. **Compiler padding** between fields (alignment rules on x86_64)

3. **Debian-specific patches** — Debian's 6.12.86 may have backported
   patches that add/remove fields or change struct layouts

4. **Kernel version** — upstream 6.12.0 vs Debian 6.12.86 may differ

## How We Find Offsets

1. **Compute from source** — read `kfd_priv.h`, `amdgpu_amdkfd.h`, etc.
   from the kernel source tree, count field sizes, account for padding.
   Most offsets we use (queue_properties, kfd_node) were computed this
   way and verified.

2. **Compile-time measurement** — write a one-shot module that prints
   `offsetof(struct X, field)` using a mirrored struct definition
   (see `offset.c` in the progress log for queue_properties).

3. **Runtime scanning** — search a struct in memory for a known pattern
   (e.g., scanning `kfd_process` for the PQM's self-pointer + list_head).

4. **Memory dump** — print raw bytes and identify fields by value patterns
   (e.g., `adev+2288 = 0xfeb00000` → doorbell physical base).

## Current Unresolved Offsets

| Struct | Field | Status |
|--------|-------|--------|
| `struct amdgpu_device` | `doorbell.cpu_addr` | **UNKNOWN**. Found `doorbell.base` at adev+2288 via MMIO pattern scan. `cpu_addr` should be 32 bytes later but reads as NULL. Need `pahole` or full struct definition to confirm. |
| `struct kfd_process_device` | `qpd.proc_doorbells` | PDD+176 (verified working: doorbell_bo IS found here) |
| `struct queue` | `mqd` | queue+16 (verified working: MQD writes take effect) |
| `struct queue` | `doorbell_id` | queue+260 (verified working: doorbell RUNG used this) |
| `struct kfd_node` | `kfd` | node+216 (verified: doorbell alloc succeeds) |
| `struct kgd_mem` | `bo` | mem+32 = sizeof(struct mutex) (verified: bo_kmap works) |

## When We'll Switch to Approach A

Once Phase H (GCN dispatch) is complete and we move to the production
architecture (scheduler, mailbox protocol), we'll migrate the module
into the kernel tree. At that point, all offset guessing goes away:
the compiler handles everything.

The current approach B was the right choice for development velocity:
100+ test iterations over 3 days would have taken 100+ hours with approach A.
