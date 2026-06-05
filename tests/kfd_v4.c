/*
 * kfd_v4.c — single buffer approach: ring + wptr + rptr in one allocation
 *
 * Theory: maybe the 3 separate MAP_MEMORY_TO_GPU calls create
 * attachments that the queue code doesn't find.  Using one buffer
 * reduces the problem space.
 *
 * Layout within one 64KB buffer at GPUVA 0x100000:
 *   [0x00000 - 0x07FFF]  ring buffer (32KB)
 *   [0x08000]            wptr (4 bytes)
 *   [0x08008]            rptr (4 bytes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kfd_ioctl.h>

#define GPU_ID      43858
#define PAGE        4096
#define RING_BYTES  (32U * 1024)
#define BUF_SIZE    (RING_BYTES + PAGE)  /* ring + wptr/rptr page */

#define GPUVA_BASE  0x1000000
#define GPUVA_WPTR  (GPUVA_BASE + RING_BYTES)
#define GPUVA_RPTR  (GPUVA_BASE + RING_BYTES + 8)

static int kfd_fd, drm_fd;

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s (%s)\n", s, strerror(errno));
	exit(1);
}

int main(void)
{
	printf("=== KFD v4: single-buffer queue test ===\n\n");

	kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (kfd_fd < 0) die("/dev/kfd");
	drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) die("renderD128");

	/* 1. Acquire VM */
	struct kfd_ioctl_acquire_vm_args vm = {
		.drm_fd = (__u32)drm_fd, .gpu_id = GPU_ID };
	if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &vm) < 0)
		die("ACQUIRE_VM");
	printf("[1] VM acquired\n");

	/* 2. Allocate single buffer at GPUVA */
	struct kfd_ioctl_alloc_memory_of_gpu_args a = {
		.va_addr = GPUVA_BASE, .size = BUF_SIZE, .gpu_id = GPU_ID,
		.flags = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		         KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &a) < 0)
		die("ALLOC");

	void *cpu = mmap(NULL, BUF_SIZE, PROT_READ|PROT_WRITE,
			 MAP_SHARED, drm_fd, a.mmap_offset);
	if (cpu == MAP_FAILED) die("mmap");
	printf("[2] Allocated %zu bytes at GPUVA=0x%lx CPU=%p handle=0x%lx\n",
	       (unsigned long)BUF_SIZE, (unsigned long)GPUVA_BASE, cpu,
	       (unsigned long)a.handle);

	/* 3. Map to GPU VM */
	uint32_t gpus[] = { GPU_ID };
	struct kfd_ioctl_map_memory_to_gpu_args m = {
		.handle = a.handle,
		.device_ids_array_ptr = (uint64_t)(uintptr_t)gpus,
		.n_devices = 1, .n_success = 0,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) == 0)
		printf("[3] Mapped to GPU VM\n");
	else
		printf("[3] MAP FAILED: %s\n", strerror(errno));

	/* 4. Create queue */
	struct kfd_ioctl_create_queue_args q = {
		.ring_base_address       = GPUVA_BASE,
		.write_pointer_address   = GPUVA_WPTR,
		.read_pointer_address    = GPUVA_RPTR,
		.ring_size               = RING_BYTES,
		.gpu_id                  = GPU_ID,
		.queue_type              = KFD_IOC_QUEUE_TYPE_COMPUTE,
		.queue_percentage        = 100,
		.queue_priority          = 1,
	};

	printf("[4] Creating queue...\n");
	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q) == 0) {
		printf("    SUCCESS!  id=%u  db=0x%lx\n",
		       q.queue_id, (unsigned long)q.doorbell_offset);
		printf("    ring@0x%lx wp@0x%lx rp@0x%lx\n",
		       (unsigned long)q.ring_base_address,
		       (unsigned long)q.write_pointer_address,
		       (unsigned long)q.read_pointer_address);
	} else {
		printf("    FAILED: %s\n", strerror(errno));
	}

	close(kfd_fd);
	close(drm_fd);
	return 0;
}
