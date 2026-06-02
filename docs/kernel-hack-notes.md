# Kernel Hack Notes — Why We Had to Bypass the KFD ioctl Layer

## The Problem

Creating a compute queue on GFX7 (Kaveri) through the standard `/dev/kfd` ioctl
interface (`AMDKFD_IOC_CREATE_QUEUE`) fails with `-EINVAL` inside
`kfd_queue_acquire_buffers()`. This function calls `amdgpu_vm_bo_lookup_mapping()`
to find the ring buffer's backing BO by its CPU virtual address. On GFX7,
CPU VA ≠ GPU VA (no SVM), so the lookup fails.

This is **not a bug in our code**. The ioctl path simply does not support
no-SVM GPUs when passed CPU VAs for ring/wptr/rptr. The standard workaround
would be to allocate buffers at explicit GPU VAs and pass those GPU VAs to
the ioctl — but then `access_ok()` validation on the ring address fails.

In practice, ROCm's HSA runtime works around this by using `amdgpu_gem_va`
ioctls and internal knowledge of GPU VM layout, not the KFD ioctl alone.

## Our Solution: kallsyms Bypass

We wrote a kernel module (`kernel/hk_queue.c`) that directly calls KFD's
internal (static, unexported) functions by resolving their addresses at
runtime via `kallsyms_lookup_name`.

### Functions Called

| Symbol | Purpose |
|--------|---------|
| `kfd_create_process` | Create a KFD process context for the current task |
| `kfd_process_device_data_by_id` | Get per-process per-device data |
| `kfd_process_device_init_vm` | Initialize GPU VM for the process |
| `kfd_bind_process_to_device` | Bind process to GPU (requires drm_priv) |
| `amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu` | Allocate GPU-visible buffer at explicit VA |
| `amdgpu_amdkfd_gpuvm_map_memory_to_gpu` | Map buffer into GPU page tables |
| `kfd_alloc_process_doorbells` | Allocate doorbell backing BO (required before queue creation) |
| `pqm_create_queue` | Create the actual hardware queue (bypasses `kfd_queue_acquire_buffers`) |
| `pqm_destroy_queue` | Destroy queue |

### Structural Pitfalls

All of these functions take internal KFD types (`struct kfd_process`,
`struct kfd_node`, `struct queue_properties`, etc.) that are defined in
headers under `drivers/gpu/drm/amd/` and are NOT accessible from external
kernel modules.

We had to:

1. **Hard-code struct field offsets** for `queue_properties`. These were
   verified by compiling a one-shot module that prints `offsetof()` on the
   running kernel (see `tests/kfd_queue_test.c` output in the progress log).

   We also verified the correct offset for the `bo` pointer in `struct kgd_mem`
   (32 bytes = `sizeof(struct mutex)` on 6.12 without debug mutexes).

2. **Locate `struct process_queue_manager` within `struct kfd_process`**
   by pattern-matching. The PQM is identified by scanning for:
   ```
   [process_ptr, list_head.next==&head, list_head.prev==&head, bitmap_ptr]
   ```
   Found at `proc + 688` on Debian 6.12.86.

3. **Locate `struct kfd_dev *` within `struct kfd_node`** by computing
   the byte offset from the source layout. Found at `node + 216` after
   manual calculation + verification via memory dump.

4. **Find `drm_priv` in `struct kfd_process_device`** by scanning for
   the DRM file pointer we obtained from `filp_open()`.

### Correct Call Order

The order matters. `kfd_bind_process_to_device` checks that `pdd->drm_priv`
is set, so `kfd_process_device_init_vm` (which sets it) must be called
**before** bind. We also learned that `kfd_alloc_process_doorbells` must
be called **before** `pqm_create_queue`, otherwise the CIK queue creation
code crashes trying to compute the GPU offset of a NULL doorbell BO.

Full correct order:
```
1. kfd_create_process
2. kfd_process_device_data_by_id
3. kfd_process_device_init_vm     ← sets drm_priv
4. kfd_bind_process_to_device     ← checks drm_priv != NULL
5. amdgpu_amdkfd_gpuvm_alloc × 4  ← ring, wptr, rptr, eop
6. amdgpu_amdkfd_gpuvm_map × 4
7. Build queue_properties manually
8. kfd_alloc_process_doorbells    ← required for CIK
9. pqm_create_queue
```

## What Would Be Different on GFX9+ (Vega and newer)

With SVM support, the entire `kallsyms` bypass is unnecessary:
- `create_queue` ioctl works directly (CPU VA = GPU VA)
- No manual struct layout hacking needed
- No offset scanning or pattern matching
- The module could be written as a normal kernel module with proper headers

## What Would Be Different on a Non-AMD GPU

The approach is vendor-specific. NVIDIA (nouveau/open-gpu-kernel-modules)
and Intel (i915/Xe) have completely different kernel interfaces. KFD is
AMD-specific. Porting would require understanding each vendor's compute
queue submission API (which may not exist as a public interface at all
for some vendors).
