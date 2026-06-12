# HeteroKern

**GCN tasks as first-class Linux processes on AMD APU.**

HeteroKern makes GPU compute units a native execution target for Linux
processes. A `clone3(CLONE_GCN)` call creates a real `task_struct` whose
wavefront runs on a GCN CU — same pid, same address space, same file
descriptors. The kernel's scheduler, syscall handlers, and fault handlers
remain unmodified.

## Motivation

Modern APUs ship with GPU compute units that sit idle during general
computation. To use them, programmers must write explicit offload code with
domain-specific SDKs (CUDA, HIP, OpenCL). HeteroKern asks: **what if a GCN CU
were just another core the scheduler knows about?**

The goal is to let any C program spawn threads that run transparently on
either x86 or GCN cores — no explicit offload, no driver, no SDK. A
`clone3()` with `CLONE_GCN` is all it takes.

## Architecture

```
  clone3(CLONE_GCN, &entry)
    │
    ├─ copy_process() → normal CoW mm, fd table, creds
    ├─ Allocate mailbox + HSA signal
    ├─ Submit AQL packet → wavefront starts on CU
    └─ Task sleeps (TASK_UNINTERRUPTIBLE on CFS runqueue)

         ┌─── Wavefront on CU ────────────────────────┐
         │  ... executes GCN code ...                  │
         │  Needs syscall → write mailbox, ring doorbell│
         └──────────────────────────────────────────────┘
                          │
         IH interrupt → xarray lookup → wake_up_process()
                          │
         ┌─── GCN task on CPU (CFS schedules it) ─────┐
         │  do_syscall_64(nr, args...)  ← unmodified  │
         │  Write result → re-arm signal → re-dispatch │
         │  Task sleeps again                          │
         └──────────────────────────────────────────────┘

  Signals: CWSR queue unmap (GFX9) → preempt wave → shadow context
  Faults:  IOMMUv2 PPR → handle_mm_fault() on task's mm → XNACK retry
```

- **Unmodified**: core scheduler, `do_syscall_64()`, page fault handler, signal
  delivery infrastructure
- **Kernel module owns**: clone hook, mailbox/doorbell service, AQL dispatch,
  CWSR bridge, `/sys` topology
- **SVM**: CPU and GPU share one virtual address space — no data copies

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| GCN task = real `task_struct` | Unmodified scheduler, syscall, fault handlers; task identity preserved |
| Mailbox replaces `SYSCALL` for GCN | GCN has no `SYSCALL` instruction; doorbell → IH → `wake_up_process()` → task calls `do_syscall_64()` itself |
| CWSR for signal preemption | GFX9 hardware saves wavefront state; fatal signals can kill GPU tasks < 100ms |
| IOMMUv2 PPR for faults | Hardware routes GPU page faults to `handle_mm_fault()` — no custom fault routing |
| SVM (GFX9) | `malloc()` pointers valid on both CPU and GPU; no explicit GPU VM management |
| Build on amdkfd, don't replace it | KFD handles HSA queues, AQL dispatch (MEC), doorbell allocation |

## Hardware

| Component | Detail |
|-----------|--------|
| APU | AMD Ryzen 3 2200G (Raven Ridge, AM4) |
| CPU | 4× Zen @ 3.5/3.7 GHz |
| GPU | 8× GCN 5.0 CUs (Vega 8, gfx902, 512 shaders) |
| Memory | DDR4, SVM — shared virtual address space |
| GPU features | MEC, CWSR, XNACK, IOMMUv2 PPR |

### Previous Platform (Retired)

| Component | Detail |
|-----------|--------|
| ~~APU~~ | ~~AMD A8-7500 (Kaveri, FM2+)~~ — no MEC firmware, dispatch impossible |

## Build & Development

Requires: ROCm 6.x toolchain, Linux 6.x kernel source.

```bash
# Build GCN kernels (on ROCm host)
make gcn

# Build kernel (on EPYC workstation)
make kernel

# Deploy + test
./scripts/build-kernel.sh && ./scripts/kernel-test.sh
```

Build artifacts land in `build/`. See [Roadmap.md](Roadmap.md) for the full
development plan.

## Status

Platform migrated from A8-7500 to 2200G. Architecture redesigned around the
GCN task model (real `task_struct`, unmodified kernel core). Phase 0 (2200G
bring-up) in progress — amdgpu needs Raven firmware in kernel config.

## License

TBD
