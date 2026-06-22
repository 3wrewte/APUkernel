# Architecture Decisions & Current Challenges

**Created**: 2026-06-16
**Status**: Active — documents open decisions and technical challenges

---

## 1. Dispatch Path: Kernel Compute Ring vs KFD Queue

### Decision: Use kernel compute ring + IB (VMID 0)

**Context**: We need to dispatch GCN wavefronts from the kernel module.

**Options considered**:

| Path | Mechanism | VMID | Status |
|------|-----------|------|--------|
| A. KFD PM4 queue | PACKET3_COMPUTE in KFD ring, hack WPTR | 8 (per-process) | ❌ SPI never creates waves |
| B. Kernel compute ring + IB | PACKET3 in IB via `amdgpu_ib_schedule` | 0 (kernel) | ✅ Working |

**Why KFD queue failed**:

The CP processed all PM4 packets (rptr = wptr), all compute registers were
correctly programmed (verified by post-dispatch MMIO readback), no VM faults,
`COMPUTE_STATIC_THREAD_MGMT_SE0 = 0xFFFFFFFF`, `DISPATCH_INITIATOR = 0x1` —
but `SPI_CSQ_WF_ACTIVE_COUNT_0 = 0`. The SPI never received the dispatch
request.

**Root cause hypothesis**: MEC firmware requires the queue to be explicitly
"mapped" via a KIQ `MAP_QUEUES` packet before DISPATCH_DIRECT packets are
forwarded to the SPI. The kernel compute ring bypasses this because it was
permanently mapped during driver initialization.

**Impact of using VMID 0**:
- Mailbox BO must be kernel-accessible (GTT memory via `amdgpu_bo_create_kernel`)
- User-space pointers are NOT valid for GPU (no per-process page tables)
- Future per-process features (CoW, SVM, signal handling) need VMID 8

**Future fix**: Investigate KIQ `MAP_QUEUES` via
`kfd_device_queue_manager.c:execute_queues`. Once working, swap
`hk_dispatch_ctx()` internals to use KFD queue with PACKET3_COMPUTE.

---

## 2. Syscall Notification: s_endpgm vs s_sendmsg(INTERRUPT)

### Decision: Use s_sendmsg for syscall boundaries; keep s_endpgm for final exit

**Context**: A GCN wavefront needs to notify the CPU that it has written
syscall parameters to the mailbox and is waiting for the CPU to execute
the syscall.

#### Option A: s_endpgm (old/fallback approach)

```
Wavefront:
  1. Write syscall_nr + args to mailbox (flat_store)
  2. Write state = SYSCALL_PENDING (flat_store)
  3. s_endpgm                          ← wavefront dies

CPU (IH interrupt from fence):
  4. Read mailbox
  5. Execute syscall (e.g., kernel_write)
  6. Write retval to mailbox
  7. Re-dispatch wavefront             ← brand new wavefront from scratch
```

**Problem**: Each `s_endpgm` kills the wavefront. All register state is lost.
The re-dispatched wavefront runs the SAME shader code from the beginning.
To simulate "continuing after the syscall", we use a `step` counter in the
mailbox — the shader reads `step` on each dispatch and branches to the
correct phase. This is equivalent to RISC-V saving/restoring PC across
`ecall`, but implemented in software and far more expensive (~5ms per
round-trip vs ~100ns for a real ecall).

**Why we can't just busy-wait**: If the wavefront doesn't call `s_endpgm`,
the fence never signals, the IH interrupt never fires, and the CPU never
knows the wavefront wrote anything. `flat_store` is just a memory write —
it doesn't trigger any notification. The CPU would have to poll, which
wastes CPU cycles.

#### Option B: s_sendmsg sendmsg(INTERRUPT) (current approach)

```
Wavefront:
  1. Write syscall_nr + args to mailbox
  2. s_sendmsg sendmsg(INTERRUPT)       ← trap/interrupt, wavefront stays alive
  3. Busy-wait: flat_load mailbox.state until CPU clears it
  4. flat_load mailbox.retval           ← read return value
  5. Continue to next instruction        ← registers preserved!
```

**Verified on 2026-06-22**:

1. `s_sendmsg sendmsg(MSG_INTERRUPT)` assembles on gfx902 as `0xBF900001`.

2. Without trap setup, `s_sendmsg` stops the wavefront. `sendmsg_exit.co`
   timed out with `state=0` forever, proving the instruction does not simply
   fall through under default PM4 state.

3. Minimal trap setup is sufficient for fall-through:
   - Set `COMPUTE_PGM_RSRC2.TRAP_PRESENT = 1`
   - Program `SQ_SHADER_TBA_LO/HI` to a trap handler in the same IB
   - Trap handler body:
     ```asm
     s_mov_b64 s[0:1], ttmp[0:1]
     s_rfe_b64 s[0:1]
     ```

4. CPU→GPU mailbox return requires GPU-side cache invalidation. The live
   shader must execute `buffer_wbinvl1_vol` before polling `mailbox.state`;
   otherwise it keeps reading stale `state=1` and never observes the CPU's
   `state=0` write.

5. Full live syscall proof works:
   - GPU writes `write(1, "hello from CU\n", 14)` request
   - GPU executes `s_sendmsg(MSG_INTERRUPT)` and returns through TBA handler
   - CPU polling path sees `state=1`, calls `kernel_write()`, writes retval,
     clears state, flushes mailbox fields
   - GPU invalidates cache, sees `state=0`, continues, writes `state=3`,
     and exits via final `s_endpgm`

**Remaining limitation**: The CPU currently polls the mailbox. We have proven
the GPU-side live-wavefront semantics, but have not yet routed the `s_sendmsg`
event through amdgpu IH to wake a sleeping CPU task. That is now the next
interrupt-routing task; it no longer blocks correctness of syscall return.

---

## 3. clone3(CLONE_GCN) vs fork() + ioctl

### Decision: Defer clone3 hook; use fork() + /dev/heteroken ioctl

**Context**: We need real `task_struct` semantics for GCN tasks (own mm,
fd table, creds, signal mask).

**Current approach**:
```c
// host_runner.c
pid_t pid = fork();           // child gets CoW mm, own task_struct
if (pid == 0) {
    // Child: open /dev/heteroken, dispatch shader via ioctl
    ioctl(hkfd, HK_IOCTL_RUN, &shader_req);
    _exit(0);
}
waitpid(pid, NULL, 0);        // parent waits
```

**Why this works**:
- `fork()` creates a real `task_struct` with dup'd mm — exactly what we need
- The child process can open files, call syscalls, be killed by signals
- The ioctl handler dispatches the shader and waits for completion

**Why clone3(CLONE_GCN) is deferred**:
- Modifying `kernel/fork.c` is invasive and requires careful placement
- The fork+ioctl approach gives us 90% of the functionality with 0% of the
  core kernel changes
- The child starts on CPU (calls ioctl) rather than starting on GPU —
  this is a minor semantic difference that doesn't affect correctness

**When to revisit clone3**:
- When we need the child to NEVER run user code on CPU (true GPU-first birth)
- When we need kernel-level GCN task detection for signal handling
- When the CWSR preemption path needs to identify GCN tasks by flag

---

## 4. Mailbox Protocol: Dispatch Loop Design

### Decision: CPU-side dispatch loop with step counter

**Context**: Since each syscall kills the wavefront (s_endpgm), the CPU
must re-dispatch the shader after each syscall. The shader needs to know
which "phase" to execute on each dispatch.

**Protocol**:
```
Mailbox layout (256 bytes):
  +0x00: state    (0=idle, 1=syscall_pending, 3=exit)
  +0x04: step     (shader phase counter — software PC)
  +0x08: syscall_nr
  +0x10: arg0     (64-bit)
  +0x18: arg1     (64-bit)
  +0x20: arg2     (64-bit)
  +0x38: retval   (64-bit, written by CPU)
  +0x40: data[192] (string/data buffer)

CPU dispatch loop (in ioctl handler):
  for round in 0..15:
    1. Allocate fresh IB, copy shader, build PM4
    2. amdgpu_ib_schedule → fence
    3. dma_fence_wait_timeout (10s)
    4. Read mailbox.state
    5. If state == SYSCALL_PENDING:
       - Read syscall_nr + args
       - Execute syscall (switch on nr)
       - Write retval, clear state
       - Continue (re-dispatch)
    6. If state == EXIT: break
    7. If state == 0: break (pure data, no syscall)
```

**Limitations**:
- Maximum 16 syscalls per program (loop limit)
- ~5ms latency per syscall round-trip (IB submit → GPU → fence → CPU)
- Shader must be written in assembly with explicit step branching
- No stack, no heap — all state lives in the mailbox

---

## 5. Memory Management: hUMA vs NUMA Node

### Decision: No NUMA node abstraction for hUMA; rely on IOMMUv2 + handle_mm_fault

**Context**: The 2200G APU is hUMA — GPU and CPU share the same DDR4.

On hUMA, CoW works automatically through the existing Linux mm infrastructure:
```
GPU page fault (unmapped CoW page)
  → IOMMUv2 PPR (Peripheral Page Request)
  → handle_mm_fault() on the task's mm
  → alloc page, copy, update PTE
  → GPU retries (XNACK)
```

No NUMA node abstraction needed for hUMA. The NUMA node concept becomes
relevant only for dGPU with discrete VRAM (Phase 11, future).

---

## 6. Open Questions

### Q1: Can we use the same IB for multiple dispatches?

Currently we allocate a fresh IB for each round in the dispatch loop.
This is safe but wasteful (~512 bytes allocated/freed per round).
Re-using the same IB may work but `amdgpu_ib_schedule` may not handle
re-submission cleanly. Needs testing.

### Q2: What happens if the GPU faults during shader execution?

On VMID 0 (kernel compute ring), a GPU page fault generates an IH
interrupt that the amdgpu driver logs but doesn't route to us. The
wavefront hangs (no s_endpgm), the fence times out after 10s, and
the ioctl returns -ETIMEDOUT. The GPU is not reset — subsequent
dispatches may still work.

Future: Hook into the VM fault handler to detect shader faults and
report them as SIGSEGV to the GCN task.

### Q3: Can we pass user-space pointers to the GPU?

Not with VMID 0. The GPU uses kernel page tables (VMID 0), which don't
map user-space addresses. Workaround: copy data to/from the mailbox
BO (kernel memory) in the ioctl handler.

With VMID 8 (KFD queue), user-space pointers would be valid via
IOMMUv2 SVM — but we need to solve the MAP_QUEUES blocker first.

### Q4: How many VGPRs do our shaders need?

Currently `HK_VGPRS = 1` → (1+1)*4 = 8 VGPRs. The syscall shader
uses v0-v4 (5 registers). More complex shaders may need more.
The VGPR count must be set correctly in `COMPUTE_PGM_RSRC1` or
the wavefront will fault on register access.

---

## Summary Decision Matrix

| Decision | Current | Target | Prerequisite |
|----------|---------|--------|-------------|
| Dispatch path | Kernel ring + IB (VMID 0) | KFD queue (VMID 8) | KIQ MAP_QUEUES |
| Syscall notification | s_sendmsg + trap return + CPU polling | IH-routed wakeup | amdgpu IH hook |
| Task creation | fork() + ioctl | clone3(CLONE_GCN) | Core kernel hook |
| Memory model | Mailbox BO (kernel memory) | User-space SVM | VMID 8 |
| Step tracking | Hardware register state across syscall | Same | Done for live-wavefront path |
