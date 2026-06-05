/*
 * kfd_queue_test.c — HeteroKern Phase H, Step 1: Queue creation test
 *
 * Demonstrates correct KFD queue creation on GFX7 (Kaveri):
 *   1. Open /dev/kfd + DRM render node
 *   2. Acquire VM
 *   3. Allocate ring buffer, write pointer, read pointer via KFD
 *   4. mmap each to get CPU virtual addresses
 *   5. Create COMPUTE_AQL queue, passing the CPU addrs
 *   6. Report all offsets for debug
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

/* Allocate GPU-visible GTT buffer and return its CPU mapping */
static void *alloc_and_map(size_t size, uint64_t *handle_out,
			   uint64_t *mmap_off_out, const char *name) {
	struct kfd_ioctl_alloc_memory_of_gpu_args args = {
		.size   = size,
		.gpu_id = GPU_ID,
		.flags  = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		          KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &args) < 0)
		die("ALLOC_MEMORY_OF_GPU");

	void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, drm_fd, args.mmap_offset);
	if (ptr == MAP_FAILED)
		die("mmap");

	printf("  [%s] size=%zu  handle=0x%lx  mmap_off=0x%lx  ptr=%p\n",
	       name, (unsigned long)size,
	       (unsigned long)args.handle,
	       (unsigned long)args.mmap_offset, ptr);

	if (handle_out)    *handle_out    = args.handle;
	if (mmap_off_out)  *mmap_off_out  = args.mmap_offset;
	return ptr;
}

int main(void) {
	printf("=== HeteroKern KFD Queue Test ===\n\n");

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

	/* 2. Allocate ring buffer, write ptr page, read ptr page */
	printf("[2] Allocating queue buffers:\n");
	uint64_t rh, wh, wph_moff, rph, rph_moff;
	void *ring  = alloc_and_map(RING_SIZE, &rh, NULL, "ring");
	void *wp_buf = alloc_and_map(PAGE_SIZE, &wh, &wph_moff, "write_ptr");
	void *rp_buf = alloc_and_map(PAGE_SIZE, &rh, &rph_moff, "read_ptr");

	/* 3. Create queue */
	struct kfd_ioctl_create_queue_args q = {
		.ring_base_address       = (uint64_t)(uintptr_t)ring,
		.write_pointer_address   = (uint64_t)(uintptr_t)wp_buf,
		.read_pointer_address    = (uint64_t)(uintptr_t)rp_buf,
		.ring_size               = RING_SIZE,
		.gpu_id                  = GPU_ID,
		.queue_type              = KFD_IOC_QUEUE_TYPE_COMPUTE_AQL,
		.queue_percentage        = 100,
		.queue_priority          = 1,
	};

	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &q) < 0)
		die("CREATE_QUEUE");

	printf("\n[3] Queue created successfully!\n");
	printf("    queue_id    = %u\n", q.queue_id);
	printf("    ring_base   = 0x%lx\n", (unsigned long)q.ring_base_address);
	printf("    write_ptr   = 0x%lx\n", (unsigned long)q.write_pointer_address);
	printf("    read_ptr    = 0x%lx\n", (unsigned long)q.read_pointer_address);
	printf("    doorbell    = 0x%lx\n", (unsigned long)q.doorbell_offset);

	/* 4. Map doorbell page for submission */
	uint64_t db_page = q.doorbell_offset & ~((uint64_t)PAGE_SIZE - 1);
	uint32_t *db = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED,
			    kfd_fd, db_page);
	if (db == MAP_FAILED) die("mmap doorbell");

	uint64_t db_word_off = (q.doorbell_offset - db_page) / sizeof(uint32_t);
	printf("[4] Doorbell mapped: db_page=0x%lx  word_off=%lu  db_ptr=%p\n",
	       (unsigned long)db_page, (unsigned long)db_word_off, (void *)db);

	printf("\n=== All steps passed! Queue ready for dispatch ===\n");

	close(kfd_fd);
	close(drm_fd);
	return 0;
}
