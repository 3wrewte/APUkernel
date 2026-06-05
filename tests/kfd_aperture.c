/*
 * kfd_aperture.c — Get process apertures to find GPU VA range
 *
 * Steps:
 *   1. Open /dev/kfd, DRM render node, acquire VM
 *   2. Get process apertures to learn GPU VM range
 *   3. Allocate ring/wptr/rptr buffers at GPU VA addresses within range
 *   4. Map buffers to GPU VM via MAP_MEMORY_TO_GPU
 *   5. Create AQL queue with GPU VA addresses
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

#define GPU_ID    43858
#define PAGE_SIZE 4096
#define RING_SIZE (64U * 1024)

static int kfd_fd, drm_fd;

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

int main(void) {
	printf("=== HeteroKern Aperture Test ===\n\n");

	kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (kfd_fd < 0) die("open /dev/kfd");
	drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) die("open renderD128");

	/* 1. Acquire VM */
	struct kfd_ioctl_acquire_vm_args vm = {
		.drm_fd = (__u32)drm_fd, .gpu_id = GPU_ID };
	if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &vm) < 0)
		die("ACQUIRE_VM");
	printf("[1] VM acquired\n");

	/* 2. Get process apertures to find GPU VA range */
	struct kfd_ioctl_get_process_apertures_new_args ap = {
		.kfd_process_device_apertures_ptr = 0,
		.num_of_nodes = 8,
	};
	struct kfd_process_device_apertures
		aps[8] __attribute__((aligned(8))) = {{0}};
	ap.kfd_process_device_apertures_ptr = (uint64_t)(uintptr_t)aps;

	if (ioctl(kfd_fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &ap) < 0)
		die("GET_PROCESS_APERTURES_NEW");

	printf("[2] Process apertures: num_nodes=%u\n", ap.num_of_nodes);
	for (int i = 0; i < ap.num_of_nodes; i++) {
		printf("  GPU %u: lds=0x%lx-0x%lx  scratch=0x%lx-0x%lx  "
		       "gpuvm=0x%lx-0x%lx\n",
		       aps[i].gpu_id,
		       (unsigned long)aps[i].lds_base,
		       (unsigned long)aps[i].lds_limit,
		       (unsigned long)aps[i].scratch_base,
		       (unsigned long)aps[i].scratch_limit,
		       (unsigned long)aps[i].gpuvm_base,
		       (unsigned long)aps[i].gpuvm_limit);
	}

	/* Find our GPU's aperture */
	uint64_t gpuvm_base = 0, gpuvm_limit = 0;
	for (int i = 0; i < ap.num_of_nodes; i++) {
		if (aps[i].gpu_id == GPU_ID || ap.num_of_nodes == 1) {
			gpuvm_base = aps[i].gpuvm_base;
			gpuvm_limit = aps[i].gpuvm_limit;
			break;
		}
	}

	if (gpuvm_base == 0 && gpuvm_limit == 0) {
		/* GPU might not be in apertures list; use the first GPU */
		if (ap.num_of_nodes > 0) {
			gpuvm_base = aps[0].gpuvm_base;
			gpuvm_limit = aps[0].gpuvm_limit;
			printf("  Using first GPU: gpu_id=%u\n", aps[0].gpu_id);
		} else {
			printf("  No GPUs in apertures list!\n");
		}
	}

	if (gpuvm_base) {
		printf("\n  GPU VM range: 0x%lx - 0x%lx (size 0x%lx)\n",
		       (unsigned long)gpuvm_base,
		       (unsigned long)gpuvm_limit,
		       (unsigned long)(gpuvm_limit - gpuvm_base));
	}

	/* 3. Try allocating at a specific GPU VA in the range */
	if (gpuvm_base) {
		uint64_t va_start = gpuvm_base; /* use the start of GPU VM range */
		struct kfd_ioctl_alloc_memory_of_gpu_args alloc = {
			.va_addr = va_start,
			.size    = PAGE_SIZE,
			.gpu_id  = GPU_ID,
			.flags   = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
			           KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE,
		};

		int ret = ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &alloc);
		if (ret == 0) {
			printf("[3] Alloc at GPU VA 0x%lx: OK  handle=0x%lx  "
			       "mmap_off=0x%lx  ret_va=0x%lx\n",
			       (unsigned long)va_start,
			       (unsigned long)alloc.handle,
			       (unsigned long)alloc.mmap_offset,
			       (unsigned long)alloc.va_addr);
		} else {
			printf("[3] Alloc at GPU VA 0x%lx: FAILED (%s)\n",
			       (unsigned long)va_start, strerror(errno));
		}
	}

	/* 4. Try creating queue with ring_base_address = 0 (KFD auto-allocates?) */
	/* For COMPUTE queue type (not AQL), KFD might auto-allocate buffers */
	if (1) {
		struct kfd_ioctl_create_queue_args q = {
			.ring_size        = RING_SIZE,
			.gpu_id           = GPU_ID,
			.queue_type       = KFD_IOC_QUEUE_TYPE_COMPUTE, /* non-AQL */
			.queue_percentage = 100,
			.queue_priority   = 1,
		};
		int ret = ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q);
		printf("[4] CREATE_QUEUE (COMPUTE, non-AQL): %s\n",
		       ret == 0 ? "OK" : "FAIL");
		if (ret == 0) {
			printf("    queue_id=%u  ring=0x%lx  wp=0x%lx  rp=0x%lx  db=0x%lx\n",
			       q.queue_id,
			       (unsigned long)q.ring_base_address,
			       (unsigned long)q.write_pointer_address,
			       (unsigned long)q.read_pointer_address,
			       (unsigned long)q.doorbell_offset);
		}
	}

	close(kfd_fd);
	close(drm_fd);
	return 0;
}
