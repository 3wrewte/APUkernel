# HeteroKern — Roadmap

## Project Goal

Transform AMD Ryzen 3 2200G (Raven Ridge) APU into a heterogeneous system where
a GCN task is a **real Linux task_struct** — created by `clone3(CLONE_GCN)`,
with a normal CoW address space — whose execution happens on a GPU wavefront.
The kernel's core scheduler, syscall handlers, and fault handlers remain
**unmodified**; only the dispatch and communication path is new.

- A GCN task is a `task_struct` with a GPU wavefront as its execution context
- `clone3(CLONE_GCN + GCN entry descriptor)` → `copy_process()` builds mm with
  normal CoW → the task arms a per-task mailbox + HSA signal, dispatches one
  AQL packet (its wavefront), and sleeps on the CPU runqueue
- Mailbox doorbell → IH interrupt → `wake_up_process()` → the task executes
  `do_syscall_64()` **as itself** on the CPU → writes result → wave resumes
- Signals preempt via CWSR queue unmap (GFX9), execute handlers on the shadow
  context
- Faults flow through IOMMUv2 PPR / retry faults into `handle_mm_fault()` on
  the task's mm
- CPU threads use normal `SYSCALL` — zero kernel changes for the x86 path

---

## Hardware Target

| Component | Detail |
|-----------|--------|
| APU | AMD Ryzen 3 2200G (Raven Ridge, AM4) |
| CPU | 4× Zen @ 3.5/3.7 GHz |
| GPU | 8× GCN 5.0 CUs (Vega 8, gfx902, 512 shaders) |
| Memory | DDR4, SVM — single unified virtual address space |
| GPU features | MEC (AQL dispatch), CWSR (wave preemption), XNACK, IOMMUv2 PPR |

### Why 2200G (GFX9) instead of A8-7500 (GFX7)

GFX7 (CIK / Kaveri) cannot dispatch user shaders through user-mode compute
queues — the CP firmware lacks MEC, and PM4 privileged packets are silently
dropped on user queues. After ~150 test iterations, we concluded this is an
architectural limitation, not a bug. See `docs/cik-lessons.md`.

GFX9 (Vega / Raven Ridge) provides:

| Feature | GFX7 (Kaveri) | GFX9 (Raven Ridge) |
|---------|---------------|---------------------|
| AQL dispatch | No MEC firmware → impossible | MEC firmware handles AQL natively |
| Signal preemption | Cannot preempt wavefront | CWSR queue unmap → shadow context |
| Page fault handling | Manual KFD trap routing | IOMMUv2 PPR / retry fault → `handle_mm_fault()` |
| Address space | Separate CPU/GPU page tables | SVM: `malloc()` pointers valid on both |
| XNACK | No | Hardware auto-retries on fault |

---

## Development Environment

| Component | Detail |
|-----------|--------|
| Build machine | EPYC 7502 workstation (openSUSE Leap 16.0, 192.168.1.162) |
| Target machine | 2200G (Debian 13, 192.168.2.170, PXE boot) |
| ROCm build host | Fedora 42 (192.168.1.8, user: dev) |
| ROCm version | 6.3.1 (clang 20.0.0.rocm) |
| ROCm llvm path | `/usr/lib64/rocm/llvm/bin/` |
| Device libs | `/usr/lib64/rocm/llvm/lib/clang/18/amdgcn/bitcode/` |
| Target kernel | Linux v6.12 (source at `kernel-deb/`) |
| Target GPU ISA | `gfx902` (Raven Ridge / GCN 5.0) |
| Target triple | `amdgcn-amd-amdhsa` |
| Kernel config | `kernel-deb/.config` |
| PXE deploy path | `/srv/http/vmlinuz` (served via HTTP) |
| Serial console | /dev/ttyUSB0 → target ttyS0 (115200n8) |

### SSH Quick Reference

```bash
# Target (2200G)
sshpass -p admin114514 ssh root@192.168.2.170

# ROCm build host
sshpass -p admin114514 ssh dev@192.168.1.8
```

---

## Architecture: The GCN Task Model

### Core Principle

A GCN task is a **first-class Linux task** that happens to execute on a GPU
wavefront. The kernel does not need a heterogeneous scheduler — CFS treats a
sleeping GCN task the same as any other `TASK_UNINTERRUPTIBLE` task.

### Lifecycle of a GCN Task

```
clone3(CLONE_GCN, &entry)
  │
  ├─ copy_process() → normal CoW mm, fd table, creds, etc.
  ├─ Allocate per-task mailbox + HSA signal (kernel module)
  ├─ Construct AQL dispatch packet (kernel module)
  ├─ Submit AQL packet to HSA compute queue → wavefront starts on CU
  └─ Set task state = TASK_UNINTERRUPTIBLE → schedule() returns to parent

              ┌─── Wavefront running on CU ────────────────────┐
              │  ... executes GCN code ...                     │
              │  Needs syscall: write args to mailbox           │
              │  Ring HSA doorbell → IH interrupt on CPU       │
              └────────────────────────────────────────────────┘
                              │
              IH ISR → lookup task by mailbox addr (xarray)
                      → wake_up_process(gcn_task)
                              │
              ┌─── GCN task on CPU (CFS schedules it) ────────┐
              │  do_syscall_64(nr, args...)  ← unmodified!    │
              │  Write result to mailbox                       │
              │  Re-arm HSA signal                             │
              │  Re-dispatch AQL packet → wave resumes         │
              │  Set state = TASK_UNINTERRUPTIBLE → sleep      │
              └────────────────────────────────────────────────┘

Exit paths:
  Cooperative: wave finishes → mailbox exit request → wakes shadow task
               → requests queue drain/unmap → do_exit()
  Fatal signal: CWSR queue unmap/preempt → wait for preemption fence
               → destroy GPU context → do_exit() on shadow task
```

### What the Kernel Module Owns (and ONLY this)

| Responsibility | Mechanism |
|---------------|-----------|
| **Clone path hook** | Intercept `clone3(CLONE_GCN)` → allocate mailbox + HSA signal + construct + submit AQL packet → set task UNINTERRUPTIBLE |
| **Mailbox / doorbell service** | IH interrupt → xarray lookup `mailbox_addr → task_struct` → `wake_up_process()` |
| **Dispatch** | AQL packet construction + HSA queue submission (MEC firmware handles the rest) |
| **Signal / CWSR bridge** | Fatal signal → CWSR queue unmap preempt → wait fence → destroy GPU context → exit shadow task |
| **/sys node exposure** | `/sys/devices/system/node/nodeX/heteroken/{type,compute_units,isa}` |

### What Is NOT Modified

| Kernel component | Why it doesn't need changes |
|-----------------|-----------------------------|
| Core scheduler (CFS) | GCN task is `TASK_UNINTERRUPTIBLE` when on CU, `TASK_RUNNING` when on CPU — normal CFS behavior |
| `do_syscall_64()` | Called by the GCN task itself on CPU — same fd table, same creds, same mm |
| Page fault handler | IOMMUv2 PPR routes GPU faults to `handle_mm_fault()` on the task's mm automatically |
| Signal delivery (normal) | Fatal signals use CWSR; non-fatal are deferred to next CPU visit |
| Interrupt handling | Irrelevant — IH handles the GPU→CPU doorbell, not the CU itself |

### Mailbox Protocol

The mailbox replaces the `SYSCALL` instruction for GCN tasks. It does NOT
replace the syscall handler.

```
GCN CU:     write nr+args to mailbox   →  ring doorbell (HSA signal)
IH ISR:     mailbox addr → xarray → task_struct  →  wake_up_process()
CPU:        GCN task runs do_syscall_64(nr, args...)  ← same function!
CPU:        Write result to mailbox     →  re-arm signal + re-dispatch AQL
GCN CU:     Read result from mailbox    →  wavefront resumes
```

This is architecturally equivalent to virtio (guest→host via virtqueue),
io_uring (userspace→kernel via shared rings), or HSA doorbell itself.

Mailbox structure (128 bytes, cache-line aligned):
```
Offset  Size    Field           Description
0x00    4       state           0=idle, 1=syscall_pending, 2=trap_pending
0x04    4       syscall_nr      Syscall number
0x08    48      args[6]         Syscall arguments (6×uint64_t)
0x38    8       retval          Syscall return value
0x40    8       error_code      errno on failure
0x48    8       fault_addr      Page fault virtual address
0x50    4       fault_type      0=r, 1=w, 2=x
0x54    44      reserved        Padding to 128 bytes
```

### Signal Handling on GCN Tasks

GFX9 provides CWSR (CWSR = Compute Wavefront Save/Restore):

- **Non-fatal signals** (e.g. SIGUSR1): deferred until the GCN task next
  visits the CPU (at a syscall boundary). The signal is pending in
  `task_struct::pending`; when the task wakes on CPU it checks `TIF_SIGPENDING`
  before re-dispatching. Handler executes on the shadow context (CPU-side
  saved register state).

- **Fatal signals** (SIGKILL, SIGTERM with default disposition, SIGSEGV):
  CWSR queue unmap preempts the wavefront immediately. The kernel module
  waits for the preemption fence/event, destroys the GPU context (queue,
  doorbell, mailbox), then the shadow task calls `do_exit()`.

- **Shadow context**: each live GCN task owns a private slot (allocated from a
  shared pool) for SGPR/VGPR save area. Shared global save memory is unsafe —
  a preempted task must have its own isolated save region.

### Fault Handling

On GFX9 with IOMMUv2:

1. GPU wavefront accesses an unmapped or read-only page
2. IOMMUv2 generates a PPR (Peripheral Page Request) or retry fault
3. The fault is attributed to the GCN task's `mm` (via PASID)
4. `handle_mm_fault()` resolves it — same path as CPU page faults
5. Wavefront automatically retries the access (XNACK) or is restarted

No custom fault routing is needed. This is a major simplification over the
GFX7 approach (manual KFD trap handler → mailbox forwarding).

### Root Filesystem

**Decision: debootstrap `--variant=minbase` + musl cross-compilation, NOT LFS.**

The target runs a standard Debian 13 rootfs (already deployed on the 2200G's
local SSD). GCN binaries are cross-compiled and deployed alongside x86 ones.

```
/ (local ext4 on /dev/sda2)
├── boot/           # Debian stock kernels (fallback)
├── lib/modules/    # kernel modules
├── sbin/init       # x86 binary, first user process
├── bin/            # x86 binaries + GCN binaries (future)
├── lib/            # glibc for x86, musl for amdgcn (future)
├── etc/            # minimal config
└── usr/            # additional binaries
```

systemd is excluded. Init is busybox init or a minimal script.

---

## Implementation Phases

### Phase 0: 2200G Bring-Up [ONLINE — 1-2 days] 🚨 BLOCKER

| # | Task | Verification |
|---|------|-------------|
| 0.1 | Update kernel config: replace Kaveri firmware in `CONFIG_EXTRA_FIRMWARE` with Raven firmware | `zcat /proc/config.gz \| grep EXTRA_FIRMWARE` shows raven_*.bin |
| 0.2 | OR: configure PXE to also serve an initramfs containing Raven firmware | amdgpu initializes before rootfs mount |
| 0.3 | Rebuild kernel + deploy to PXE + reboot target | `dmesg \| grep amdgpu` shows successful init |
| 0.4 | Verify KFD sees GPU: `/sys/devices/virtual/kfd/kfd/topology/nodes/1/` exists with simd_count > 0 | `rocminfo` shows gfx902 agent |
| 0.5 | Verify `/dev/kfd` and `/dev/dri/renderD128` both exist | Both device nodes present |
| 0.6 | Install `rocminfo` on target (if not present) | `rocminfo` reports CU count, ISA, SVM pools |

**Current blocker**: The PXE kernel has `CONFIG_EXTRA_FIRMWARE` hardcoded to
Kaveri firmware only. amdgpu fails to load `raven_gpu_info.bin` during early
boot (before rootfs is mounted). Fix requires either building Raven firmware
into the kernel image, or serving an initramfs with firmware files.

### Phase 1: AQL Dispatch End-to-End [ONLINE — 3-5 days] 🎯 FIRST MILESTONE — ACHIEVED 2026-06-15

**Milestone met**: GCN wavefront wrote magic value 0x21544148 to GPU memory,
read back by CPU via kernel compute ring + IB dispatch.

**Actual dispatch mechanism** (different from original plan):
- Kernel compute ring (`adev->gfx.compute_ring[0]`) + IB (Indirect Buffer)
- Uses `PACKET3()` (not `PACKET3_COMPUTE()`), VMID 0
- Shader code and result buffer placed in IB data area
- `amdgpu_ib_schedule()` + `dma_fence_wait_timeout()` for dispatch + completion

**KFD queue path** (attempted but SPI never created waves):
- KFD PM4-format queue: CP processed DISPATCH_DIRECT, all regs correct, but
  `SPI_CSQ_WF_ACTIVE_COUNT_0=0` — MEC firmware apparently requires KIQ
  MAP_QUEUES before forwarding dispatches to SPI.

| # | Task | Verification |
|---|------|-------------|
| 1.1 | Adapt `heteroken.c` for GFX9: in-tree build, PM4 format queue | Compiles + boots |
| 1.2 | GPU memory allocation: KFD gpuvm buffers at explicit GPU VAs | Allocation + mapping succeeds ✅ |
| 1.3 | PM4 command stream: SET_SH_REG + DISPATCH_DIRECT via KFD queue | CP processes all packets, rptr=WPTR ✅ |
| 1.4 | Rebuild `hello_gcn` targeting `gfx902` | `llvm-objdump` confirms GFX9 ISA ✅ |
| 1.5 | Kernel compute ring IB dispatch — shader executes, result verified | **result=0x21544148 MAGIC MATCH** ✅ |
| 1.6 | KFD queue path blocked: SPI never creates waves. Future work: add KIQ MAP_QUEUES | 🔧 |
| 1.7 | Clean up heteroken.c, remove dead KFD queue code | Pending |

### Phase 2: Mailbox + IH Interrupt Path [ONLINE — 4-6 days]

| # | Task | Verification |
|---|------|-------------|
| 2.1 | `include/mailbox.h` — update mailbox struct for new protocol (same layout, confirm alignment) | Compiles for both x86 and amdgcn |
| 2.2 | GCN-side mailbox write + doorbell ring: wavefront writes args, signals CPU | `llvm-objdump` validates ISA |
| 2.3 | xarray: `mailbox_gpu_addr → task_struct` lookup, registered at dispatch time | Lookup returns correct task |
| 2.4 | IH ISR hook: on doorbell interrupt, resolve mailbox → task → `wake_up_process()` | Woken task runs on CPU |
| 2.5 | GCN task on CPU: read mailbox → call `do_syscall_64()` → write result → re-dispatch AQL → sleep | Syscall executed as the GCN task |
| 2.6 | Test: GCN wavefront issues `write(1, "hello from CU\n", 14)` via mailbox → output appears on stdout | End-to-end syscall works |

### Phase 3: Clone Hook + GCN Task Birth [IN PROGRESS — 4-6 days]

**P3 core lifecycle verified (2026-06-16)** via kthread: `sleep → GPU → IH → wake → CPU → repeat`.
The only difference from final clone3 design is task_struct origin (kthread vs clone3).

**Architecture**: The dispatch layer is swappable — `hk_dispatch_ctx()` encapsulates
kernel compute ring + IB. When KFD queue path works, only this function changes.

| # | Task | Verification |
|---|------|-------------|
| 3.1 | `struct hk_gcn_ctx` — mailbox BO, fence_cb, owning task pointer | Compiles + boots ✅ |
| 3.2 | `hk_dispatch_ctx()` — refactored dispatch with pre-allocated mailbox, fence callback → `wake_up_process()` | IH wakes task ✅ |
| 3.3 | `hk_gcn_thread()` — kthread lifecycle: submit → sleep → wake → loop → stop | 50+ iterations ✅ |
| 3.4 | sysfs `spawn` / `stop` nodes — create and destroy GCN kthreads | Clean start/stop ✅ |
| 3.5 | Define `CLONE_GCN` flag + clone3 hook in `kernel/fork.c` | Pending |
| 3.6 | Hook `copy_process()`: detect `CLONE_GCN` → call `hk_clone_gcn_callback` | Pending |
| 3.7 | Post-clone: allocate mailbox, submit dispatch via `hk_dispatch_ctx()`, task → UNINTERRUPTIBLE | Pending |
| 3.8 | Test: real user-space task created via clone3, runs on GPU, exits | Pending |

### Phase 4: CoW Semantics + Address Space [ONLINE — 3-5 days]

| # | Task | Verification |
|---|------|-------------|
| 4.1 | Verify CoW: `hk_spawn(entry, 0)` (no CLONE_VM) → child gets CoW copy of parent mm | Child writes don't affect parent |
| 4.2 | Verify shared: `hk_spawn(entry, CLONE_VM)` → shared address space | Child writes visible to parent |
| 4.3 | Verify fd table: GCN task opens a file → appears in `/proc/<pid>/fd` | Correct fd ownership |
| 4.4 | Test: Test 2 from tests.txt — CoW fork semantics, same VAs | Pass |

### Phase 5: Signal Handling + CWSR [ONLINE — 5-7 days]

| # | Task | Verification |
|---|------|-------------|
| 5.1 | Non-fatal signal path: defer until next CPU visit, check `TIF_SIGPENDING` before re-dispatch | SIGUSR1 handler fires on CPU |
| 5.2 | Fatal signal path: CWSR queue unmap → wait preemption fence → destroy GPU context → `do_exit()` | SIGKILL kills GCN task < 100ms |
| 5.3 | SIGSEGV: null-pointer store on CU → IOMMUv2 fault → signal delivered to GCN task | Correct `si_addr` in siginfo |
| 5.4 | Shadow context: allocate from shared pool, each live task owns a private slot | Preemption save area isolated |
| 5.5 | Test: Test 4 from tests.txt — spinning GCN task is killable | Pass (the smoke test GFX7 could never pass) |

### Phase 6: IPC Across ISA Boundary [ONLINE — 3-5 days]

| # | Task | Verification |
|---|------|-------------|
| 6.1 | Pipe: CPU writes → GCN reads (and vice versa) via `read()`/`write()` syscalls through mailbox | Data flows correctly |
| 6.2 | Futex: GCN task `FUTEX_WAIT`, CPU task `FUTEX_WAKE` — proves mailbox composes with sleeping syscalls | GCN waiter wakes |
| 6.3 | Shared memory: CPU and GCN tasks share mmap'd region (SVM makes this trivial) | Both see same data |
| 6.4 | Test: Test 5 from tests.txt — pipe + futex across ISA boundary | Pass |

### Phase 7: Heterogeneous Fan-Out [ONLINE — 3-4 days] 🎯 SECOND MILESTONE

| # | Task | Verification |
|---|------|-------------|
| 7.1 | SAXPY benchmark: parent forks N GCN tasks + M CPU tasks over one array | Correct result |
| 7.2 | Verify GCN tasks appear in `top` / `ps` as normal tasks | Visible |
| 7.3 | GCN chunks beat one CPU core on FP32 throughput (sanity, not benchmark) | Measured |
| 7.4 | Test: Test 6 from tests.txt — heterogeneous fan-out | Pass |

### Phase 8: Negative Tests + Robustness [ONLINE — 3-4 days]

| # | Task | Verification |
|---|------|-------------|
| 8.1 | `execve()` from GCN task: either continue as x86 or fail with documented errno — never wedge | Documented behavior |
| 8.2 | x86 function pointer as kernel entry: spawn fails with `-ENOEXEC`, no GPU hang | Clean failure |
| 8.3 | 1000 sequential spawn/exit cycles: no CU/queue/doorbell leak | KFD queue counts return to baseline |
| 8.4 | Test: Test 7 from tests.txt — negative tests | Pass |

### Phase 9: /sys Topology + NUMA [ONLINE — 2-3 days]

| # | Task | Verification |
|---|------|-------------|
| 9.1 | Expose GPU as a NUMA node: `/sys/devices/system/node/node1/heteroken/type` = `GCN_CU` | Visible |
| 9.2 | `/sys/devices/system/node/node1/heteroken/compute_units` = `8` | Correct |
| 9.3 | `/sys/devices/system/node/node1/heteroken/isa` = `amdgcn-gfx902` | Correct |
| 9.4 | `set_mempolicy(MPOL_BIND, gpu_node)` — documented behavior on APU (EINVAL or node0) | Deterministic |
| 9.5 | Test: Test 0 + Test 3 from tests.txt | Pass |

### Phase 10: musl Port + Busybox [ONLINE — 2-4 weeks]

| # | Task | Verification |
|---|------|-------------|
| 10.1 | Port musl internal threading: `clone()` → GCN wavefront spawn via `hk_spawn()` | `pthread_create` works on GCN |
| 10.2 | Port musl stdio (buffered I/O) | `printf`, `scanf` work on GCN |
| 10.3 | Port musl malloc | `malloc`/`free` under GCN memory model |
| 10.4 | Cross-compile busybox for `amdgcn-amd-amdhsa` | All applets compile |
| 10.5 | Test shell (ash) on GCN | Interactive shell via serial |
| 10.6 | Test coreutils equivalents (ls, cp, mv, cat) | Basic file operations |

### Phase 11: dGPU as NUMA Node [FUTURE — requires discrete GPU]

This phase extends the model to discrete GPUs with local VRAM, treating them
as NUMA nodes with their own memory. Not implemented on the 2200G.

| # | Task | Verification |
|---|------|-------------|
| 11.1 | `set_mempolicy(MPOL_BIND, dgpu_node)` allocates pages in VRAM | Pages on GPU node |
| 11.2 | GCN task with `MPOL_LOCAL` allocates in local VRAM by default | Default behavior correct |
| 11.3 | Migration: pages can be migrated between CPU and GPU nodes | Data movement works |

---

## Key Invariants

1. **GCN task is a real task_struct** — created by `clone3()`, has its own pid, mm, fd table, creds
2. **Core scheduler is unmodified** — CFS schedules GCN tasks normally (sleeping when on CU, runnable on CPU)
3. **Syscall handlers are unmodified** — `do_syscall_64()` is called by the GCN task on CPU
4. **Fault handlers are unmodified** — IOMMUv2 PPR routes GPU faults to `handle_mm_fault()` on the task's mm
5. **Unified address space** — SVM: CPU and GPU share the same page tables and virtual addresses
6. **Core type is set at clone3() time** — `CLONE_GCN` spawns GCN wavefront; default spawns x86 thread
7. **No implicit migration between CPU and GCN** — different ISA, set at birth

## Hard Problems (Ranked)

1. **IH → task_struct lookup**: mapping a mailbox doorbell event to the correct
   sleeping task via xarray. Must be O(1) under interrupt context.
2. **CWSR preemption fence**: fatal signals must wait for the hardware to
   complete wavefront save before destroying context. Timing-sensitive.
3. **Shadow context slot management**: shared pool with per-task private
   isolation. Must not leak slots on crash/kill paths.
4. **CoW correctness**: GPU page faults on CoW pages must fault on the GPU
   side and be resolved by IOMMUv2 → `handle_mm_fault()`. Need to verify
   the full CoW path works through IOMMUv2.
5. **libc port**: musl assumes x86 syscall ABI. Each arch-specific file
   replaced with GCN mailbox shim.

## What Exists and Must NOT Be Rewritten

- **amdkfd** — upstream Linux kernel driver for AMD HSA
- **amdgpu** — upstream DRM driver (handles display, firmware, power management)
- **HSA ABI** — AMD-documented queue/dispatch packet format
- **GCN ISA** — fully documented by AMD
- **LLVM amdgcn backend** — compiles C to GCN machine code
- **IOMMUv2 PPR** — hardware fault routing to `handle_mm_fault()`
- **CWSR** — hardware wavefront save/restore (GFX9+)

### Code Reuse from CIK Work

The entire `heteroken.c` infrastructure (process creation, VM init, binding,
buffer allocation, queue creation, doorbell access, sysfs trigger) is directly
portable. See `docs/cik-lessons.md` section "What Stays".

---

## File Layout

```
APUkernel/
├── Makefile              # Top-level build orchestration
├── Roadmap.md            # This file
├── README.md             # Project overview
├── include/
│   ├── mailbox.h         # Shared mailbox struct (both targets)
│   ├── gcn_thread.h      # Per-task GPU context structure
│   └── protocol.h        # CLONE_GCN flag, syscall mappings, constants
├── gcn/                  # amdgcn target code
│   ├── hello_gcn.S       # Minimal GCN kernel
│   ├── mailbox.S         # GCN assembly mailbox primitives
│   ├── trap_handler.S    # GCN trap handler
│   ├── crt0_gcn.S        # _start entry point
│   ├── syscall_shim.c    # __gcnsyscall implementation
│   ├── gcnsyscall.h      # GCN_SYSCALL macros
│   ├── linker.ld         # GCN executable linker script
│   └── libc/             # Ported musl source files
├── kernel/               # x86 Linux kernel module [DEPRECATED — moved in-tree]
│   ├── hk_queue.c         # Phase H: external module (kallsyms bypass, OBSOLETE)
│   ├── heteroken.c        # Phase H: in-tree kernel module (SYMLINK → kernel-patches/)
│   └── kbuild.mk          # Kbuild for external module
├── kernel-patches/        # Kernel modifications (managed by git)
│   ├── heteroken.c        # In-tree KFD module: create queue + dispatch
│   └── amdkfd-makefile.patch  # Adds heteroken.o to AMDKFD_FILES
├── kernel-deb/            # Linux 6.12 kernel source (patched, built for PXE)
├── scripts/               # Automation
│   ├── build-kernel.sh    # Build kernel + deploy bzImage to PXE
│   ├── kernel-test.sh     # Rebuild + deploy + reboot + trigger test
│   ├── reboot.sh          # sysrq reboot + wait for SSH
│   ├── hw-check.sh        # Check target hardware status
│   ├── apply-kernel-patch.sh  # Apply heteroken changes to clean kernel tree
│   ├── build-gcn.sh       # Compile gcn/*.S → .hsaco
│   ├── deploy-test.sh     # SCP → build → insmod (old out-of-tree path, OBSOLETE)
│   └── cycle.sh           # reboot → deploy → test loop
├── tests/
│   ├── tests.txt          # Terminal test definitions
│   ├── test_mailbox.c     # Mailbox protocol unit tests
│   ├── test_scheduler.c   # Scheduler logic tests
│   └── test_mock.c        # Mock HSA dispatch for integration tests
├── rootfs/               # Root filesystem build scripts
│   ├── mkrootfs.sh       # debootstrap + customize script
│   ├── init.c            # Minimal init (x86 bootstrap)
│   └── gcn_init.c        # GCN-compiled /sbin/init
├── pxe/                  # PXE server config
│   ├── dhcpd.conf
│   ├── grub.cfg
│   └── exports
├── tmp/                  # Working state (not tracked)
│   └── state.md          # Current machine state + handoff notes
└── docs/                 # Design documents, notes
    ├── cik-lessons.md        # CIK dispatch analysis + migration rationale
    ├── gfx7-vs-gfx9.md      # SVM comparison (historical)
    ├── phase-h-dispatch-notes.md  # CIK dispatch debugging log
    ├── kernel-hack-notes.md  # Why kallsyms bypass was needed
    ├── kernel-patch-plan.md  # Plan to fix KFD ioctl for no-SVM
    ├── why-offsets.md        # Offset guessing explanation
    ├── kernel-compile.md     # Kernel build instructions
    └── gcn_isa_notes.md      # GCN ISA references
```

---

## Progress Log

| Date | Phase | Item | Status |
|------|-------|------|--------|
| 2026-05-07 | — | Roadmap created | ✅ |
| 2026-05-07 | A1-A4 | ROCm toolchain installed (11 packages) | ✅ |
| 2026-05-07 | A5 | Kernel v6.12 source cloned to kernel-src/ | ✅ |
| 2026-05-07 | — | Environment details added to Roadmap | ✅ |
| 2026-05-08 | — | Project directory structure created | ✅ |
| 2026-05-08 | C1 | `include/mailbox.h` — shared mailbox struct | ✅ |
| 2026-05-08 | B1 | `gcn/hello_gcn.S` — 2 kernels (direct + rel), compiles to .hsaco | ✅ |
| 2026-05-08 | B2 | `gcn/mailbox.S` — 7 mailbox primitives, compiles to .hsaco | ✅ |
| 2026-05-08 | B3 | `gcn/trap_handler.S` — 2 trap handlers, compiles to .hsaco | ✅ |
| 2026-05-08 | B4-B5 | Build system (Makefile) — .S→.o→.hsaco pipeline works | ✅ |
| 2026-05-08 | — | GFX7 ISA constraints documented (flat offset, v_add_u32, etc.) | ✅ |
| 2026-05-08 | — | Build output redirected to `build/`; `.gitignore` created | ✅ |
| 2026-05-08 | — | HW spec corrected: 4 CPU + 6 GPU (not 8) | ✅ |
| 2026-05-08 | — | Architecture revised: x86 can also run user threads; heterogeneous scheduling | ✅ |
| 2026-06-01 | — | Target ready: Debian 13, kernel 6.12.86, amdkfd built-in | ✅ |
| 2026-06-01 | G | rocminfo: 6 CUs / gfx700 / hUMA pools all visible | ✅ |
| 2026-06-01 | G | libhsakmt + HSA runtime installed; hsaKmtAllocMemory works | ✅ |
| 2026-06-01 | G | GPU VA allocation+MAP confirmed working (userspace + kallsyms) | ✅ |
| 2026-06-01 | D | Kernel module: 9 KFD internal symbols resolved via kallsyms | ✅ |
| 2026-06-01 | D | queue_properties offsets verified on target (offsetof module) | ✅ |
| 2026-06-01 | D | pqm located at proc+688 via list_head pattern match | ✅ |
| 2026-06-01 | D | GPU memory alloc+map works from kernel module | ✅ |
| 2026-06-01 | D | `pqm_create_queue` crashes — missing doorbell alloc + wrong kfd_dev offset | 🔧 |
| 2026-06-01 | D | Full fix: add EOP buffer, doorbell alloc, correct kfd_dev at node+216 | ✅ |
| 2026-06-01 | H | **QUEUE CREATED on GCN CU!** id=0 via kernel module kallsyms bypass | ✅ |
| 2026-06-01 | — | GFX7-vs-GFX9 doc written; sticking with Kaveri | ✅ |
| 2026-06-01 | — | Root cause of create_queue ioctl failure: GFX7 lacks SVM | ✅ |
| 2026-06-01 | — | docs/kernel-hack-notes.md: why kallsyms bypass was needed | ✅ |
| 2026-06-01 | — | docs/kernel-patch-plan.md: plan to fix KFD ioctl for GFX7 | ✅ |
| 2026-06-01 | H | Kernel code loaded into GPU memory via bo_kmap | ✅ |
| 2026-06-01 | H | Args buffer allocated + populated with target address | ✅ |
| 2026-06-01 | H | MQD updated: compute_pgm, rsrc1/2, user_data, dispatch_initiator | ✅ |
| 2026-06-01 | H | PM4 DISPATCH_DIRECT written to ring buffer | ✅ |
| 2026-06-01 | H | Doorbell rung (bo_kptr, intermittent: works 50% of boots) | ✅ |
| 2026-06-01 | H | **GCN execution blocked**: CIK MQD shader regs unused; need INDIRECT_BUFFER | 🔧 |
| 2026-06-01 | H | Hand-coded GCN bytes replaced with actual .hsaco extraction (bug fix) | ✅ |
| 2026-06-01 | H | SET_SH_REG direct on ring: CP ignores (likely privileged packet) | 🔧 |
| 2026-06-01 | H | INDIRECT_BUFFER(0x3F) with IB: rejected or ignored by CP | 🔧 |
| 2026-06-01 | H | pqm_update_mqd returns -EACCES | 🔧 |
| 2026-06-01 | H | **CIK conclusion**: user queues can't set shader via PM4; need HSA AQL dispatch | 🔧 |
| 2026-06-02 | H | AQL queue format (QP_FORMAT=1) + AQL dispatch packet: no execution | 🔧 |
| 2026-06-02 | — | docs/why-offsets.md: why we guess struct offsets | ✅ |
| 2026-06-02 | — | scripts/: deploy-test.sh, reboot.sh, cycle.sh, build-gcn.sh, hw-check.sh | ✅ |
| 2026-06-05 | — | Development migrated to EPYC 7502 workstation (64 threads, openSUSE) | ✅ |
| 2026-06-05 | — | PXE+routing configured; A8-7500 at 192.168.2.170 | ✅ |
| 2026-06-05 | — | Kernel 6.12.86+deb13 source extracted; heteroken.c moved in-tree | ✅ |
| 2026-06-05 | — | All kallsyms/offset-guessing removed: direct pdd/q/node access | ✅ |
| 2026-06-05 | H | Doorbell: adev->doorbell.cpu_addr — always works | ✅ |
| 2026-06-05 | H | sysfs /sys/kernel/heteroken/run trigger — no boot blocking | ✅ |
| 2026-06-05 | H | Full pipeline works: process→VM→bind→alloc×7→queue→MQD→descriptor→dispatch | ✅ |
| 2026-06-05 | H | AQL dispatch: doorbell rings, ring processed, kernel doesn't execute (CIK descriptor issue) | 🔧 |
| 2026-06-05 | H | **Root cause**: CIK has no MEC firmware; user-mode PM4/AQL dispatch non-viable | 🔧 |
| 2026-06-05 | — | **Decision**: Migrate to GFX9+ APU (Raven Ridge / Picasso / Renoir) | 🔄 |
| 2026-06-05 | — | kernel-patches/: git-tracked heteroken.c + amdkfd-makefile.patch | ✅ |
| 2026-06-05 | — | scripts/: apply-kernel-patch.sh, build-kernel.sh, kernel-test.sh | ✅ |
| 2026-06-05 | — | docs/cik-lessons.md: CIK dispatch analysis + migration plan | ✅ |
| 2026-06-05 | — | tmp/state.md: current machine state + handoff notes | ✅ |
| 2026-06-12 | — | **Platform migrated**: A8-7500 → 2200G (Raven Ridge, gfx902) at 192.168.2.170 | ✅ |
| 2026-06-12 | — | **Architecture redesigned**: GCN task model — real task_struct, unmodified scheduler/handlers | ✅ |
| 2026-06-12 | 0 | 2200G boots PXE, but amdgpu init fails: missing Raven firmware in CONFIG_EXTRA_FIRMWARE | 🔧 |
| 2026-06-12 | — | Roadmap rewritten for new platform + new architecture | ✅ |
| 2026-06-14 | 1 | Phase 1 begin: KFD queue + PM4 dispatch on GFX9 | ✅ |
| 2026-06-14 | 1 | kfd_queue_acquire_buffers CWSR size mismatch fixed (exact size required) | ✅ |
| 2026-06-14 | 1 | NO_HWS mode (sched_policy=2) for explicit VMID allocation. VMID=8 assigned | ✅ |
| 2026-06-14 | 1 | PM4 NOP dispatch: CP processes packet, rptr advances — CP ALIVE! | ✅ |
| 2026-06-14 | 1 | PM4 SET_SH_REG works: all compute registers programmed, confirmed by readback | ✅ |
| 2026-06-14 | 1 | PACKET3_COMPUTE format bugs fixed: opcode at bits[15:8], n = data_words-1 | ✅ |
| 2026-06-14 | 1 | COMPUTE_VMID must be set explicitly (CP doesn't inherit from queue VMID) | ✅ |
| 2026-06-14 | 1 | AQL queue format tested: WPTR stuck at 0 (rptr never advances). Switched to PM4 | ✅ |
| 2026-06-15 | 1 | gfx902 hello_gcn.hsaco built on dev machine (clang + ld.lld), encoding verified | ✅ |
| 2026-06-15 | 1 | gfx902 shader embedded in heteroken.c: 8-dword direct shader (no kernarg indirection) | ✅ |
| 2026-06-15 | 1 | **KFD queue dispatch blocked**: SPI never creates waves despite correct regs. SPI_CSQ_WF_ACTIVE=0 | 🔧 |
| 2026-06-15 | 2 | **Phase 2 mailbox + IH interrupt path complete!** callback fires in IRQ context | ✅ |
| 2026-06-15 | 2 | Mailbox shader: 26-dword gfx902 code, writes 5 structured fields to mailbox BO | ✅ |
| 2026-06-15 | 2 | `amdgpu_bo_create_kernel()`: persistent mailbox BO survives IB lifetime | ✅ |
| 2026-06-15 | 2 | `dma_fence_add_callback()`: IH interrupt fires callback in IRQ context, reads mailbox | ✅ |
| 2026-06-16 | 2 | `hk_dispatch_ctx()`: refactored dispatch with pre-allocated mailbox BO + task wakeup via `schedule()` + `wake_up_process()` | ✅ |
| 2026-06-16 | 3 | **Phase 3 GCN task lifecycle complete!** kthread → GPU (sleep) → IH IRQ → wake → CPU → loop → stop | ✅ |
| 2026-06-16 | 3 | `struct hk_gcn_ctx`: per-task GPU context (mailbox BO, fence_cb, owning task) | ✅ |
| 2026-06-16 | 3 | sysfs `spawn` / `stop`: create/kill GCN kthread, each iteration 12ms (IB→IH→wake) | ✅ |
| 2026-06-16 | 3 | **Verified**: 50+ GPU→CPU cycles without failure, mailbox data correct, clean exit | ✅ |
| 2026-06-16 | 3 | **hello_string shader**: GCN writes "hello from CU\n" to GPU memory, CPU reads via sysfs `/result` | ✅ |
| 2026-06-16 | 3 | `host_runner.sh` + `hello`/`result` sysfs nodes: GPU hello world end-to-end | ✅ |
