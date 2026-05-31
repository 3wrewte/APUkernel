/*
 * kfd_probe.c — Stage 1: KFD ioctl stack verification
 *
 * Steps:
 *   1. Open /dev/kfd, get version
 *   2. Open DRM render node, acquire VM
 *   3. Allocate GPU-visible buffer (GTT, coherent)
 *   4. CPU-map the buffer, write a test pattern
 *   5. Create a COMPUTE_AQL queue on GPU node 0
 *   6. Map doorbell + ring buffer
 *   7. Write a NOP packet to the ring, ring doorbell
 *   8. Report all addresses/offsets for debug
 *
 * Compile on A8-7500:
 *   gcc -O2 -Wall -I/lib/modules/$(uname -r)/build/include/uapi \
 *       -I/lib/modules/$(uname -r)/build/include \
 *       -o kfd_probe kfd_probe.c
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

#define GPU_NODE_ID 0
#define PAGE_SIZE   4096
#define RING_SIZE   (64 * 1024)

/* Type-3 PM4 NOP packet for testing ring submission without GPU work */
#define PM4_TYPE3_NOP  0x10
#define PM4_TYPE3(cnt, op)  (0xc0000000U | ((cnt) << 16) | ((op) << 8))

static int kfd_fd    = -1;
static int drm_fd    = -1;
static int mapped_drm_fd = -1; /* kept open for mmap lifetime */
static uint32_t gpu_id = 0;

static void die(const char *msg) {
	fprintf(stderr, "FATAL: %s: %s\n", msg, strerror(errno));
	exit(1);
}

/* Step 1: Open /dev/kfd */
static void open_kfd(void) {
	kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (kfd_fd < 0) die("open /dev/kfd");
	printf("[1] /dev/kfd opened (fd=%d)\n", kfd_fd);
}

/* Step 2: Get KFD version */
static void get_version(void) {
	struct kfd_ioctl_get_version_args args = {0};
	if (ioctl(kfd_fd, AMDKFD_IOC_GET_VERSION, &args) < 0)
		die("AMDKFD_IOC_GET_VERSION");
	printf("[2] KFD version: %u.%u\n", args.major_version, args.minor_version);
}

/* Step 3: Open DRM render node, acquire VM */
static void acquire_vm(void) {
	drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) die("open /dev/dri/renderD128");
	mapped_drm_fd = drm_fd;

	struct kfd_ioctl_acquire_vm_args args = {
		.drm_fd = (__u32)drm_fd,
		.gpu_id = gpu_id,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &args) < 0)
		die("AMDKFD_IOC_ACQUIRE_VM");
	printf("[3] VM acquired (drm_fd=%d, gpu_id=%u)\n", drm_fd, gpu_id);
}

/* Step 4: Allocate GPU-visible buffer */
static uint64_t alloc_buf(size_t size, void **cpu_ptr, uint32_t flags) {
	struct kfd_ioctl_alloc_memory_of_gpu_args args = {
		.va_addr = 0,
		.size    = size,
		.gpu_id  = gpu_id,
		.flags   = flags,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &args) < 0)
		die("AMDKFD_IOC_ALLOC_MEMORY_OF_GPU");

	/* Map for CPU access.  mmap_offset is the GEM offset within the DRM device. */
	void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, drm_fd, args.mmap_offset);
	if (ptr == MAP_FAILED) die("mmap GPU buffer");

	printf("[4] Allocated %zu bytes: handle=0x%lx, mmap_offset=0x%lx, "
	       "va=0x%lx\n",
	       (unsigned long)size,
	       (unsigned long)args.handle,
	       (unsigned long)args.mmap_offset,
	       (unsigned long)args.va_addr);

	*cpu_ptr = ptr;
	return args.handle;
}

/* Step 5: Map memory to GPU */
static void map_to_gpu(uint64_t handle) {
	uint32_t gpus[] = { gpu_id };
	struct kfd_ioctl_map_memory_to_gpu_args args = {
		.handle               = handle,
		.device_ids_array_ptr = (uint64_t)(uintptr_t)gpus,
		.n_devices            = 1,
		.n_success            = 0,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &args) < 0)
		die("AMDKFD_IOC_MAP_MEMORY_TO_GPU");
	printf("[5] Memory mapped to GPU node %u\n", gpu_id);
}

/* Step 6: Create AQL compute queue */
static uint32_t create_queue(uint64_t *ring_addr, uint64_t *wp_addr,
			     uint64_t *rp_addr, uint64_t *db_offset) {
	struct kfd_ioctl_create_queue_args args = {
		.ring_base_address      = 0,    /* KFD allocates */
		.ring_size              = RING_SIZE,
		.gpu_id                 = gpu_id,
		.queue_type             = KFD_IOC_QUEUE_TYPE_COMPUTE_AQL,
		.queue_percentage       = KFD_MAX_QUEUE_PERCENTAGE,
		.queue_priority         = KFD_MAX_QUEUE_PRIORITY,
		.eop_buffer_address     = 0,
		.eop_buffer_size        = 0,
		.ctx_save_restore_address = 0,
		.ctx_save_restore_size  = 0,
		.ctl_stack_size         = 0,
	};
	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &args) < 0)
		die("AMDKFD_IOC_CREATE_QUEUE");

	printf("[6] Queue created: id=%u, ring=%#lx, wp=%#lx, rp=%#lx, db=%#lx\n",
	       args.queue_id,
	       (unsigned long)args.ring_base_address,
	       (unsigned long)args.write_pointer_address,
	       (unsigned long)args.read_pointer_address,
	       (unsigned long)args.doorbell_offset);

	*ring_addr = args.ring_base_address;
	*wp_addr   = args.write_pointer_address;
	*rp_addr   = args.read_pointer_address;
	*db_offset = args.doorbell_offset;
	return args.queue_id;
}

/* Step 7: Map doorbell and ring buffer */
static void map_queue_mem(uint64_t ring_addr, uint32_t **ring,
			  uint64_t *wp_p, uint64_t *rp_p, uint32_t **wp, uint32_t **rp,
			  uint64_t db_off, uint32_t **db) {
	/* Ring buffer: KFD returns a GEM handle-based address; we mmap through KFD directly.
	 * For KFD, the ring base address is a GPU virtual address.  We use
	 * AMDKFD_IOC_GET_PROCESS_APERTURES_NEW to find the CPU mapping.
	 *
	 * Actually, for simplicity, KFD allows mmap on the kfd fd with offset
	 * derived from ring_base_address.  But the exact mechanism depends on
	 * the kernel version.  For 6.12 with SVM support, we use the kfd fd.
	 *
	 * Doorbell: mmap kfd_fd at doorbell_offset * 4096 with size 4096.
	 */

	/* Doorbell page: the doorbell_offset is in units of 64-bit words.
	 * The KFD returns a doorbell offset that we use as an mmap offset. */
	uint64_t db_mmap_off = (db_off / sizeof(uint32_t)) * 4096ULL; /* rough */
	/* Actually, KFD doorbell offset is the byte offset for mmap on kfd_fd */
	uint64_t db_page = db_off & ~(PAGE_SIZE - 1);
	*db = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED, kfd_fd, db_page);
	if (*db == MAP_FAILED) die("mmap doorbell");

	/* Ring buffer: mmap through kfd_fd using ring_base_address as offset.
	 * For SVM-based KFD (6.12), we need to mmap through the kfd process
	 * device.  But the IOCTL returns values that are GPU addresses.
	 *
	 * For GTT memory, the ring base address IS the mmap offset on kfd_fd. */
	*ring = mmap(NULL, RING_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, kfd_fd, ring_addr);
	if (*ring == MAP_FAILED) {
		perror("mmap ring");
		fprintf(stderr, "  ring_addr=0x%lx — trying DRM fd instead\n",
			(unsigned long)ring_addr);
		*ring = mmap(NULL, RING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, drm_fd, ring_addr);
		if (*ring == MAP_FAILED) die("mmap ring (drm)");
	}

	/* Write pointer and read pointer: these are GPU-mapped addresses.
	 * For user-mode queue submission, the KFD maps them CPU-accessible.
	 * The pointers are in the same mmap region. */
	uint64_t wp_page = *wp_p & ~(PAGE_SIZE - 1);
	*wp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED, kfd_fd, wp_page);
	if (*wp == MAP_FAILED) {
		*wp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, drm_fd, wp_page);
		if (*wp == MAP_FAILED) die("mmap write_ptr");
	}
	*wp = (uint32_t *)((uint8_t *)*wp + (*wp_p - wp_page));

	uint64_t rp_page = *rp_p & ~(PAGE_SIZE - 1);
	*rp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED, kfd_fd, rp_page);
	if (*rp == MAP_FAILED) {
		*rp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, drm_fd, rp_page);
		if (*rp == MAP_FAILED) die("mmap read_ptr");
	}
	*rp = (uint32_t *)((uint8_t *)*rp + (*rp_p - rp_page));

	printf("[7] Mapped ring=%p, wp=%p (value %p), rp=%p, db=%p\n",
	       (void *)*ring, (void *)*wp, (void *)(uintptr_t)*(*wp),
	       (void *)*rp, (void *)*db);
}

/* Step 8: Write NOP packet and ring doorbell */
static void submit_nop(uint32_t *ring, uint32_t *wp_ptr, uint32_t *db) {
	uint32_t wptr = *wp_ptr;

	/* Write two NOP packets */
	ring[(wptr / 4) % (RING_SIZE / 4)] = PM4_TYPE3(0, PM4_TYPE3_NOP);
	ring[((wptr / 4) + 1) % (RING_SIZE / 4)] = 0;

	wptr += 8; /* 2 dwords */
	*wp_ptr = wptr;

	/* Ring the doorbell: write the new wptr */
	*db = wptr;

	printf("[8] Submitted NOP packet, doorbell rung\n");
}

int main(void) {
	uint64_t buf_handle, ring_addr, wp_addr, rp_addr, db_off;
	uint32_t queue_id;
	void *test_buf;
	uint32_t *ring, *wp_ptr, *rp_ptr, *db;

	printf("=== HeteroKern KFD Probe ===\n\n");

	open_kfd();
	get_version();
	acquire_vm();

	buf_handle = alloc_buf(PAGE_SIZE, &test_buf,
		KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
		KFD_IOC_ALLOC_MEM_FLAGS_COHERENT);

	map_to_gpu(buf_handle);

	queue_id = create_queue(&ring_addr, &wp_addr, &rp_addr, &db_off);
	map_queue_mem(ring_addr, &ring, &wp_addr, &rp_addr,
		      &wp_ptr, &rp_ptr, db_off, &db);

	submit_nop(ring, wp_ptr, db);

	usleep(100000); /* let GPU process the NOP */

	printf("\n=== All KFD ioctls passed ===\n");
	printf("Test buffer at %p: ", test_buf);
	/* Write a pattern to prove CPU access */
	((volatile uint32_t *)test_buf)[0] = 0x21544148;
	printf("wrote 0x%08x\n", ((volatile uint32_t *)test_buf)[0]);

	close(kfd_fd);
	close(drm_fd);
	return 0;
}
