# HeteroKern

**A unified heterogeneous OS kernel on AMD Kaveri APU.**

HeteroKern treats the AMD A8-7500 APU as a single machine with two classes of
cores — x86 Steamroller CPUs and GCN Compute Units — managed by one Linux
kernel under a single virtual address space (hUMA).

## Motivation

Modern APUs ship with powerful GPU compute units that sit idle during general
computation.  To use them, programmers must write explicit offload code with
domain-specific SDKs (CUDA, HIP, OpenCL).  HeteroKern asks: **what if GCN CUs
felt like just wider SIMD lanes?**

The goal is to let any C program spawn threads that run transparently on
either x86 or GCN cores — no explicit offload, no driver, no SDK.  A normal
`clone()` with a `CLONE_GCN` flag is all it takes.  The kernel scheduler
distributes work across the heterogeneous core topology automatically.

## Architecture

```
  x86 Steamroller (4 cores)          GCN Compute Units (6 CUs)
 ┌───────────┬───────────┐        ┌─────┬─────┬─────┬─────┬─────┬─────┐
 │ Ring 0:   │ Ring 3:   │        │ Wavefront (user threads)           │
 │ Linux     │ User      │        │ • No interrupts, ever              │
 │ kernel    │ threads   │        │ • Native 64-wide SIMD             │
 │ • IRQs    │ • I/O     │        │ • Syscall via shared memory        │
 │ • Syscall │ • low     │        │   mailbox (no SYSCALL instr)       │
 │   handler │   latency │        │ • Full hUMA address space          │
 └───────────┴───────────┘        └────────────────────────────────────┘
           │                                    │
           └──────── hUMA shared memory ────────┘
              (single unified physical address space)
```

- **x86 cores**: run the kernel (Ring 0) and optionally user threads.  Handle
  all interrupts, all I/O, all syscall processing.
- **GCN CUs**: run user threads as wavefronts.  No interrupt lines are wired —
  they execute with deterministic timing.  Vector registers (VGPRs) are only
  saved at deliberate scheduling boundaries, not stolen by IRQs.
- **Syscall path**: GCN wavefront writes args to a per-thread mailbox in hUMA
  shared memory → x86 kernel polls the mailbox → executes `do_syscall_64()` →
  writes result back → GCN wavefront resumes.  The same `do_syscall_64()` serves
  both x86 `SYSCALL` instructions and GCN mailbox messages.

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| x86 runs both kernel AND user threads | Avoids bootstrap deadlock; I/O-heavy threads better on x86 |
| Mailbox protocol replaces `SYSCALL` for GCN | GCN has no `SYSCALL` instruction; shared memory is the only channel |
| Heterogeneous scheduling, not pure GPU-only | Makes GPU transparent — `clone(CLONE_GCN)` is the only API surface |
| hUMA as the foundation | No PCIe boundary, no explicit data copies, one page table |
| Build on amdkfd, don't replace it | KFD already handles HSA queues, trap dispatch, page fault routing |

## Hardware

| Component | Detail |
|-----------|--------|
| APU | AMD A8-7500 (Kaveri, FM2+) |
| CPU | 4× x86 Steamroller @ 3.0/3.7 GHz |
| GPU | 6× GCN 1.1 CUs (Radeon R7, 384 shaders) |
| Memory | DDR3, hUMA — unified physical address space |
| GPU features | hQ (self-queueing), HSA-compliant |

## Build & Development

Requires: ROCm 6.x toolchain, Linux 6.x kernel source.

```bash
# Build GCN kernels
make gcn

# Build kernel module (requires kernel source at kernel-src/)
make kernel

# Clean
make clean
```

Build artifacts land in `build/`.  See [Roadmap.md](Roadmap.md) for the full
development plan.

## Status

Phase B complete — minimal GCN kernels compile and link to `.hsaco` code
objects.  Phase C (mailbox protocol) is next.  Target machine is being
provisioned for on-hardware testing.

## License

TBD
