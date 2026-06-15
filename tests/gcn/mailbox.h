/*
 * mailbox.h — shared mailbox protocol definition
 *
 * This header is compiled for BOTH targets:
 *   x86:    included by the kernel module (heteroken.c) and test host programs
 *   amdgcn: included by GCN kernels running on GPU wavefronts
 *
 * The mailbox is a 128-byte cacheline-aligned structure in GPU-accessible
 * GTT memory. The GCN wavefront writes to it, the CPU task reads it.
 *
 * Protocol (GPU→CPU):
 *   1. GCN writes syscall_nr + args to mailbox
 *   2. GCN sets state = HK_MB_STATE_SYSCALL
 *   3. GPU writes trigger IH interrupt via dma_fence callback
 *   4. CPU task wakes, reads mailbox, executes do_syscall_64()
 *   5. CPU writes retval, sets state = HK_MB_STATE_IDLE
 *   6. CPU re-submits GCN dispatch, sleeps until next interrupt
 *
 * Protocol (CPU→GPU):
 *   1. CPU writes data to a separate "return buffer" shared page
 *   2. CPU re-dispatches the GCN wavefront
 *   3. GCN reads the return data from buffer
 */

#ifndef HK_MAILBOX_H
#define HK_MAILBOX_H

#include <stdint.h>

/* Mailbox states */
#define HK_MB_STATE_IDLE            0
#define HK_MB_STATE_SYSCALL         1   /* GPU requests syscall execution */
#define HK_MB_STATE_TRAP            2   /* GPU reports a trap/fault */
#define HK_MB_STATE_EXIT            3   /* GPU is done, task should exit */

/* Full mailbox layout (128 bytes, cacheline-aligned) */
struct hk_mailbox {
	uint32_t state;             /* +0x00: HK_MB_STATE_* */

	/* Syscall request (valid when state == HK_MB_STATE_SYSCALL) */
	uint32_t syscall_nr;        /* +0x04: linux syscall number */
	uint32_t _pad0;
	uint64_t args[6];           /* +0x08: syscall arguments (6×8 = 48 bytes) */

	/* Syscall result (written by CPU, valid when state == HK_MB_STATE_IDLE) */
	uint64_t retval;            /* +0x38: return value */
	uint64_t error_code;        /* +0x40: errno (0 = success) */

	/* Fault report (valid when state == HK_MB_STATE_TRAP) */
	uint64_t fault_addr;        /* +0x48: page fault virtual address */
	uint32_t fault_type;        /* +0x50: 0=read, 1=write, 2=exec */
	uint32_t _pad1;

	/* Padding to 128 bytes */
	uint32_t _reserved[9];      /* +0x58: 36 bytes padding */
} __attribute__((aligned(128)));

_Static_assert(sizeof(struct hk_mailbox) == 128, "mailbox must be 128 bytes");

/* --------------------------------------------------------------------------
 * GCN-side helpers (compiled by clang --target=amdgcn-amd-amdhsa)
 *
 * These would be implemented in assembly (gcn/mailbox.S) and provide
 * the low-level flat_store / flat_load / s_waitcnt sequence for
 * communicating with the CPU via the mailbox.
 * --------------------------------------------------------------------------
 *
 * void hk_mb_write_state(volatile struct hk_mailbox *mb, uint32_t state);
 *   // flat_store_dword state at mb+0x00
 *
 * uint32_t hk_mb_read_state(volatile struct hk_mailbox *mb);
 *   // s_waitcnt vmcnt(0); return mb->state
 *
 * void hk_mb_write_syscall(volatile struct hk_mailbox *mb,
 *                          uint32_t nr, uint64_t a0,...);
 *   // write nr at mb+0x04, args[] at mb+0x08..0x38, state=1 at mb+0x00
 *
 * uint64_t hk_mb_read_retval(volatile struct hk_mailbox *mb);
 *   // s_waitcnt vmcnt(0); return mb->retval
 */

/* --------------------------------------------------------------------------
 * GCN syscall macros (for GCN kernel code)
 *
 * These expand to inline assembly sequences that write to the mailbox
 * and signal the CPU. Example usage:
 *
 *   __attribute__((hk_kernel))
 *   void my_gcn_program(void *arg) {
 *       register struct hk_mailbox *mb asm("s2") = arg;
 *       hk_syscall3(SYS_write, 1, (long)"hello\n", 6);
 *       hk_syscall1(SYS_exit_group, 0);
 *   }
 *
 * The actual implementation requires:
 *   1. Mailbox pointer in an SGPR (passed via USER_DATA or kernel arg)
 *   2. flat_store sequences for each mailbox field
 *   3. A signal/doorbell write to wake the CPU
 * --------------------------------------------------------------------------
 */

#ifdef __AMDGCN__
/* Placeholder — actual implementation in gcn/mailbox.S */
#define hk_syscall1(nr, a0) \
	__asm__ __volatile__(";; hk_syscall1 stub" :: "r"(nr), "r"(a0))
#define hk_syscall3(nr, a0, a1, a2) \
	__asm__ __volatile__(";; hk_syscall3 stub" :: "r"(nr), "r"(a0), "r"(a1), "r"(a2))
#endif /* __AMDGCN__ */

#endif /* HK_MAILBOX_H */
