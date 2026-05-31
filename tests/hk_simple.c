/*
 * hk_simple.c — simplest possible KFD test via libhsakmt
 *
 * Tests hsaKmtCreateQueue on GFX7 and reports results.
 * Compile: gcc -O2 -Wall -o hk_simple hk_simple.c -lhsakmt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <hsakmt/hsakmt.h>
#include <hsakmt/hsakmttypes.h>

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

int main(void) {
	HSAKMT_STATUS st;

	/* Open KFD */
	st = hsaKmtOpenKFD();
	if (st != HSAKMT_STATUS_SUCCESS)
		die("hsaKmtOpenKFD");
	printf("[OK] KFD opened\n");

	/* Try creating queue with different types */
	HSAuint32 gpu_id = 1; /* node 1 = GPU */

	/* Try COMPUTE (PM4 packets on GFX7) */
	HsaQueueResource q;
	memset(&q, 0, sizeof(q));

	for (int qtype = 0; qtype < 3; qtype++) {
		const char *tname = qtype == 0 ? "MULTI" :
		                    qtype == 1 ? "SINGLE" : "COOPERATIVE";
		memset(&q, 0, sizeof(q));
		st = hsaKmtCreateQueue(gpu_id, qtype, 100, 1, NULL, &q);
		printf("[%s] CreateQueue: %s",
		       tname, st == HSAKMT_STATUS_SUCCESS ? "OK" : "FAIL");
		if (st == HSAKMT_STATUS_SUCCESS) {
			printf("  id=%u  db=%p  wptr=%p  rptr=%p\n",
			       q.QueueId, (void*)q.Queue_DoorBell,
			       (void*)q.Queue_write_ptr,
			       (void*)q.Queue_read_ptr);
			hsaKmtDestroyQueue(q.QueueId);
		} else {
			printf("  (error code: %d)\n", st);
		}
	}

	hsaKmtCloseKFD();
	return 0;
}
