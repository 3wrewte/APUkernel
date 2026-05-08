# HeteroKern — Roadmap

## Project Goal

Transform AMD A8-7500 APU into a machine where:
- **x86 Steamroller cores** run ONLY the Linux kernel (Ring 0)
- **GCN Compute Units** run ALL user-space programs (bash, python, etc.)
- GCN vector units are treated like a wider SIMD unit (AVX-like), not an offload device

---

## Hardware Target

| Component | Detail |
|-----------|--------|
| APU | AMD A8-7500 (Kaveri, FM2+) |
| CPU cores | 4x x86 Steamroller |
| GPU cores | 8x GCN 1.1 Compute Units (Radeon R7) |
| Memory model | hUMA — single unified physical address space |
| GPU self-dispatch | hQ (HSA heterogeneous queueing) |
| Kernel driver | amdkfd (upstream Linux 6.x) |

---

## Development Environment

| Component | Detail |
|-----------|--------|
| Host OS | Fedora 42 |
| Host kernel | Locked (NVIDIA driver), not used for target |
| ROCm version | 6.3.1 (rocm-*-18-37.rocm6.3.1.fc42) |
| ROCm llvm path | `/usr/lib64/rocm/llvm/bin/` |
| Device libs | `/usr/lib64/rocm/llvm/lib/clang/18/amdgcn/bitcode/` |
| Target kernel | Linux v6.12 (cloned as `kernel-src/`) |
| Target CPU | `gfx700` (Sea Islands / GCN 1.1 / Kaveri) |
| Target triple | `amdgcn-amd-amdhsa` |
| Installed ROCm packages | rocm-llvm, rocm-llvm-devel, rocm-llvm-libs, rocm-clang, rocm-clang-devel, rocm-clang-libs, rocm-lld, rocm-device-libs, rocm-comgr, rocm-core, rocminfo |

### GFX7 ISA constraints (Kaveri = Sea Islands = GCN 1.1)

| Operation | Correct GFX7 encoding | Wrong (>GFX7) encoding |
|-----------|----------------------|------------------------|
| 32-bit add w/o carry | `v_add_i32_e32 dst, vcc, imm, src` | `v_add_u32_e32` (GFX8+) |
| 64-bit add w/ carry | `v_add_co_u32` + `v_addc_co_u32` (VOP3 only) | `v_addc_u32_e32` (GFX8+) |
| Flat load/store | `flat_load_dword vd, v[addr]` (no offset) | offset operand not supported |
| Flat load/store x2 | `flat_store_dwordx2 v[addr], v[data]` | ✓ works |
| Inline constant range | -16 to 64 (signed) | Larger values need VOP3 |

### Workaround: offset-free flat instructions

GFX7 flat instructions do not accept an immediate offset operand.
We compute the target address by adding the offset to the low 32 bits
of the pointer using `v_add_i32_e32`. This is safe because:
- Mailbox is 128 bytes, all offsets are < 128
- Mailbox is page-aligned (4096-byte boundary)
- Adding a small offset never overflows the low 32 bits of the pointer
- No carry propagation needed

---

## Architecture Decisions

### Root Filesystem Basis

**Decision: debootstrap `--variant=minbase` + musl cross-compilation, NOT LFS.**

Rationale:
- LFS is excessive — we need a working Linux userspace, not a from-scratch distro
- We only need to modify the syscall *entry point*, not the syscall implementations
- `task_struct` and the scheduler core remain intact; we replace only `context_switch()` for GCN threads

Boot sequence:
```
BIOS → GRUB → Linux kernel (x86, loads amdkfd)
  → initramfs: small x86 init loads heteroken_module.ko
  → module creates HSA queues, dispatches /sbin/init as GCN wavefront
  → PID 1 and all fork()/exec() children run on GCN CUs
```

Rootfs composition:
```
/ (NFS root or local ext4)
├── boot/          # kernel + initramfs (x86)
├── lib/modules/   # kernel modules including heteroken_module.ko (x86)
├── sbin/init      # GCN binary (amdgcn target), first user process
├── bin/           # busybox compiled for amdgcn
├── lib/           # musl libc compiled for amdgcn
├── etc/           # minimal config files (text)
└── usr/           # additional GCN user binaries
```

systemd is excluded entirely. Init is a custom minimal program or busybox init compiled for amdgcn.

### Mailbox Protocol

**Decision: Mailbox replaces the x86 `SYSCALL` instruction, NOT the syscall handler.**

In normal Linux, the syscall path is:
```
Userspace: mov $nr, %rax; SYSCALL     →  CPU traps to kernel
Kernel:    entry_SYSCALL_64()          →  do_syscall_64(nr, args...)
Userspace: result in %rax
```

In HeteroKern for GCN threads:
```
GCN CU:     write nr+args to mailbox   →  set state=1 (pending)
x86 poller: detects state==1           →  do_syscall_64(nr, args...)  ← same function!
x86 poller: writes result to mailbox   →  set state=0 (done)
GCN CU:     reads result from mailbox  →  continues execution
```

Linux already supports multiple syscall entry mechanisms per architecture
(x86 has `int $0x80`, `SYSENTER`, `SYSCALL`). The mailbox adds one more entry
mechanism. The actual kernel handler (`do_syscall_64`, individual `sys_*`
functions) are untouched.

This is architecturally equivalent to:
- **virtio** — guest-to-host communication via shared virtqueues
- **io_uring** — userspace-to-kernel via shared SQ/CQ rings
- **HSA doorbell** — GPU-to-CPU signaling via shared memory + doorbell

### Kernel Modifications Scope

**We do NOT rewrite the scheduler. We intercept the dispatch path.**

| Kernel component | Modified? | How |
|-----------------|-----------|-----|
| `task_struct` | Augmented | Add field: `is_gcn_thread`, `mailbox_ptr`, `gcn_context` |
| `do_fork()` / `copy_process()` | Augmented | New GCN threads get `is_gcn_thread=1`, allocate mailbox |
| `__schedule()` / `context_switch()` | Intercepted | If `next->is_gcn_thread`, call `dispatch_to_gcn(next)` instead of `switch_to()` |
| `do_syscall_64()` | NOT modified | Called from our mailbox poller, not from x86 trap entry |
| Page fault handler | Augmented | GCN page faults arrive via KFD trap → forwarded to x86 `do_page_fault()` |
| Signal delivery | Augmented | Signals queued in mailbox, no `send_signal()` direct delivery |
| Interrupt handling | NOT modified | Interrupts physically cannot reach GCN CUs (no IRQ lines wired) |

---

## Implementation Phases

### Phase A: Toolchain Setup [OFFLINE — 2-3 days]

| # | Task | Verification |
|---|------|-------------|
| A1 | Install `clang` with `amdgcn-amd-amdhsa` target | `clang --target=amdgcn-amd-amdhsa -c test.c -o test.o` succeeds |
| A2 | Install ROCm device-libs (libc.bc, libm.bc for GCN) | `/opt/rocm/lib/amdgcn/bitcode/libc.bc` exists |
| A3 | Install `libhsakmt` development headers | `pkg-config --cflags libhsakmt` works |
| A4 | Install `rocr-runtime` or build minimal HSA user lib | Can link against HSA ABI |
| A5 | Set up PXE + NFS server for A8-7500 | TFTP serves kernel, NFS exports rootfs |
| A6 | Write top-level build system (Makefile) | `make all` builds x86 module + GCN binaries |

### Phase B: Minimal GCN Kernel Code [OFFLINE — 3-5 days]

| # | Task | Verification |
|---|------|-------------|
| B1 | `gcn/hello_gcn.S` — write a magic value to a known hUMA address | Assembles to valid `.hsaco` |
| B2 | `gcn/mailbox.S` — GCN assembly macros for mailbox R/W | Compiles, `llvm-objdump -d` shows correct GCN ISA |
| B3 | `gcn/trap_handler.S` — minimal trap handler skeleton | Compiles |
| B4 | Build script: `.S → .o → .hsaco` | `file *.hsaco` reports `AMDGPU code object` |

### Phase C: Mailbox Protocol Definition [OFFLINE — 4-6 days]

| # | Task | Verification |
|---|------|-------------|
| C1 | `include/mailbox.h` — shared mailbox struct definition | Compiles for both x86 and amdgcn targets |
| C2 | GCN side `gcn/syscall_shim.c` — fill mailbox, wait for reply | Compiles to GCN ISA, verified via `llvm-objdump` |
| C3 | x86 side `kernel/mailbox_handler.c` — poll loop, call do_syscall | Compiles as kernel module |
| C4 | Unit tests for mailbox state machine | `make test_mailbox` passes on x86 host |
| C5 | Simulated integration test (x86 side mocks GCN writes) | Protocol correctness verified |

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

### Phase D: Kernel Module Skeleton [OFFLINE — 4-6 days]

| # | Task | Verification |
|---|------|-------------|
| D1 | `kernel/heteroken_main.c` — module init/exit | `insmod` succeeds on dev machine (no-op) |
| D2 | `kernel/hsa_queue.c` — HSA queue create/destroy via KFD ioctl | Compiles, API calls verified against kernel headers |
| D3 | `kernel/procfs.c` — /proc/heteroken/ debug interface | Compiles |
| D4 | `kernel/kbuild.mk` — Kbuild integration | `make -C /lib/modules/$(uname -r)/build M=$(pwd)` succeeds |

### Phase E: GCN Syscall Shim + musl Port Start [OFFLINE — 5-8 days]

| # | Task | Verification |
|---|------|-------------|
| E1 | `gcn/gcnsyscall.h` — GCN_SYSCALL0..6 macros | Compiles amdgcn, `llvm-objdump` validates |
| E2 | `gcn/gcnsyscall.c` — full `__gcnsyscall` implementation | Compiles |
| E3 | `gcn/crt0_gcn.S` — `_start` for GCN executables | Compiles, objdump shows correct entry |
| E4 | Port first 5 musl syscall wrappers (write, read, exit, brk, mmap) | Compiles for amdgcn |
| E5 | `gcn/linker.ld` — linker script for GCN executables | `ld.lld` accepts it |

### Phase F: Scheduler + Thread Model [OFFLINE — 4-6 days]

| # | Task | Verification |
|---|------|-------------|
| F1 | `include/gcn_thread.h` — `gcn_thread` struct definition | Compiles for x86 |
| F2 | `kernel/scheduler.c` — FIFO ready queue, run queue per CU | Unit tests pass |
| F3 | `kernel/context.c` — GCN context save/restore (SGPR/VGPR/LDS) | Logic tests pass |
| F4 | `kernel/dispatch.c` — AQL packet construction + queue submission | Compiles |
| F5 | Scheduler unit tests with mocked HSA dispatch | `make test_scheduler` passes |

### Phase G: KFD Integration Layer [OFFLINE compile / ONLINE test — 3-4 days]

| # | Task | Needs HW | Verification |
|---|------|----------|-------------|
| G1 | Enumerate GCN CUs via KFD topology ioctl | 🔴 | List of visible CU nodes |
| G2 | Create HSA queue per CU | 🔴 | `kfd_ioc_create_queue` succeeds |
| G3 | Register KFD trap/event handler for GCN page faults | 🔴 | Trap events reach handler |
| G4 | Allocate hUMA shared memory region (mailbox ring + scratch) | 🔴 | `mmap` from `/dev/kfd` succeeds |
| G5 | AQL packet constructor `kernel/aql.c` | ⬜ | Compiles, packet format validated |

### Phase H: End-to-End Hello World [ONLINE — 2-3 days] 🎯 FIRST MAJOR MILESTONE

| # | Task | Verification |
|---|------|-------------|
| H1 | Deploy kernel + module + GCN binary to A8-7500 | Boots, amdkfd loads |
| H2 | `test_mailbox_gcn.S` — GCN writes to shared memory, signals x86 | x86 reads correct value from hUMA |
| H3 | `test_dispatch.c` — x86 kernel dispatches GCN kernel, waits for result | Full round-trip works |
| H4 | GCN `write(1, "hello\n", 6)` via mailbox → x86 handles syscall → UART output | "hello" appears on serial console |
| H5 | GCN `exit(0)` via mailbox — clean termination | Process exits cleanly, resources freed |

### Phase I: Full Syscall Set [ONLINE — 4-6 days]

| # | Task | Verification |
|---|------|-------------|
| I1 | Implement remaining essential syscalls (~50) in musl GCN shim | Each syscall tested individually |
| I2 | File I/O: open, close, read, write, lseek, stat, fstat | File operations work |
| I3 | Memory: mmap, munmap, brk, mprotect | Memory allocation works |
| I4 | Process: fork, execve, wait4, exit, getpid | fork/exec round-trip works on GCN |
| I5 | Pipe/dup: pipe2, dup, dup2 | Pipes between GCN processes |

### Phase J: musl libc Deep Port [ONLINE — 2-4 weeks]

| # | Task | Verification |
|---|------|-------------|
| J1 | Port musl internal threading (clone → GCN wavefront spawn) | pthread_create works on GCN |
| J2 | Port musl stdio (buffered I/O) | printf, scanf work |
| J3 | Port musl malloc (dlmalloc or similar) | malloc/free under GCN memory model |
| J4 | Port musl dynamic linker (ld-musl-amdgcn.so) | Shared libraries work on GCN |
| J5 | Signal handling via mailbox (signals are "software" only) | kill(2), signal handlers fire |

### Phase K: Busybox Port [ONLINE — 1-2 weeks]

| # | Task | Verification |
|---|------|-------------|
| K1 | Cross-compile busybox for amdgcn-amd-amdhsa | All applets compile |
| K2 | Test shell (ash) on GCN | Interactive shell works via serial |
| K3 | Test coreutils equivalents (ls, cp, mv, cat, etc.) | Basic file operations |
| K4 | Test networking tools (ifconfig, ping, wget) | Networking over GCN dispatches |
| K5 | Full busybox test suite passed | All enabled applets functional |

### Phase L: Performance Optimization [ONLINE — ongoing]

| # | Task | Verification |
|---|------|-------------|
| L1 | Optimize mailbox polling (reduce x86 to GCN latency) | syscall latency measured |
| L2 | Lazy VGPR save/restore (only save if VGPRs used) | Context switch cost reduced |
| L3 | Large time quanta for GCN threads to amortize dispatch cost | Throughput measured |
| L4 | GCN vector intrinsic examples (demonstrate SIMD throughput) | GFLOPS measured |
| L5 | Batch syscall processing (x86 handles multiple at once) | syscall throughput improved |

---

## Key Invariants

1. **No user thread ever executes on x86 cores** — enforced by `__schedule()` dispatch path
2. **No interrupt ever fires on a GCN CU** — guaranteed by hardware (no IRQ lines)
3. **All privileged operations happen on x86 in kernel context** — mailbox protocol ensures this
4. **Unified address space** — hUMA, page tables shared between x86 and GCN
5. **Syscall path** — GCN → mailbox → x86 poller → `do_syscall_64()` → result → GCN

## Hard Problems (Ranked)

1. **GCN wavefront ≠ CPU thread**: 64 lanes in lockstep. Single-threaded code needs scalar ALU paths for control flow divergence.
2. **No hardware call stack**: GCN uses scratch memory — must be managed by compiler/linker.
3. **Page fault handling**: GCN page faults routed via KFD trap → x86 handler. amdkfd already supports this; we integrate, not reinvent.
4. **Context switch cost**: GCN register file is large. Mitigation: large time quanta, lazy VGPR save.
5. **libc port**: musl assumes x86 syscall ABI. Each arch-specific file replaced with GCN mailbox shim.

## What Exists and Must NOT Be Rewritten

- **amdkfd** — upstream Linux kernel driver for AMD HSA
- **libhsakmt** — HSA kernel-mode thunk library API
- **HSA ABI** — AMD-documented queue/dispatch packet format
- **GCN ISA** — fully documented by AMD
- **LLVM amdgcn backend** — compiles C to GCN machine code

---

## File Layout

```
APUkernel/
├── Makefile              # Top-level build orchestration
├── Roadmap.md            # This file
├── include/
│   ├── mailbox.h         # Shared mailbox struct (both targets)
│   ├── gcn_thread.h      # Per-thread state structure
│   └── protocol.h        # Syscall number mappings, constants
├── gcn/                  # amdgcn target code
│   ├── hello_gcn.S       # Minimal GCN kernel
│   ├── mailbox.S         # GCN assembly mailbox primitives
│   ├── trap_handler.S    # GCN trap handler
│   ├── crt0_gcn.S        # _start entry point
│   ├── syscall_shim.c    # __gcnsyscall implementation
│   ├── gcnsyscall.h      # GCN_SYSCALL macros
│   ├── linker.ld         # GCN executable linker script
│   └── libc/             # Ported musl source files
├── kernel/               # x86 Linux kernel module
│   ├── kbuild.mk         # Kbuild makefile
│   ├── heteroken_main.c  # Module init/exit
│   ├── hsa_queue.c       # HSA queue management via KFD
│   ├── scheduler.c       # Thread scheduler
│   ├── context.c         # GCN context save/restore
│   ├── dispatch.c        # AQL packet dispatch to GCN CU
│   ├── mailbox_handler.c # Syscall poller (x86 side)
│   ├── page_fault.c      # GCN page fault handler
│   ├── procfs.c          # /proc/heteroken interface
│   └── aql.c             # AQL packet construction helpers
├── tests/                # Unit/integration tests (x86 host)
│   ├── test_mailbox.c    # Mailbox protocol unit tests
│   ├── test_scheduler.c  # Scheduler logic tests
│   └── test_mock.c       # Mock HSA dispatch for integration tests
├── rootfs/               # Root filesystem build scripts
│   ├── mkrootfs.sh       # debootstrap + customize script
│   ├── init.c            # Minimal init (x86 bootstrap)
│   └── gcn_init.c        # GCN-compiled /sbin/init
├── pxe/                  # PXE + NFS server config
│   ├── dhcpd.conf
│   ├── grub.cfg
│   └── exports
└── docs/                 # Design documents, notes
    └── gcn_isa_notes.md  # GCN ISA references and gotchas
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
