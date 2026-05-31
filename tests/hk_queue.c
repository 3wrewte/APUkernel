/*
 * hk_queue.c — HeteroKern: create HSA queue via libhsakmt (correct API)
 *
 * 1. Allocate ring + wptr + rptr via hsaKmtAllocMemory
 * 2. Map to GPU
 * 3. Create queue with pre-allocated ring
 * 4. Report doorbell/wptr/rptr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <hsakmt/hsakmt.h>
#include <hsakmt/hsakmttypes.h>

#define GPU_ID  1
#define RING_BYTES (64U * 1024)
#define PAGE      4096

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

int main(void) {
	HSAKMT_STATUS st;

	st = hsaKmtOpenKFD();
	if (st) die("hsaKmtOpenKFD");
	printf("[OK] KFD opened\n");

	/* Allocate ring buffer */
	HsaMemFlags rf = {{0}};
	rf.ui32.HostAccess = 1;
	rf.ui32.CoarseGrain = 1;
	void *ring;
	st = hsaKmtAllocMemory(GPU_ID, RING_BYTES, rf, &ring);
	if (st) die("hsaKmtAllocMemory ring");
	st = hsaKmtMapMemoryToGPU(ring, RING_BYTES, NULL);
	if (st) die("hsaKmtMapMemoryToGPU ring");
	printf("[OK] Ring allocated at %p\n", ring);

	/* Create queue */
	HsaQueueResource q = {{0}};
	st = hsaKmtCreateQueue(GPU_ID, HSA_QUEUE_TYPE_MULTI,
			       100, 1, ring, RING_BYTES, NULL, &q);
	if (st) { printf("CreateQueue FAILED (err=%d)\n", st); return 1; }

	printf("[OK] Queue created: id=%lu\n", (unsigned long)q.QueueId);
	printf("     doorbell=%p  wptr=%p  rptr=%p\n",
	       (void *)q.Queue_DoorBell,
	       (void *)q.Queue_write_ptr,
	       (void *)q.Queue_read_ptr);

	hsaKmtDestroyQueue(q.QueueId);
	hsaKmtFreeMemory(ring, RING_BYTES);
	hsaKmtCloseKFD();
	return 0;
}
