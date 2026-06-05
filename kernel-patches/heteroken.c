/*
 * HeteroKern — in-tree GCN dispatch test (Phase H)
 *
 * Built as part of drivers/gpu/drm/amd/amdkfd/.
 * Creates a sysfs node /sys/kernel/heteroken/run — write "1" to trigger
 * the full queue creation + GCN dispatch test.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include "kfd_priv.h"
#include "../amdgpu/amdgpu_amdkfd.h"

/*
 * Minimal amd_kernel_code_t layout — only fields the CP reads for AQL dispatch.
 * Full struct is ~256 bytes; we zero the rest via memset.
 */
struct hk_kernel_descriptor {
	uint32_t amd_kernel_code_version_major;  /* 0 */
	uint32_t amd_kernel_code_version_minor;  /* 4 */
	uint16_t amd_machine_kind;               /* 8 */
	uint16_t amd_machine_version_major;      /* 10 */
	uint16_t amd_machine_version_minor;      /* 12 */
	uint16_t amd_machine_version_stepping;   /* 14 */
	int64_t  kernel_code_entry_byte_offset;  /* 16 */
	int64_t  kernel_code_prefetch_byte_size; /* 24 */
	uint32_t max_scratch_backing_memory;     /* 32 */
	uint32_t compute_pgm_rsrc1;              /* 36 */
	uint32_t compute_pgm_rsrc2;              /* 40 */
	uint16_t enable_sgpr_private_segment;    /* 44 */
	uint16_t enable_sgpr_dispatch_ptr;       /* 46 */
	uint16_t enable_sgpr_queue_ptr;          /* 48 */
	uint16_t enable_sgpr_kernarg_segment_ptr; /* 50 */
	uint16_t enable_sgpr_dispatch_id;        /* 52 */
	uint16_t enable_sgpr_flat_scratch_init;  /* 54 */
	uint16_t enable_sgpr_private_segment_size; /* 56 */
	uint16_t enable_sgpr_grid_workgroup_count; /* 58 */
	uint16_t granulated_workitem_vgpr_count; /* 60 */
	uint16_t granulated_wavefront_sgpr_count; /* 62 */
	uint16_t enable_wg_size;                 /* 64 */
	uint16_t enable_exception_msb;           /* 66 */
	uint16_t granulated_lds_size;            /* 68 */
	uint16_t enable_exception_address_watch;  /* 70 */
	uint32_t enable_exception_handle;        /* 72 */
	uint32_t is_ptr64;                       /* 76 */
	uint32_t workitem_private_segment_size;  /* 80 */
	uint32_t workgroup_group_segment_size;   /* 84 */
	uint32_t gds_segment_size;               /* 88 */
	int64_t  kernarg_segment_byte_size;      /* 96 */
	uint32_t workgroup_fbarrier_count;       /* 104 */
	uint16_t wavefront_sgpr_count;           /* 108 */
	uint16_t workitem_vgpr_count;            /* 110 */
	uint16_t reserved_vgpr_first;            /* 112 */
	uint16_t reserved_vgpr_count;            /* 114 */
	uint16_t reserved_sgpr_first;            /* 116 */
	uint16_t reserved_sgpr_count;            /* 118 */
	uint16_t debug_wavefront_private_segment_offset_sgpr; /* 120 */
	uint16_t debug_private_segment_buffer_sgpr; /* 122 */
	uint8_t  enable_private_segment;         /* 124 */
	uint8_t  enable_dispatch_ptr;            /* 125 */
	uint8_t  enable_queue_ptr;               /* 126 */
	uint8_t  enable_kernarg_segment_ptr;     /* 127 */
	uint8_t  enable_dispatch_id;             /* 128 */
	uint8_t  enable_flat_scratch_init;       /* 129 */
	uint8_t  enable_private_segment_size;    /* 130 */
	uint8_t  enable_grid_workgroup_count;    /* 131 */
	uint16_t enable_ordered_append_gds;      /* 132 */
	uint16_t flat_scratch_init_size;         /* 134 */
	uint32_t compute_pgm_rsrc3;              /* 136 */
	uint32_t wavefront_size;                 /* 140 (6 = 2^6 = 64) */
};
#define HK_KERNEL_DESC_SIZE 256

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern GCN dispatch test");

#define HK_GPUVA_RING    0x1000000ULL
#define HK_GPUVA_WPTR    0x1010000ULL
#define HK_GPUVA_RPTR    0x1020000ULL
#define HK_GPUVA_EOP     0x1030000ULL
#define HK_GPUVA_KERN    0x1040000ULL
#define HK_GPUVA_RESULT  0x1050000ULL
#define HK_GPUVA_ARGS    0x1060000ULL
#define RING_BYTES  (32ULL * 1024)
#define PAGE       4096ULL

static const uint32_t hello_gcn_code[] = {
	0xC0440100, 0xBF8C007F, 0x7E000208, 0x7E020209,
	0x7E0402FF, 0x21544148, 0xDC700000, 0x00000200,
	0xBF810000,
};
static const uint32_t hk_rsrc1 = (4 - 1) | ((16 - 1) << 6) | (3 << 12) | (1 << 20);
static const uint32_t hk_rsrc2 = (0 << 0) | (1 << 6) | (0 << 15);

/* ── State ── */
static struct kfd_process     *hk_proc;
static struct kgd_mem         *ring_mem, *wptr_mem, *rptr_mem, *eop_mem;
static struct kgd_mem         *kern_mem, *result_mem, *args_mem;
static unsigned int            hk_qid;
static bool                    hk_ready;

/* ── Core: create queue + dispatch ── */
static int hk_run(void)
{
	struct kfd_process_device *pdd;
	struct amdgpu_device *adev;
	void *drm_priv;
	struct queue *q;
	struct file *kfd_f, *drm_f;
	uint32_t *mqd, wval, idx, db_id, pgm_lo, pgm_hi;
	void *kern_ptr, *args_ptr, *r_ptr;
	uint64_t mmap_off;
	uint32_t fl = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
	int ret;

	/* 1. Open /dev/kfd */
	kfd_f = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(kfd_f)) { pr_err("hk: /dev/kfd: %ld\n", PTR_ERR(kfd_f)); return PTR_ERR(kfd_f); }
	hk_proc = kfd_f->private_data;

	/* 2. Open DRM, get PDD, init VM */
	drm_f = filp_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(drm_f)) { pr_err("hk: renderD128: %ld\n", PTR_ERR(drm_f)); return PTR_ERR(drm_f); }

	/* Auto-detect GPU: iterate process PDDs (GPU ID varies per kernel config) */
	pdd = NULL;
	for (int i = 0; i < hk_proc->n_pdds; i++) {
		if (hk_proc->pdds[i] && hk_proc->pdds[i]->dev && hk_proc->pdds[i]->dev->adev) {
			pdd = hk_proc->pdds[i];
			break;
		}
	}
	if (!pdd) { pr_err("hk: PDD not found (n_pdds=%d)\n", hk_proc->n_pdds); return -EINVAL; }

	adev = pdd->dev->adev;
	ret = kfd_process_device_init_vm(pdd, drm_f);
	if (ret) { pr_err("hk: init_vm: %d\n", ret); return ret; }
	drm_priv = pdd->drm_priv;

	pdd  = kfd_bind_process_to_device(pdd->dev, hk_proc);
	if (IS_ERR(pdd)) { pr_err("hk: bind: %ld\n", PTR_ERR(pdd)); return PTR_ERR(pdd); }
	pr_info("hk: process ready  pdd=%px  adev=%px\n", pdd, adev);

	/* 3. Alloc + map ring / wptr / rptr / eop */
#define HK_ALLOC_MAP(mem, va, sz, fl) do {				\
	ret = amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu(			\
		adev, va, sz, drm_priv, &mem, &mmap_off, fl, false);	\
	if (ret) goto out;						\
	ret = amdgpu_amdkfd_gpuvm_map_memory_to_gpu(adev, mem, drm_priv); \
	if (ret) goto out;						\
} while (0)

	HK_ALLOC_MAP(ring_mem,   HK_GPUVA_RING,   RING_BYTES, fl);
	HK_ALLOC_MAP(wptr_mem,   HK_GPUVA_WPTR,   PAGE,       fl);
	HK_ALLOC_MAP(rptr_mem,   HK_GPUVA_RPTR,   PAGE,       fl);
	HK_ALLOC_MAP(eop_mem,    HK_GPUVA_EOP,    PAGE,       fl);
	fl |= KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
	HK_ALLOC_MAP(kern_mem,   HK_GPUVA_KERN,   PAGE,       fl);
	fl &= ~KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
	HK_ALLOC_MAP(result_mem, HK_GPUVA_RESULT, PAGE,       fl);
	HK_ALLOC_MAP(args_mem,   HK_GPUVA_ARGS,   PAGE,       fl);
#undef HK_ALLOC_MAP
	pr_info("hk: %d buffers allocated\n", 7);

	/* 4. Build queue_properties + create queue */
	{
		struct queue_properties qp;
		uint32_t db_off;
		memset(&qp, 0, sizeof(qp));
		qp.type = KFD_QUEUE_TYPE_COMPUTE;
		qp.format = KFD_QUEUE_FORMAT_PM4;  /* PM4 — no AQL, raw packets */
		qp.queue_address = HK_GPUVA_RING;
		qp.queue_size    = RING_BYTES;
		qp.priority = 1;
		qp.queue_percent = 100;
		qp.read_ptr  = (void __user *)HK_GPUVA_RPTR;
		qp.write_ptr = (void __user *)HK_GPUVA_WPTR;
		qp.wptr_bo = wptr_mem->bo;
		qp.rptr_bo = rptr_mem->bo;
		qp.ring_bo = ring_mem->bo;
		qp.eop_buf_bo = eop_mem->bo;
		qp.eop_ring_buffer_address = HK_GPUVA_EOP;
		qp.eop_ring_buffer_size    = PAGE;

		ret = kfd_alloc_process_doorbells(adev->kfd.dev, pdd);
		if (ret) { pr_err("hk: doorbell alloc: %d\n", ret); goto out; }

		ret = pqm_create_queue(&hk_proc->pqm, pdd->dev, NULL, &qp,
				       &hk_qid, NULL, NULL, NULL, &db_off);
		if (ret) { pr_err("hk: create_queue: %d\n", ret); goto out; }
		hk_ready = true;
		pr_info("hk: QUEUE CREATED  id=%u  db_off=0x%x\n", hk_qid, db_off);
	}

	/* 5. Load kernel code + descriptor + args */
	ret = amdgpu_bo_kmap(kern_mem->bo, &kern_ptr);
	if (ret) goto out;

	/* Build kernel descriptor at start of buffer */
	{
		struct hk_kernel_descriptor *akc = kern_ptr;
		uint32_t code_offset = ALIGN(HK_KERNEL_DESC_SIZE, 256);
		memset(akc, 0, HK_KERNEL_DESC_SIZE);
		akc->amd_kernel_code_version_major = 1;
		akc->amd_kernel_code_version_minor = 1;
		akc->amd_machine_kind            = 1; /* AMDGPU */
		akc->amd_machine_version_major    = 7;
		akc->kernel_code_entry_byte_offset = code_offset;
		akc->kernel_code_prefetch_byte_size = sizeof(hello_gcn_code);
		akc->compute_pgm_rsrc1 = hk_rsrc1;
		akc->compute_pgm_rsrc2 = hk_rsrc2;
		akc->is_ptr64 = 1;
		akc->enable_sgpr_kernarg_segment_ptr = 1;
		akc->wavefront_size = 6;   /* 2^6 = 64 */
		akc->workitem_vgpr_count = 4;
		akc->wavefront_sgpr_count = 16;
		akc->kernarg_segment_byte_size = 8;
		memcpy((char *)kern_ptr + code_offset, hello_gcn_code,
		       sizeof(hello_gcn_code));
		pr_info("hk: descriptor at kern+0, code at kern+%u\n", code_offset);
	}

	ret = amdgpu_bo_kmap(args_mem->bo, &args_ptr);
	if (ret) goto out;
	*(uint64_t *)args_ptr = HK_GPUVA_RESULT;

	/* 6. Update MQD */
	q = pqm_get_user_queue(&hk_proc->pqm, hk_qid);
	if (!q) { ret = -EIO; goto out; }
	mqd = q->mqd;
	pgm_lo = (HK_GPUVA_KERN >> 8) & 0xFFFFFFFF;
	pgm_hi = (HK_GPUVA_KERN >> 40) & 0xFF;
	mqd[1]  = (1 << 0) | (1 << 2);
	/* Note: compute_pgm_* and rsrc* are read from kernel descriptor by CP,
	 * so MQD settings for these are not strictly needed with AQL dispatch.
	 * We still set them for belt-and-suspenders. */
	mqd[17] = pgm_lo;
	mqd[18] = pgm_hi;
	mqd[23] = hk_rsrc1;
	mqd[24] = hk_rsrc2;
	mqd[33] = HK_GPUVA_ARGS & 0xFFFFFFFF;
	mqd[34] = (HK_GPUVA_ARGS >> 32) & 0xFFFFFFFF;
	pr_info("hk: MQD updated  pgm=0x%08x_%02x\n", pgm_lo, pgm_hi);

	/* 7. Write PM4 WRITE_DATA to ring — validates ring + doorbell (no shader) */
	{
		uint32_t *r, *w;
		ret = amdgpu_bo_kmap(ring_mem->bo, (void **)&r); if (ret) goto out;
		ret = amdgpu_bo_kmap(wptr_mem->bo, (void **)&w); if (ret) goto out;
		wval = *w; idx = (wval / 4) % (RING_BYTES / 4);

		/* PACKET3_RELEASE_MEM (0x3C): event-based write to memory */
		/* Write timestamp/marker to result address */
		r[idx + 0] = 0xc0000000U | (5 << 16) | (0x3C << 8);
		r[idx + 1] = PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS) |
			     PACKET3_RELEASE_MEM_EVENT_INDEX(5) |
			     (1 << 26); /* DATA_SEL: send 64-bit data */
		r[idx + 2] = (1 << 29) | (1 << 30); /* INT_SEL + DST_SEL = memory */
		r[idx + 3] = HK_GPUVA_RESULT & 0xFFFFFFFF;
		r[idx + 4] = (HK_GPUVA_RESULT >> 32) & 0xFFFFFFFF;
		r[idx + 5] = 0x21544148; /* data lo */
		r[idx + 6] = 0;          /* data hi */

		wval += 7 * 4; *w = wval;

		db_id = q->doorbell_id;
		if (adev->doorbell.cpu_addr) {
			writel(wval, adev->doorbell.cpu_addr + db_id);
			pr_info("hk: WRITE_DATA doorbell RUNG  id=%d  wptr=0x%x\n", db_id, wval);
		}
	}

	/* 8. Wait + verify */
	msleep(200); dma_wmb();
	ret = amdgpu_bo_kmap(result_mem->bo, (void **)&r_ptr);
	if (ret == 0) {
		uint32_t magic = *(uint32_t *)r_ptr;
		pr_info("hk: RESULT 0x%08x %s\n", magic,
			magic == 0x21544148 ? "MATCH! ring+doorbell WORK!" : "(no match)");
	}
	return 0;

out:
	pr_err("hk: FAILED at step (ret=%d)\n", ret);
	return ret;
}

/* ── Sysfs: /sys/kernel/heteroken/run ── */
static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	hk_run();
	return count;
}
static struct kobj_attribute run_attr = __ATTR_WO(run);
static struct kobject *hk_kobj;

static int __init hk_init(void)
{
	pr_info("hk: HeteroKern sysfs interface ready\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj) return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))
		pr_warn("hk: sysfs_create_file failed\n");
	return 0;
}

static void __exit hk_exit(void)
{
	if (hk_ready) pqm_destroy_queue(&hk_proc->pqm, hk_qid);
	if (hk_kobj) kobject_put(hk_kobj);
	pr_info("hk: unloaded\n");
}

module_init(hk_init);
module_exit(hk_exit);
