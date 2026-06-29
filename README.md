# HeteroKern

**GCN tasks as first-class Linux processes on AMD APU.**

HeteroKern makes GPU compute units a native execution target for Linux
processes. A GCN wavefront runs on a Vega CU — same pid, same file
descriptors, same syscalls (`read`, `write`, `exit`). The kernel's scheduler,
syscall handlers, and fault handlers remain unmodified.

## Quick Start

### Write a GCN program in C

```c
#include "hk_libc.h"

static inline __attribute__((always_inline))
void hk_main(volatile void *mb, void *input, void *output,
             int width, int height)
{
    hk_write(mb, 1, "hello from GCN C!\n", 18);
    hk_exit(mb, 0);
}

HK_ENTRY
```

### Compile and run

```bash
# Package the SDK (on ROCm build host)
make sdk

# Compile a program
build/sdk/bin/hk-compile my_program.c -o my_program.co

# Run on target (2200G with HeteroKern kernel)
./compute_runner cprog my_program.co
```

The GPU calls `write(stdout, "hello from GCN C!", 18)` via the mailbox
syscall protocol — no explicit data transfer, no driver API calls.

## How It Works

```
  C source                →  clang -O2  →  GCN binary (.co)
       │                                        │
  hk_write()            inlined into        flat_store + s_sendmsg
  hk_read()             one function         + buffer_wbinvl1_vol
  hk_exit()                                 + s_endpgm
                                                │
                                    ┌───────────┘
                                    ▼
              ┌─── Wavefront on CU ──────────────────┐
              │  flat_store mailbox: syscall_nr, args │
              │  s_sendmsg(MSG_INTERRUPT)             │
              │  → trap handler → s_rfe_b64 → return  │
              │  poll mailbox.state until CPU clears   │
              │  flat_load mailbox.retval              │
              │  ... continue executing ...            │
              └────────────────────────────────────────┘
                               │
              ┌─── CPU (kernel ioctl) ──────────────────┐
              │  Poll mailbox for state=SYSCALL_PENDING │
              │  Execute: kernel_read / kernel_write    │
              │  Write retval, clear state, flush cache │
              └─────────────────────────────────────────┘
```

The wavefront **stays alive** across syscalls — registers and PC are
preserved. `s_sendmsg(MSG_INTERRUPT)` triggers a trap; the minimal trap
handler (`s_rfe_b64`) returns to the next instruction. The CPU polls the
mailbox and executes the syscall on behalf of the GPU.

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| `s_sendmsg` + live wavefront | Wavefront survives syscalls; no register save/restore needed |
| Mailbox = control channel only | Data goes through separate BOs; mailbox carries only syscall_nr + args |
| GPU addr → CPU addr translation | `hk_translate()` maps GPU BO addresses to kernel virtual addresses |
| `static inline` + `-O2` | Avoids `buffer_store` (broken under VMID 0); everything inlines into one flat function |
| `drm_clflush_virt_range` | CPU→GPU mailbox writes require cache flush; GPU needs `buffer_wbinvl1_vol` for reverse |

## Available Syscalls

| C function | Syscall | Direction |
|-----------|---------|-----------|
| `hk_write(mb, fd, buf, n)` | `write(fd, buf, n)` | GPU → CPU (data from any GPU buffer) |
| `hk_read(mb, fd, buf, n)` | `read(fd, buf, n)` | CPU → GPU (data into GPU buffer) |
| `hk_exit(mb, code)` | N/A | Terminates wavefront |

## SDK

The SDK packages everything a developer needs:

```
build/sdk/
├── include/hk_libc.h      C library (read/write/exit, all inline)
├── lib/hk.ld              Linker script
├── bin/hk-compile         One-command compiler wrapper
├── examples/hello.c       Hello world sample
└── host/compute_runner.c  Host-side runner
```

See [build/sdk/README.md](build/sdk/README.md) for full documentation.

## Hardware

| Component | Detail |
|-----------|--------|
| APU | AMD Ryzen 3 2200G (Raven Ridge, gfx902, 8 CUs) |
| Build host | EPYC workstation with ROCm 6.x toolchain |
| Target | 2200G at 192.168.2.170, PXE boot, HeteroKern kernel |

## Build & Development

Requires: ROCm 6.x toolchain, Linux 6.x kernel source.

```bash
# Build GCN kernels + SDK
make gcn sdk

# Build kernel (on EPYC workstation)
make kernel

# Deploy + test
./scripts/build-kernel.sh && ./scripts/kernel-test.sh
```

See [Roadmap.md](Roadmap.md) for progress and [docs/architecture-decisions.md](docs/architecture-decisions.md)
for detailed design rationale.

## License

TBD
