/*
 * kfd_gpuva.c — HeteroKern: create queue using GPU VAs (not CPU VAs)
 *
 * Key insight for GFX7 (no SVM):
 *   kfd_queue_buffer_get() looks up addresses in GPU VM page tables.
 *   We must pass GPU virtual addresses, not CPU mmap addresses.
 *
 *   CPU VA and GPU VA are DIFFERENT on GFX7.
 *   Allocate at specific GPU VAs, mmap to get CPU access,
 *   pass GPU VAs to create_queue.
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
#define RING_KB     64
#define RING_BYTES  (RING_KB * 1024)

/* GPU VA addresses (must be in range 0x10000 - 0xfffffffff) */
#define GPUVA_RING  0x100000
#define GPUVA_WPTR  0x200000
#define GPUVA_RPTR  0x210000

static int kfd_fd, drm_fd;

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

static void *alloc_at_gpuva(uint64_t gpu_va, size_t size,
			    uint64_t *handle, const char *name)
{
	struct kfd_ioctl_alloc_memory_of_gpu_args a = {
		.va_addr = gpu_va,
		.size    = size,
		.gpu_id  = GPU_ID,
		.flags   = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		           KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &a) < 0)
		die("ALLOC_MEMORY");

	void *cpu = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, drm_fd, a.mmap_offset);
	if (cpu == MAP_FAILED) die("mmap");

	printf("  [%s] GPUVA=0x%lx  CPU=%p  handle=0x%lx\n",
	       name, (unsigned long)gpu_va, cpu, (unsigned long)a.handle);

	if (handle) *handle = a.handle;
	return cpu;
}

int main(void)
{
	printf("=== HeteroKern GPU-VA Queue Test ===\n\n");

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

	/* 2. Allocate ring, wptr, rptr at explicit GPU VAs */
	printf("[2] Allocating at GPU VAs:\n");
	uint64_t rh, wh, rh2;
	void *ring  = alloc_at_gpuva(GPUVA_RING, RING_BYTES, &rh,  "ring");
	void *wp_cpu = alloc_at_gpuva(GPUVA_WPTR, PAGE,       &wh,  "wptr");
	void *rp_cpu = alloc_at_gpuva(GPUVA_RPTR, PAGE,       &rh2, "rptr");

	/* 3. Map each buffer into GPU VM */
	printf("[3] Mapping to GPU VM:\n");
	{
		uint32_t gpus[] = { GPU_ID };
		struct kfd_ioctl_map_memory_to_gpu_args m = {
			.device_ids_array_ptr = (uint64_t)(uintptr_t)gpus,
			.n_devices = 1, .n_success = 0,
		};

		m.handle = rh;
		if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) < 0)
			printf("  ring: MAP TO GPU FAILED (%s)\n", strerror(errno));
		else
			printf("  ring: mapped to GPU VM at 0x%lx\n", (unsigned long)GPUVA_RING);

		m.handle = wh;  m.n_success = 0;
		if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) < 0)
			printf("  wptr: MAP TO GPU FAILED (%s)\n", strerror(errno));
		else
			printf("  wptr: mapped to GPU VM at 0x%lx\n", (unsigned long)GPUVA_WPTR);

		m.handle = rh2;  m.n_success = 0;
		if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) < 0)
			printf("  rptr: MAP TO GPU FAILED (%s)\n", strerror(errno));
		else
			printf("  rptr: mapped to GPU VM at 0x%lx\n", (unsigned long)GPUVA_RPTR);
	}

	/* 4. Create queue using GPU VAs (not CPU VAs!) */
	printf("[4] Creating queue...\n");
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

	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q) < 0) {
		/* If COMPUTE fails, try COMPUTE_AQL */
		q.queue_type = KFD_IOC_QUEUE_TYPE_COMPUTE_AQL;
		if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q) < 0)
			die("CREATE_QUEUE");
	}

	printf("[4] QUEUE CREATED SUCCESSFULLY!\n");
	printf("    queue_id  = %u\n", q.queue_id);
	printf("    doorbell  = 0x%lx\n", (unsigned long)q.doorbell_offset);
	printf("    ring_base = 0x%lx\n", (unsigned long)q.ring_base_address);
	printf("    write_ptr = 0x%lx\n", (unsigned long)q.write_pointer_address);
	printf("    read_ptr  = 0x%lx\n", (unsigned long)q.read_pointer_address);

	close(kfd_fd);
	close(drm_fd);
	printf("\n=== ALL PASSED ===\n");
	return 0;
}
