#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

typedef uint32_t __u32;
typedef uint64_t __u64;

#define KFD_IOC_QUEUE_TYPE_COMPUTE 0x0

struct kfd_ioctl_create_queue_args {
	__u64 ring_base_address, write_pointer_address, read_pointer_address, doorbell_offset;
	__u32 ring_size, gpu_id, queue_type, queue_percentage, queue_priority, queue_id;
	__u64 eop_buffer_address, eop_buffer_size, ctx_save_restore_address;
	__u32 ctx_save_restore_size, ctl_stack_size, sdma_engine_id, pad;
};
struct kfd_ioctl_destroy_queue_args { __u32 queue_id; __u32 pad; };
struct kfd_ioctl_acquire_vm_args { __u32 drm_fd; __u32 gpu_id; };

#define AMDKFD_IOCTL_BASE 'K'
#define AMDKFD_IOWR(nr, type)   _IOWR(AMDKFD_IOCTL_BASE, nr, type)
#define AMDKFD_IOW(nr, type)    _IOW(AMDKFD_IOCTL_BASE, nr, type)
#define AMDKFD_IOC_CREATE_QUEUE   AMDKFD_IOWR(0x02, struct kfd_ioctl_create_queue_args)
#define AMDKFD_IOC_DESTROY_QUEUE  AMDKFD_IOWR(0x03, struct kfd_ioctl_destroy_queue_args)
#define AMDKFD_IOC_ACQUIRE_VM     AMDKFD_IOW(0x15, struct kfd_ioctl_acquire_vm_args)

#define KFD_MMAP_TYPE_DOORBELL 0x1
#define KFD_MMAP_GPU_ID_SHIFT  24

#define PAGE_SZ 4096
#define RING_SZ (64 * 1024)
#define CTX_SZ (3 * 1024 * 1024)

static uint32_t read_sysfs_gpu_id(void)
{
	FILE *f = fopen("/sys/class/kfd/kfd/topology/nodes/1/gpu_id", "r");
	if (!f) return 0;
	uint32_t id = 0;
	fscanf(f, "%u", &id);
	fclose(f);
	return id;
}

static int find_render_fd(void)
{
	DIR *d = opendir("/dev/dri");
	if (!d) return -1;
	struct dirent *ent;
	char path[256];
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "renderD", 7) == 0) {
			snprintf(path, sizeof(path), "/dev/dri/%.100s", ent->d_name);
			int fd = open(path, O_RDWR);
			if (fd >= 0) { closedir(d); return fd; }
		}
	}
	closedir(d);
	return -1;
}

int main(void)
{
	uint32_t gpu_id = read_sysfs_gpu_id();
	if (!gpu_id) { fprintf(stderr, "No GPU\n"); return 1; }
	printf("gpu_id = 0x%x\n", gpu_id);

	int kfd_fd = open("/dev/kfd", O_RDWR);
	if (kfd_fd < 0) { perror("open /dev/kfd"); return 1; }

	int drm_fd = find_render_fd();
	if (drm_fd < 0) { fprintf(stderr, "No renderD\n"); return 1; }

	struct kfd_ioctl_acquire_vm_args av = { .drm_fd = drm_fd, .gpu_id = gpu_id };
	if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &av) < 0) {
		fprintf(stderr, "ACQUIRE_VM: %s\n", strerror(errno));
		return 1;
	}
	close(drm_fd);
	printf("ACQUIRE_VM ok\n");

	void *ring, *wptr_buf, *rptr_buf, *ctx;
	posix_memalign(&ring, PAGE_SZ, RING_SZ);
	posix_memalign(&wptr_buf, PAGE_SZ, PAGE_SZ);
	posix_memalign(&rptr_buf, PAGE_SZ, PAGE_SZ);
	posix_memalign(&ctx, PAGE_SZ, CTX_SZ);
	memset(ring, 0, RING_SZ);
	memset(wptr_buf, 0, PAGE_SZ);
	memset(rptr_buf, 0, PAGE_SZ);
	memset(ctx, 0, CTX_SZ);
	printf("ring=%p wptr=%p rptr=%p ctx=%p\n", ring, wptr_buf, rptr_buf, ctx);

	struct kfd_ioctl_create_queue_args cq = {};
	cq.ring_base_address = (__u64)(uintptr_t)ring;
	cq.write_pointer_address = (__u64)(uintptr_t)wptr_buf;
	cq.read_pointer_address = (__u64)(uintptr_t)rptr_buf;
	cq.ring_size = RING_SZ;
	cq.gpu_id = gpu_id;
	cq.queue_type = KFD_IOC_QUEUE_TYPE_COMPUTE;
	cq.queue_percentage = 100;
	cq.queue_priority = 1;
	cq.ctx_save_restore_address = (__u64)(uintptr_t)ctx;
	cq.ctx_save_restore_size = CTX_SZ;
	cq.ctl_stack_size = 0;

	if (ioctl(kfd_fd, AMDKFD_IOC_CREATE_QUEUE, &cq) < 0) {
		fprintf(stderr, "CREATE_QUEUE: %s\n", strerror(errno));
		return 1;
	}
	printf("CREATE_QUEUE: qid=%u doorbell_offset=0x%lx\n",
		cq.queue_id, (unsigned long)cq.doorbell_offset);

	void *doorbell = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE,
			      MAP_SHARED, kfd_fd, cq.doorbell_offset);
	if (doorbell == MAP_FAILED) { perror("mmap doorbell"); return 1; }
	printf("doorbell=%p\n", doorbell);

	volatile uint32_t *rp = (volatile uint32_t *)rptr_buf;
	volatile uint32_t *wp = (volatile uint32_t *)wptr_buf;
	uint32_t rptr_before = *rp;

	volatile uint32_t *ring32 = (volatile uint32_t *)ring;
	ring32[0] = 0xC0001000U;
	__sync_synchronize();
	*wp = 4;
	__sync_synchronize();
	*(volatile uint32_t *)doorbell = 4;
	printf("PM4 NOP submitted: rptr_before=%u\n", rptr_before);

	for (int i = 0; i < 30; i++) {
		usleep(100000);
		uint32_t r = *rp;
		printf("  poll %d: rptr=%u wptr=%u\n", i, r, *wp);
		if (r != rptr_before) {
			printf("CP ALIVE! rptr %u -> %u\n", rptr_before, r);
			break;
		}
	}

	printf("Final: rptr=%u wptr=%u\n", *rp, *wp);

	struct kfd_ioctl_destroy_queue_args dq = { .queue_id = cq.queue_id };
	ioctl(kfd_fd, AMDKFD_IOC_DESTROY_QUEUE, &dq);
	close(kfd_fd);
	return 0;
}
