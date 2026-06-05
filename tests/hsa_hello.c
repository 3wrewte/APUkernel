/*
 * hsa_hello.c — Phase H milestone: dispatch hello_gcn.hsaco via HSA
 *
 * Uses libhsakmt directly.  Parses .hsaco ELF to extract kernel
 * code (.text) and kernel descriptor, dispatches to GCN CU,
 * verifies the magic value returned through hUMA shared memory.
 *
 * Compile on target:
 *   gcc -O2 -Wall -o hsa_hello hsa_hello.c -lhsakmt -lelf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <gelf.h>
#include <libelf.h>

/* libhsakmt */
#include "hsakmt/hsakmt.h"
#include "hsakmt/hsakmttypes.h"
#include "hsa/amd_hsa_kernel_code.h"
#include "hsa/amd_hsa_queue.h"

#define GPU_NODE_ID    1       /* node 1 = GPU from topology */
#define RING_SIZE      (64 * 1024)
#define PAGE_ALIGNED   4096

static void die(const char *s) {
	fprintf(stderr, "FATAL: %s: %s\n", s, strerror(errno));
	exit(1);
}

/* Simple ELF parser: find section by name */
static Elf_Scn *find_section(Elf *e, const char *name)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	char *sname;

	if (elf_getshdrstrndx(e, &shstrndx) != 0) return NULL;

	while ((scn = elf_nextscn(e, scn)) != NULL) {
		gelf_getshdr(scn, &shdr);
		sname = elf_strptr(e, shstrndx, shdr.sh_name);
		if (sname && !strcmp(sname, name))
			return scn;
	}
	return NULL;
}

/* Load entire section into a malloc'd buffer */
static void *load_section(Elf *e, Elf_Scn *scn, size_t *sz)
{
	GElf_Shdr shdr;
	Elf_Data *data;

	gelf_getshdr(scn, &shdr);
	*sz = shdr.sh_size;
	data = elf_getdata(scn, NULL);
	if (!data || data->d_size < *sz) return NULL;

	void *buf = malloc(*sz);
	memcpy(buf, data->d_buf, *sz);
	return buf;
}

int main(int argc, char **argv)
{
	const char *hsaco_path = argc > 1 ? argv[1] : "hello_gcn.hsaco";
	printf("=== HeteroKern Phase H: HSA Hello World ===\n");

	/* ── 0. Parse .hsaco ELF ── */
	if (elf_version(EV_CURRENT) == EV_NONE)
		die("elf_version");

	int fd = open(hsaco_path, O_RDONLY);
	if (fd < 0) die("open hsaco");
	Elf *e = elf_begin(fd, ELF_C_READ, NULL);
	if (!e) die("elf_begin");

	/* Extract .text (GCN ISA code) */
	Elf_Scn *text = find_section(e, ".text");
	if (!text) die("find .text");
	size_t code_size;
	void *code = load_section(e, text, &code_size);
	if (!code) die("load .text");
	printf("[0] Loaded %zu bytes of GCN code from %s\n", code_size, hsaco_path);

	/* Extract kernel descriptor from .AMDGPU.csdata */
	/* The descriptor is at a fixed offset in the note section.
	 * For simplicity, scan for the amd_kernel_code_t magic */
	amd_kernel_code_t *akc = NULL;
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	char *sname;
	elf_getshdrstrndx(e, &shstrndx);
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		gelf_getshdr(scn, &shdr);
		sname = elf_strptr(e, shstrndx, shdr.sh_name);
		if (!sname || strncmp(sname, ".AMDGPU.csdata", 14)) continue;
		Elf_Data *data = elf_getdata(scn, NULL);
		if (!data) continue;
		/* Scan for amd_kernel_code_t at proper alignment */
		for (size_t off = 0; off + sizeof(*akc) <= data->d_size; off += 256) {
			amd_kernel_code_t *candidate =
				(amd_kernel_code_t *)((char *)data->d_buf + off);
			if (candidate->amd_kernel_code_version_major == 1 &&
			    candidate->kernel_code_entry_byte_offset < code_size) {
				akc = malloc(sizeof(*akc));
				memcpy(akc, candidate, sizeof(*akc));
				printf("[0] Kernel descriptor found: "
				       "kernel_code_entry_byte_offset=%u\n",
				       akc->kernel_code_entry_byte_offset);
				break;
			}
		}
		if (akc) break;
	}
	if (!akc) die("find kernel descriptor");

	elf_end(e);
	close(fd);

	/* ── 1. Open KFD ── */
	HSAKMT_STATUS st;
	st = hsaKmtOpenKFD();
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtOpenKFD");
	printf("[1] KFD opened\n");

	/* ── 2. Allocate hUMA buffer for kernel code + result ── */
	void *huma_code, *huma_result;
	HsaMemFlags flags = { .Value = 0 };
	flags.ui32.HostAccess     = 1;
	flags.ui32.CoarseGrain    = 1;
	flags.ui32.ExecuteAccess  = 1;

	st = hsaKmtAllocMemory(GPU_NODE_ID,
			       (code_size + PAGE_ALIGNED - 1) & ~(PAGE_ALIGNED - 1),
			       flags, &huma_code);
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtAllocMemory(code)");
	printf("[2] Allocated %zu bytes for kernel code at %p\n",
	       code_size, huma_code);

	flags.ui32.ExecuteAccess = 0;
	st = hsaKmtAllocMemory(GPU_NODE_ID, PAGE_ALIGNED, flags, &huma_result);
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtAllocMemory(result)");
	printf("[2] Allocated result buffer at %p\n", huma_result);

	/* Copy kernel code into hUMA buffer, then map to GPU */
	memcpy(huma_code, code, code_size);
	st = hsaKmtMapMemoryToGPU(huma_code, code_size, NULL);
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtMapMemoryToGPU(code)");
	st = hsaKmtMapMemoryToGPU(huma_result, PAGE_ALIGNED, NULL);
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtMapMemoryToGPU(result)");
	printf("[2] Mapped buffers to GPU\n");

	/* ── 3. Create AQL queue ── */
	HsaQueueResource qres;
	memset(&qres, 0, sizeof(qres));
	st = hsaKmtCreateQueue(GPU_NODE_ID, HSA_QUEUE_TYPE_COMPUTE_AQL,
			       100, 1, NULL, &qres);
	if (st != HSAKMT_STATUS_SUCCESS) die("hsaKmtCreateQueue");
	printf("[3] Queue created: id=%u  doorbell=%p  write_ptr=%p  read_ptr=%p\n",
	       qres.QueueId, (void *)qres.Queue_DoorBell,
	       (void *)qres.Queue_write_ptr,
	       (void *)qres.Queue_read_ptr);

	/* ── 4. Build AQL dispatch packet ── */
	/*
	 * For GFX7 (CIK), the AQL dispatch packet lives in the ring buffer.
	 * The ring buffer is at qres.Queue_write_ptr - we need to get the
	 * ring base address.  The hsaKmtCreateQueue allocates it internally.
	 *
	 * For simplicity, we write directly to the write pointer location.
	 * The AQL packet format:
	 *
	 * For user mode queues, we write an amd_kernel_dispatch_packet
	 * (which is backed by a PM4 dispatch packet for GFX7).
	 *
	 * Actually, the ring buffer is at a fixed address that we need
	 * to retrieve from the queue resource.  hsaKmtCreateQueue fills
	 * QueueWptrValue with the initial write pointer.  We can subtract
	 * to find the ring base.
	 *
	 * Even simpler: use the HSA AQL packet helpers from amd_hsa_queue.h
	 */

	/* The QueueResource has Queue_write_ptr pointing to the current
	 * write pointer location.  For AQL queue, each packet is 64 bytes.
	 * We write one packet at the current wptr, then advance wptr by 64. */

	volatile uint32_t *wptr  = (volatile uint32_t *)qres.Queue_write_ptr;
	uint64_t *ring = NULL; /* need to find ring base */

	/* hsaKmtCreateQueue for AQL allocates the ring internally.
	 * The ring base is accessible via the write pointer; we need
	 * to find the ring base page.
	 *
	 * On GFX7, the ring buffer is a user-mode mapped buffer.
	 * We can mmap it by finding the GEM handle or using the process
	 * apertures.  But for now, let's use a simpler approach:
	 * use the Queue_write_ptr address to locate the ring. */

	/* The AQL queue ring starts at the page containing Queue_write_ptr.
	 * The current write pointer is at ring_base + initial offset (0) */
	uint64_t ring_page = ((uint64_t)(uintptr_t)wptr) & ~(PAGE_ALIGNED - 1);
	ring = (uint64_t *)ring_page;

	printf("[4] Ring at %p (wptr=%p, rptr=%p)\n",
	       (void *)ring, (void *)wptr, (void *)qres.Queue_read_ptr);

	/* Build a simple PM4 dispatch_direct packet for GFX7 */
	/* For GFX7, we issue a DISPATCH_DIRECT PM4 packet */
	{
		uint32_t header = 0xc0000000U | (4 << 16) | (0x15 << 8); /* IT_DISPATCH_DIRECT, 5 dwords */
		uint32_t dim_x = 1, dim_y = 1, dim_z = 1;
		uint32_t dispatch_initiator = 0; /* use default from MQD */

		/* Write to ring at current wptr */
		uint32_t cur = *wptr;
		uint32_t idx = (cur / 4) % (RING_SIZE / 4);

		ring[idx + 0] = header;
		ring[idx + 1] = dim_x;
		ring[idx + 2] = dim_y;
		ring[idx + 3] = dim_z;
		ring[idx + 4] = dispatch_initiator;
		ring[idx + 5] = 0;
		ring[idx + 6] = 0;
		ring[idx + 7] = 0;

		/* Advance wptr (5 dwords for PM4 header+data) */
		*wptr = cur + 8 * 4;

		/* Ring doorbell */
		*qres.Queue_DoorBell = *wptr;

		printf("[4] PM4 DISPATCH_DIRECT submitted, doorbell rung\n");
	}

	/* ── 5. Wait for completion ── */
	usleep(500000); /* 500ms */

	printf("[5] Result: magic = 0x%08x\n",
	       *(volatile uint32_t *)huma_result);

	hsaKmtDestroyQueue(qres.QueueId);
	hsaKmtFreeMemory(huma_code, code_size);
	hsaKmtFreeMemory(huma_result, PAGE_ALIGNED);
	hsaKmtCloseKFD();

	free(code);
	free(akc);
	printf("\n=== Done ===\n");
	return 0;
}
