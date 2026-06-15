# HeteroKern Test Plan — GFX9 / Raven Ridge / 2200G

## Current Infrastructure (2026-06-16)

```
Kernel module:  heteroken.c (in-tree, amdkfd/)
Dispatch:       kernel compute ring + IB  (PACKET3, VMID 0)
Mailbox:        amdgpu_bo_create_kernel() GTT BO, GPU writes via flat_store
IH interrupt:   dma_fence_add_callback → fires in IRQ context → wake_up_process()
GCN task:       kthread_run → hk_dispatch_ctx → schedule() → IH wakes → loop
Interface:      /sys/kernel/heteroken/{run,spawn,stop}
```

### What We CAN Test Now

| # | Test | Mechanism | Proves |
|---|------|-----------|--------|
| T0 | Single-shot dispatch | `echo 1 >/sys/kernel/heteroken/run; dmesg \| grep 'hk:'` | GCN wavefront executes, mailbox protocol works |
| T1 | GCN task lifecycle | `echo 1 > spawn; sleep; echo 1 > stop; dmesg` | sleep→GPU→IH→wake→CPU→repeat loop |
| T2 | Multi-cycle stress | spawn, wait for N cycles, stop, verify no leaks | stable across 100+ GPU→CPU transitions |

### What We CANNOT Test Yet (Needs clone3 + Mailbox syscall shim)

| # | Test | Infrastructure Needed |
|---|------|----------------------|
| T3 | Hello GCN: write(1, "hello") from CU | `hk_syscall3` mailbox shim on GCN side + CPU-side syscall dispatcher |
| T4 | CoW: fork-like clone3, child writes, parent unchanged | `clone3(CLONE_GCN)` hook in `kernel/fork.c` |
| T5 | Signals: SIGKILL → CWSR preempt | KFD queue (VMID 8) + CWSR trap handler |
| T6 | IPC: pipe/futex across ISA boundary | mailbox syscall shim that composes with blocking syscalls |
| T7 | execve: GCN task exec's a new x86 binary | clone3 hook + real task_struct |

## Test 0: GPU Enumeration (Manual Check)

Not a script — just verify the GPU is alive after boot:

```bash
ssh root@2200g "dmesg | grep -E 'hk:.*ready|amdgpu.*gfx902'"
```

Expected:
```
hk: HeteroKern ready (gfx902, phases 1-3)
[drm] amdgpu kernel modesetting enabled
```

The three sysfs nodes should exist:
```
ls /sys/kernel/heteroken/
# run  spawn  stop
```

## Test 1: Single-Shot Dispatch (sysfs)

**File**: `test_sysfs.sh`

Proves: GCN wavefront executes, writes structured mailbox data, CPU reads it.

```bash
echo 1 > /sys/kernel/heteroken/run
sleep 3
dmesg | tail -6 | grep 'hk:'
```

Expected dmesg output:
```
hk: run: magic=0x21544148 state=1 syscall=42 arg0=0xdeadbeef arg1=0xcafebabe
```

## Test 2: GCN Task Lifecycle

**File**: `test_sysfs.sh` (continuation)

Proves: Full async lifecycle — spawn → GPU sleep → IH wake → CPU → repeat → stop.

```bash
echo 1 > /sys/kernel/heteroken/spawn
sleep 2
echo 1 > /sys/kernel/heteroken/stop
sleep 1
dmesg | grep 'hk:' | tail -20
```

Expected dmesg pattern:
```
hk: GCN task spawned: hk-gcn-0 [PID]
hk: GCN task hk-gcn-0 [PID] born
hk: hk-gcn-0 [PID] → GPU (sleep)
hk: *** IH → wake hk-gcn-0 [PID] (IRQ context: yes) ***
hk: hk-gcn-0 [PID] ← GPU (wake)
hk: GCN task mailbox: magic=0x21544148 state=1 syscall=42 ...
hk: GCN task would execute syscall 42
  ... (repeats for each GPU→CPU cycle) ...
hk: GCN task hk-gcn-0 [PID] exiting
hk: GCN task stopped
```

## Test 3: Future — Hello GCN (write syscall from CU)

**File**: `gcn/hello_world.c`

This is the target: a GCN program that calls `write(1, "hello from CU\n", 14)`.
The `write` is implemented via the mailbox protocol — the wavefront writes
syscall_nr + args to the mailbox, triggers IH, the CPU-side task executes
`do_syscall_64(__NR_write, 1, "hello from CU\n", 14)`.

```c
// gcn/hello_world.c — compiled with clang --target=amdgcn-amd-amdhsa -mcpu=gfx902
#include "mailbox.h"

__attribute__((hk_kernel))
void gcn_main(void *arg) {
    hk_syscall3(SYS_write, 1, (long)"hello from CU\n", 14);
    hk_syscall1(SYS_exit_group, 0);
}
```

Required infrastructure:
- `hk_syscallN()` macros — write nr+args to mailbox, set state=1, wait until CPU clears state to 0
- CPU-side dispatcher — read mailbox, call `do_syscall_64()`, write retval, clear state, re-dispatch
- `clone3(CLONE_GCN)` — creates real task_struct with dup'd mm/fd/creds

## Test 4: Future — CoW Address Space

Proves: clone3 with no CLONE_VM gives the GCN child a CoW copy.

```c
// host
char *buf = mmap(..., MAP_SHARED, ...);
strcpy(buf, "parent");
pid_t pid = clone3(&(struct clone_args){
    .flags = CLONE_GCN,
    .gcn_code_fd = fd,
    .gcn_kernel = "cow_test",
    .gcn_arg = buf,
});
waitpid(pid, NULL, 0);
assert(!strcmp(buf, "parent")); // child's CoW write didn't leak
```

The GCN kernel:
```c
void cow_test(void *arg) {
    char *s = arg;
    hk_assert(s[0] == 'p');     // sees parent data at same VA
    s[0] = 'X';                 // triggers CoW — IOMMUv2 PPR → handle_mm_fault()
    hk_assert(s[0] == 'X');     // sees own write
    // exit — parent still sees "parent"
}
```

## Test 5: Future — Signal / CWSR Preemption

Proves: SIGKILL kills a spinning GCN task via CWSR queue preempt.

Requires KFD queue path (VMID 8) + CWSR trap handler.

## Ownership

All tests assume the GCN task is a **real task_struct**:
- `/proc/<pid>/stat` shows it
- `strace -p <pid>` can trace its syscalls
- Its fd table, creds, and mm are its own
- `kill(pid, SIGTERM)` works as expected
