# Kernel Patch Plan — Fix KFD Queue Creation on GFX7 (no-SVM GPUs)

## Goal

Make `AMDKFD_IOC_CREATE_QUEUE` work on GFX7 (CIK/Sea Islands) when the
caller passes GPU virtual addresses (not CPU addresses) for the ring
buffer, write pointer, and read pointer.

## Root Cause

In `kfd_queue_acquire_buffers()` (`drivers/gpu/drm/amd/amdkfd/kfd_queue.c`),
the function calls `amdgpu_vm_bo_lookup_mapping(vm, user_addr)` using the
CPU virtual address from userspace. On GFX7 (no SVM), the buffer is mapped
in the GPU VM at its GPU VA, not at the CPU VA where `mmap` placed it.
The lookup fails because the CPU VA is not in the GPU page tables.

## Proposed Fix

**One-line change in `kfd_chardev.c`**: when the caller passes a non-zero
`va_addr` in the allocation request (meaning they requested a specific GPU VA),
store the GPU VA as the queue's ring/wptr/rptr address instead of the CPU VA.

More precisely, the fix is in `set_queue_properties_from_user()`:

```c
/* In kfd_chardev.c, set_queue_properties_from_user() */
/* Current code: */
q_properties->queue_address = args->ring_base_address;
q_properties->read_ptr = (void __user *)args->read_pointer_address;
q_properties->write_ptr = (void __user *)args->write_pointer_address;

/* Patched code for the caller who allocated at GPU VAs: */
q_properties->queue_address = args->ring_base_address;
q_properties->read_ptr   = (void __user *)args->read_pointer_address;
q_properties->write_ptr  = (void __user *)args->write_pointer_address;
```

Wait — the fix isn't in the properties assignment. The properties already
store the GPU VA (which the caller passed). The issue is in
`kfd_queue_acquire_buffers()` which calls `amdgpu_vm_bo_lookup_mapping()`
and expects to find the buffer at the user address.

**Actual fix needed**: `kfd_queue_acquire_buffers()` should not do the
VM lookup for the ring/wptr/rptr if the caller already provided them via
pre-allocated memory. Instead, the BO pointers should be obtained directly
from the KFD memory allocation handle.

### Concrete Implementation

1. **Extend `kfd_ioctl_create_queue_args`** with a new flag field:
   ```c
   #define KFD_QUEUE_FLAG_PREALLOCATED_RING  (1 << 0)
   ```
   When set, the ring/wptr/rptr are already allocated at the given GPU VAs
   and mapped in the GPU VM. KFD should skip `kfd_queue_acquire_buffers()`.

2. **Extend `kfd_ioctl_create_queue_args`** with handle fields:
   ```c
   __u64 ring_buffer_handle;
   __u64 write_pointer_handle;
   __u64 read_pointer_handle;
   ```
   These are the KFD memory handles returned by `AMDKFD_IOC_ALLOC_MEMORY_OF_GPU`.

3. **In `kfd_ioctl_create_queue`**, when the flag is set:
   - Skip `kfd_queue_acquire_buffers()`
   - Look up the BOs by handle using `kfd_process_device_translate_handle()`
   - Set `q_properties->wptr_bo`, `rptr_bo`, `ring_bo` directly

### Alternative: Fix kfd_queue_acquire_buffers for GPU VAs

Instead of new ioctl fields, teach `kfd_queue_acquire_buffers()` to also
search by GPU VA when the CPU VA lookup fails:

```c
/* In kfd_queue_acquire_buffers(), after amdgpu_vm_bo_lookup_mapping fails: */
/* Try the user address as a GPU VA (shifted by GPU page shift) */
if (!mapping) {
    uint64_t gpu_addr = user_addr >> AMDGPU_GPU_PAGE_SHIFT;
    mapping = amdgpu_vm_bo_lookup_mapping(vm, gpu_addr);
}
```

This is a minimal change that handles the common case where the caller
allocated at an explicit GPU VA.

## Effort Estimate

| Task | Effort |
|------|--------|
| Read and understand the full call chain | ✅ Done |
| Implement the fix (option 1: new flags) | 2-3 hours |
| Implement the fix (option 2: GPU VA fallback) | 30 minutes |
| Test on Kaveri with current userspace test programs | 1 hour |
| Compile replacement kernel | 30-60 min (existing build config) |
| Submit upstream (if desired) | Several weeks of review cycles |

## Relation to HeteroKern Architecture

This patch would make it possible to create queues from **userspace** using
standard KFD ioctls, which simplifies Phase 0 testing and allows mixing
userspace queue creation with kernel-space dispatch.

However, the kernel module approach (`hk_queue.c`) is still needed for
the **production HeteroKern architecture** because:

1. The kernel module owns the scheduler — it needs direct control over
   queue lifecycle, not mediated through ioctls.
2. The kernel module manages GCN thread context save/restore, which
   requires accessing internal KFD structures.
3. The kernel module handles the mailbox protocol for syscall forwarding,
   which runs in kernel context.

The patch is a **convenience improvement**, not a replacement for the
kernel module approach.
