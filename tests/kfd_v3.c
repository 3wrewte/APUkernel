/*
 * kfd_v3.c — create queue with COMPUTE (PM4) type, not AQL
 *            + extended diagnostic for GPU VM attach
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
#define RING_BYTES  (32U * 1024)  /* 32KB ring — smaller to test */

/* GPU VAs: spread out to avoid collisions */
#define GPUVA_RING  0x1000000
#define GPUVA_WPTR  0x1010000
#define GPUVA_RPTR  0x1020000

static int kfd_fd, drm_fd;

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

/* alloc+mmap+report */
static void *alloc_at_gpuva(uint64_t gva, size_t sz,
			    uint64_t *handle, const char *name)
{
	struct kfd_ioctl_alloc_memory_of_gpu_args a = {
		.va_addr = gva, .size = sz, .gpu_id = GPU_ID,
		.flags = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		         KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &a) < 0)
		die("ALLOC");

	void *cpu = mmap(NULL, sz, PROT_READ|PROT_WRITE,
			 MAP_SHARED, drm_fd, a.mmap_offset);
	if (cpu == MAP_FAILED) die("mmap");

	printf("  [%s] GPUVA=0x%lx CPU=%p handle=0x%lx\n",
	       name, (unsigned long)gva, cpu, (unsigned long)a.handle);

	*handle = a.handle;
	return cpu;
}

/* map buffer to GPU VM */
static void map_buf(uint64_t handle, const char *name)
{
	uint32_t gpus[] = { GPU_ID };
	struct kfd_ioctl_map_memory_to_gpu_args m = {
		.handle = handle,
		.device_ids_array_ptr = (uint64_t)(uintptr_t)gpus,
		.n_devices = 1, .n_success = 0,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) < 0)
		printf("  %s: MAP FAILED (%s)\n", name, strerror(errno));
	else
		printf("  %s: mapped to GPU VM\n", name);
}

int main(void)
{
	printf("=== KFD v3: COMPUTE queue with GPU VAs ===\n\n");

	kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (kfd_fd < 0) die("/dev/kfd");
	drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) die("renderD128");

	struct kfd_ioctl_acquire_vm_args vm = {
		.drm_fd = (__u32)drm_fd, .gpu_id = GPU_ID };
	if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &vm) < 0)
		die("ACQUIRE_VM");
	printf("[1] VM acquired\n");

	printf("[2] Allocating:\n");
	uint64_t rh, wh, rph;
	alloc_at_gpuva(GPUVA_RING, RING_BYTES, &rh,  "ring");
	alloc_at_gpuva(GPUVA_WPTR, PAGE,       &wh,  "wptr");
	alloc_at_gpuva(GPUVA_RPTR, PAGE,       &rph, "rptr");

	printf("[3] Mapping to GPU VM:\n");
	map_buf(rh,  "ring");
	map_buf(wh,  "wptr");
	map_buf(rph, "rptr");

	/* 4. Try queue creation with COMPUTE type ONLY */
	struct kfd_ioctl_create_queue_args q = {
		.ring_base_address       = GPUVA_RING,
		.write_pointer_address   = GPUVA_WPTR,
		.read_pointer_address    = GPUVA_RPTR,
		.ring_size               = RING_BYTES,
		.gpu_id                  = GPU_ID,
		.queue_type              = KFD_IOC_QUEUE_TYPE_COMPUTE,
		.queue_percentage        = 100,
		.queue_priority          = 1,
	};

	printf("[4] Creating COMPUTE (PM4) queue...\n");
	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q) == 0) {
		printf("    SUCCESS!  id=%u  db=0x%lx\n",
		       q.queue_id, (unsigned long)q.doorbell_offset);
	} else {
		printf("    COMPUTE FAILED (%s)\n", strerror(errno));
	}

	close(kfd_fd);
	close(drm_fd);
	return 0;
}
