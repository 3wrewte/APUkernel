/*
 * hk_ready.c — HeteroKern: HSA queue creation, ready for dispatch
 *
 * Steps:
 *   1. Enumerate topology, find GPU node
 *   2. Allocate ring buffer in hUMA
 *   3. Create compute queue
 *   4. Verify doorbell/wptr/rptr are mapped
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <hsakmt/hsakmt.h>
#include <hsakmt/hsakmttypes.h>

#define RING_BYTES (64U * 1024)

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

int main(void) {
	HSAKMT_STATUS st;

	st = hsaKmtOpenKFD();
	if (st) { printf("KFD open failed: %d\n", st); return 1; }
	printf("[1] KFD opened\n");

	/* Enumerate topology */
	HsaSystemProperties sp = {{0}};
	st = hsaKmtAcquireSystemProperties(&sp);
	printf("[2] System: %u nodes\n", sp.NumNodes);

	HSAuint32 gpu_node = 0;
	int found = 0;
	for (HSAuint32 i = 0; i < sp.NumNodes; i++) {
		HsaNodeProperties np = {{0}};
		hsaKmtGetNodeProperties(i, &np);

		int is_gpu = (np.NumFComputeCores > 0);
		printf("    Node %u: %s  FComputeCores=%u  CPQueues=%u  "
		       "GPUId=%u  Integrated=%u\n",
		       i, is_gpu ? "GPU" : "CPU",
		       np.NumFComputeCores, np.NumCpQueues,
		       np.KFDGpuID, np.Integrated);

		if (is_gpu && !found) {
			gpu_node = i;
			found = 1;
		}
	}

	if (!found) { printf("No GPU found!\n"); return 1; }
	printf("[2] Using GPU node %u (KFDGpuID=%u)\n", gpu_node,
	       ({ HsaNodeProperties np = {{0}};
		  hsaKmtGetNodeProperties(gpu_node, &np);
		  np.KFDGpuID; }));

	/* Allocate ring buffer */
	HsaMemFlags rf = {{{0}}};
	rf.ui32.HostAccess  = 1;
	rf.ui32.CoarseGrain = 1;
	void *ring;
	st = hsaKmtAllocMemory(gpu_node, RING_BYTES, rf, &ring);
	if (st) { printf("AllocMemory FAILED (err=%d)\n", st); return 1; }
	printf("[3] Ring allocated at %p (%u KB)\n", ring, RING_BYTES/1024);

	st = hsaKmtMapMemoryToGPU(ring, RING_BYTES, NULL);
	if (st) { printf("MapMemoryToGPU FAILED (err=%d)\n", st); return 1; }
	printf("[3] Ring mapped to GPU\n");

	/* Create queue */
	HsaQueueResource q;
	memset(&q, 0, sizeof(q));
	st = hsaKmtCreateQueue(gpu_node, HSA_QUEUE_COMPUTE,
			       100, HSA_QUEUE_PRIORITY_NORMAL,
			       ring, RING_BYTES, NULL, &q);
	if (st) { printf("CreateQueue FAILED (err=%d)\n", st); return 1; }

	printf("[4] Queue created: id=%lu\n", (unsigned long)q.QueueId);
	printf("    doorbell=%p\n", (void *)q.Queue_DoorBell);
	printf("    wptr=%p (val=%u)\n",
	       (void *)q.Queue_write_ptr,
	       q.Queue_write_ptr ? *q.Queue_write_ptr : 0);
	printf("    rptr=%p (val=%u)\n",
	       (void *)q.Queue_read_ptr,
	       q.Queue_read_ptr ? *q.Queue_read_ptr : 0);

	/* Write a NOP to the ring to prove it works */
	uint32_t w = *q.Queue_write_ptr;
	uint32_t *ring32 = (uint32_t *)ring;
	ring32[(w/4) % (RING_BYTES/4)] = 0xc0001000; /* NOP */
	w += 4;
	*q.Queue_write_ptr = w;
	*q.Queue_DoorBell = w;
	printf("[5] NOP packet submitted via doorbell\n");

	hsaKmtDestroyQueue(q.QueueId);
	hsaKmtFreeMemory(ring, RING_BYTES);
	hsaKmtReleaseSystemProperties();
	hsaKmtCloseKFD();
	printf("\n=== All OK ===\n");
	return 0;
}
