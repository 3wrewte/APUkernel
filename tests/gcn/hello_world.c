/*
 * hello_world.c — target GCN program for HeteroKern Phase 3+
 *
 * This is what we WANT to run. Right now we can't — we need:
 *   1. hk_syscallN() mailbox shim (implemented in gcn/mailbox.S for gfx902)
 *   2. CPU-side syscall dispatcher in the kernel module
 *   3. clone3(CLONE_GCN) to create the task_struct
 *
 * Once those exist, compiling and running this gives:
 *   $ ./host_runner hello_world.co
 *   hello from CU
 *   GCN task exited with status 0
 *
 * Compile (future):
 *   clang --target=amdgcn-amd-amdhsa -mcpu=gfx902 \
 *         -I./tests/gcn -c hello_world.c -o hello_world.o
 *   ld.lld -shared hello_world.o -o hello_world.hsaco
 *
 * The .hsaco is then loaded by the host runner and dispatched via
 * clone3(CLONE_GCN).
 */

#include "mailbox.h"

/*
 * hk_kernel_main — entry point called by the CPU dispatcher.
 *
 * The mailbox pointer is in s[0:1] (passed via COMPUTE_USER_DATA_0/1,
 * same as our current shader convention).
 *
 * This function:
 *   1. Calls write(1, "hello from CU\n", 14) via mailbox
 *   2. Calls exit_group(0) via mailbox
 *
 * The CPU-side task (the GCN task's shadow) executes the actual
 * syscall handlers — the GCN wavefront just fills the mailbox and
 * waits for the CPU to do the work.
 */
__attribute__((hk_kernel))
void hk_kernel_main(void *mailbox_ptr)
{
	volatile struct hk_mailbox *mb = mailbox_ptr;

	/* write(1, "hello from CU\n", 14) */
	hk_syscall3(
		/* SYS_write = 1 */  1,
		/* fd = stdout */     1,
		/* buf */             (long)"hello from CU\n",
		/* count */           14
	);
	/*
	 * What actually happens here:
	 *   hk_syscall3 expands to:
	 *     flat_store_dword 1 at mb+0x04       // syscall_nr
	 *     flat_store_dwordx2 {1, 0} at mb+0x08  // arg0 (fd)
	 *     flat_store_dwordx2 {msg_addr_lo, msg_addr_hi} at mb+0x10  // arg1
	 *     flat_store_dwordx2 {14, 0} at mb+0x18  // arg2
	 *     flat_store_dword 1 at mb+0x00       // state = SYSCALL
	 *   CPU wakes, reads mailbox, calls:
	 *     ret = do_syscall_64(1, 1, "hello from CU\n", 14, 0, 0, 0)
	 *   CPU writes retval at mb+0x38, sets state=0
	 *   CPU re-dispatches this wavefront
	 *   We resume here, maybe check retval
	 */

	/* exit_group(0) */
	hk_syscall1(
		/* SYS_exit_group = 231 */ 231,
		/* status = 0 */            0
	);
	/*
	 * CPU-side handler:
	 *   do_syscall_64(231, 0, ...) → do_exit(0)
	 *   The GCN task_struct is destroyed.
	 *   The wavefront is preempted (CWSR) or completes via s_endpgm.
	 */

	__builtin_unreachable();
}


/* --------------------------------------------------------------------------
 * Alternative: a GCN program that does real computation
 *
 * compute_pi() — estimate π via Monte Carlo on GPU, return result via mailbox.
 * Proves: GCN can do math, write results, and the CPU sees correct data.
 * --------------------------------------------------------------------------
 */
#ifdef NOT_YET
__attribute__((hk_kernel))
void compute_pi(void *mailbox_ptr)
{
	volatile struct hk_mailbox *mb = mailbox_ptr;
	volatile uint64_t *result_buf = (void *)((char *)mb + 256);
	/* The CPU allocated an extra page for results after the mailbox.
	   Same GPU VA, so we access it via flat_load/store. */

	/* Monte Carlo pi estimation */
	uint64_t inside = 0;
	for (int i = 0; i < 1000000; i++) {
		float x = hk_random();  /* would need a GPU RNG */
		float y = hk_random();
		if (x*x + y*y <= 1.0f) inside++;
	}
	float pi = 4.0f * (float)inside / 1000000.0f;
	result_buf[0] = *(uint64_t *)&pi; /* bitcast float→uint64 */

	/* Report result via write() */
	char buf[64];
	int len = hk_snprintf(buf, sizeof(buf), "pi ≈ %f\n", pi);
	hk_syscall3(SYS_write, 1, (long)buf, len);

	hk_syscall1(SYS_exit_group, 0);
}
#endif /* NOT_YET */
