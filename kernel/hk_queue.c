/*
 * hk_queue.c — HeteroKern: create queue + dispatch GCN kernel
 *
 * Phase H: loads hello_gcn.hsaco, updates queue MQD with shader,
 * submits PM4 DISPATCH_DIRECT, verifies magic value in hUMA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kfd_ioctl.h>

MODULE_LICENSE("GPL");

static ulong kln_addr;
module_param(kln_addr, ulong, 0);
static unsigned long (*kln)(const char *);

/* ── Function pointers ── */
#define FPTR(ret, name, ...) static ret (*f_##name)(__VA_ARGS__)
FPTR(void *, cp, struct task_struct *);
FPTR(void,   ur, void *);
FPTR(void *, pdd, void *, uint32_t);
FPTR(int,    ivm, void *, struct file *);
FPTR(void *, bind, void *, void *);
FPTR(int,    pqm, void *, void *, void *, void *, unsigned int *,
	const void *, const void *, const void *, uint32_t *);
FPTR(int,    dqm, void *, unsigned int);
FPTR(int,    alloc, void *, uint64_t, uint64_t, void *, void **,
	uint64_t *, uint32_t, bool);
FPTR(int,    map, void *, void *, void *);
FPTR(int,    free, void *, void *, void *, uint64_t *);
FPTR(int,    db, void *, void *);
FPTR(void *, gpq, void *, unsigned int);
FPTR(void *, bokptr, void *);
FPTR(int,    bokmap, void *, void **);
FPTR(int,    upmqd,  void *, unsigned int, void *);
#undef FPTR

#define GPU_ID      43858
#define GPUVA_RING  0x1000000ULL
#define GPUVA_WPTR  0x1010000ULL
#define GPUVA_RPTR  0x1020000ULL
#define GPUVA_EOP   0x1030000ULL
#define GPUVA_KERN  0x1040000ULL   /* kernel code buffer */
#define GPUVA_RESULT 0x1050000ULL   /* result buffer for magic value */
#define GPUVA_ARGS   0x1060000ULL   /* kernel args buffer */
#define GPUVA_IB     0x1070000ULL   /* indirect buffer for shader setup */
#define RING_BYTES  (32ULL * 1024)

/* CIK PM4 packet helpers */
#define PACKET3(op, n)  ((0xC0000000U) | ((n) << 16) | ((op) << 8))
#define PACKET3_SET_SH_REG      0x76
#define PACKET3_SET_SH_REG_START 0x00002c00
#define PACKET3_DISPATCH_DIRECT 0x15

enum { QP_TYPE=0, QP_FORMAT=4, QP_QUEUE_ADDR=16, QP_QUEUE_SIZE=24,
       QP_PRIORITY=32, QP_QUEUE_PERCENT=36, QP_READ_PTR=40, QP_WRITE_PTR=48,
       QP_WPTR_BO=160, QP_RPTR_BO=168, QP_RING_BO=176 };

/* ── State ── */
static struct file *kfd_file, *drm_file;
static void *kfd_proc, *ring_mem, *wptr_mem, *rptr_mem, *eop_mem;
static void *kern_mem, *result_mem, *args_mem, *ib_mem;
static unsigned int qid;
static bool q_ok;
/* R(short, "full_symbol_name") */
#define R(sym, str) do { \
	unsigned long a = kln(str); \
	if (!a) { pr_err("hk_queue: " str " NOT FOUND\n"); return -ENOENT; } \
	f_##sym = (void *)a; \
} while (0)

/* ── Helpers ── */
static void *find_pqm(void *proc)
{
	unsigned long pv = (unsigned long)proc;
	for (int o = 64; o < 2048; o += 8) {
		unsigned long v0 = *(unsigned long *)((char *)proc + o);
		unsigned long v1 = *(unsigned long *)((char *)proc + o + 8);
		unsigned long v2 = *(unsigned long *)((char *)proc + o + 16);
		if (v0 == pv && v1 == pv + o + 8 && v2 == pv + o + 8)
			return (char *)proc + o;
	}
	return NULL;
}

static void *find_bo(void *kmem) {
	return *(void **)((char *)kmem + 32);
}

/* CIK MQD structure (relevant subset from cik_structs.h) */
struct cik_mqd_partial {
	uint32_t header;               /*   0 */
	uint32_t dispatch_initiator;   /*   4 */
	uint32_t dim[6];               /*   8 */
	uint32_t num_thread[3];        /*  32 */
	uint32_t _pad1[6];             /*  44 */
	uint32_t compute_pgm_lo;       /*  68 = offset 0x44 */
	uint32_t compute_pgm_hi;       /*  72 = offset 0x48 */
	uint32_t compute_tba_lo;       /*  76 */
	uint32_t compute_tba_hi;       /*  80 */
	uint32_t compute_tma_lo;       /*  84 */
	uint32_t compute_tma_hi;       /*  88 */
	uint32_t compute_pgm_rsrc1;    /*  92 = offset 0x5C */
	uint32_t compute_pgm_rsrc2;    /*  96 = offset 0x60 */
	uint32_t compute_vmid;          /* 100 */
	uint32_t compute_resource_limits; /* 104 */
	uint32_t _pad2[6];             /* 108 */
	uint32_t compute_user_data_0;  /* 132 = offset 0x84 */
	uint32_t compute_user_data_1;  /* 136 */
	uint32_t compute_user_data_2;  /* 140 */
	uint32_t compute_user_data_3;  /* 144 */
	uint32_t compute_user_data_4;  /* 148 */
	uint32_t compute_user_data_5;  /* 152 */
	uint32_t compute_user_data_6;  /* 156 */
	uint32_t compute_user_data_7;  /* 160 */
	uint32_t compute_user_data_8;  /* 164 */
	uint32_t compute_user_data_9;  /* 168 */
	uint32_t compute_user_data_10; /* 172 */
	uint32_t compute_user_data_11; /* 176 */
	uint32_t compute_user_data_12; /* 180 */
	uint32_t compute_user_data_13; /* 184 */
	uint32_t compute_user_data_14; /* 188 */
	uint32_t compute_user_data_15; /* 192 */
};

/* ── Pre-compiled hello_gcn GCN ISA (from hello_gcn.hsaco .text) ── */
/*
 * We hardcode the GCN instructions to avoid ELF parsing in kernel space.
 * These are the raw GCN ISA bytes for our hello_gcn kernel.
 *
 * To verify: llvm-objcopy -O binary -j .text hello_gcn.hsaco hello_gcn.bin
 *             xxd -i hello_gcn.bin
 */
static const uint32_t hello_gcn_code[] = {
	/* Actual bytes from hello_gcn.hsaco .text section (kernel 1: hello_gcn) */
	0xC0440100, 0xBF8C007F, 0x7E000208, 0x7E020209,
	0x7E0402FF, 0x21544148, 0xDC700000, 0x00000200,
	0xBF810000,
};
#define CODE_DWORDS (sizeof(hello_gcn_code) / 4)

/* Kernel descriptor for hello_gcn (simplified, valid for CIK gfx700) */
static const uint32_t hello_gcn_rsrc1 = (4 - 1)        /* VGPRs: 4 (encoded as n-1) */
	| ((16 - 1) << 6)    /* SGPRs: 16 (base) */
	| (3 << 12)          /* Priority: 3 */
	| (1 << 20);          /* IEEE mode */
static const uint32_t hello_gcn_rsrc2 = (0 << 0)       /* Scratch: none */
	| (1 << 6)           /* USER_SGPR: 1 (s[0:1] = args ptr) */
	| (0 << 15);          /* No TGID/TIDIG */

static int __init hk_queue_init(void)
{
	int ret;
	uint32_t fl = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;

	if (!kln_addr) { pr_err("need kln_addr\n"); return -EINVAL; }
	kln = (void *)kln_addr;

	R(cp,     "kfd_create_process");
	R(ur,     "kfd_unref_process");
	R(pdd,    "kfd_process_device_data_by_id");
	R(ivm,    "kfd_process_device_init_vm");
	R(bind,   "kfd_bind_process_to_device");
	R(pqm,    "pqm_create_queue");
	R(dqm,    "pqm_destroy_queue");
	R(gpq,    "pqm_get_user_queue");
	R(alloc,  "amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu");
	R(map,    "amdgpu_amdkfd_gpuvm_map_memory_to_gpu");
	R(free,   "amdgpu_amdkfd_gpuvm_free_memory_of_gpu");
	R(db,     "kfd_alloc_process_doorbells");
	R(bokptr, "amdgpu_bo_kptr");
	R(bokmap, "amdgpu_bo_kmap");
	R(upmqd,  "pqm_update_mqd");

	/* 1. Open files, create process */
	kfd_file = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(kfd_file)) return PTR_ERR(kfd_file);
	drm_file = filp_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(drm_file)) return PTR_ERR(drm_file);
	kfd_proc = kfd_file->private_data;
	pr_info("hk_queue: proc=%px\n", kfd_proc);

	/* 2. Init VM + bind */
	void *pdd = f_pdd(kfd_proc, GPU_ID);
	if (!pdd) return -EINVAL;
	ret = f_ivm(pdd, drm_file);
	if (ret) return ret;
	void *node = *(void **)pdd;
	pdd = f_bind(node, kfd_proc);
	if (IS_ERR(pdd)) return PTR_ERR(pdd);
	void *adev = *(void **)((char *)node + 8);

	void *dpriv = NULL;
	for (int o = 200; o < 1000; o += 8)
		if (*(void **)((char *)pdd + o) == drm_file)
			{ dpriv = *(void **)((char *)pdd + o + 8); break; }
	if (!dpriv) return -EIO;

	/* 3. Alloc + map ring/wptr/rptr/eop + kernel + result */
	uint64_t off;
	{ uint64_t va = GPUVA_RING; ret = f_alloc(adev, va, RING_BYTES, dpriv, &ring_mem, &off, fl, false); if (ret) return ret; ret = f_map(adev, ring_mem, dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_WPTR; ret = f_alloc(adev, va, 4096, dpriv, &wptr_mem, &off, fl, false); if (ret) return ret; ret = f_map(adev, wptr_mem, dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_RPTR; ret = f_alloc(adev, va, 4096, dpriv, &rptr_mem, &off, fl, false); if (ret) return ret; ret = f_map(adev, rptr_mem, dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_EOP;  ret = f_alloc(adev, va, 4096, dpriv, &eop_mem,  &off, fl, false); if (ret) return ret; ret = f_map(adev, eop_mem,  dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_KERN;  fl |= KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE; ret = f_alloc(adev, va, 4096, dpriv, &kern_mem, &off, fl, false); fl &= ~KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE; if (ret) return ret; ret = f_map(adev, kern_mem, dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_RESULT; ret = f_alloc(adev, va, 4096, dpriv, &result_mem, &off, fl, false); if (ret) return ret; ret = f_map(adev, result_mem, dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_ARGS;   ret = f_alloc(adev, va, 4096, dpriv, &args_mem,   &off, fl, false); if (ret) return ret; ret = f_map(adev, args_mem,   dpriv); if (ret) return ret; }
	{ uint64_t va = GPUVA_IB;     ret = f_alloc(adev, va, 4096, dpriv, &ib_mem,     &off, fl, false); if (ret) return ret; ret = f_map(adev, ib_mem,     dpriv); if (ret) return ret; }
	pr_info("hk_queue: all buffers allocated\n");

	/* 4. Build queue_properties */
	char qp[256] = {0};
	*(int *)(qp + QP_TYPE) = 0; *(int *)(qp + QP_FORMAT) = 0;
	*(uint64_t *)(qp + QP_QUEUE_ADDR) = GPUVA_RING;
	*(uint64_t *)(qp + QP_QUEUE_SIZE) = RING_BYTES;
	*(uint32_t *)(qp + QP_PRIORITY) = 1;
	*(uint32_t *)(qp + QP_QUEUE_PERCENT) = 100;
	*(uint64_t *)(qp + QP_READ_PTR) = GPUVA_RPTR;
	*(uint64_t *)(qp + QP_WRITE_PTR) = GPUVA_WPTR;
	*(void **)(qp + QP_WPTR_BO) = find_bo(wptr_mem);
	*(void **)(qp + QP_RPTR_BO) = find_bo(rptr_mem);
	*(void **)(qp + QP_RING_BO) = find_bo(ring_mem);
	*(void **)(qp + 184) = find_bo(eop_mem);
	*(uint64_t *)(qp + 104) = GPUVA_EOP;
	*(uint32_t *)(qp + 112) = 4096;

	/* 5. Doorbells + 6. Create queue */
	void *pqm = find_pqm(kfd_proc);
	if (!pqm) return -EIO;
	void *kfd_dev = *(void **)((char *)node + 216);
	if (!kfd_dev) return -EIO;
	ret = f_db(kfd_dev, pdd);
	if (ret) return ret;

	uint32_t db_off;
	void *queue_mqd = NULL;
	void *q = NULL;  /* struct queue *, used in steps 8-9 */

	ret = f_pqm(pqm, node, NULL, qp, &qid, NULL, NULL, NULL, &db_off);
	if (ret) { pr_err("hk_queue: create_queue failed: %d\n", ret); return ret; }
	q_ok = true;
	pr_info("hk_queue: QUEUE id=%u db=%u\n", qid, db_off);

	/* 7. Load kernel code into GPU memory via kmap */
	{
		void *bo = find_bo(kern_mem);
		void *cpu;
		ret = f_bokmap(bo, &cpu);
		if (ret) { pr_err("hk_queue: bo_kmap failed: %d\n", ret); return ret; }
		memcpy(cpu, hello_gcn_code, sizeof(hello_gcn_code));
		pr_info("hk_queue: kernel code loaded (%zu bytes at %px)\n",
			sizeof(hello_gcn_code), cpu);
	}

	/* Load kernel args buffer: { target_addr_lo, target_addr_hi } */
	{
		void *bo = find_bo(args_mem);
		void *cpu;
		ret = f_bokmap(bo, &cpu);
		if (ret) { pr_err("hk_queue: args kmap failed: %d\n", ret); return ret; }
		uint64_t target = GPUVA_RESULT;
		memcpy(cpu, &target, 8);
		pr_info("hk_queue: args buffer loaded (target=0x%llx)\n",
			(unsigned long long)target);
	}

	/* 8. Get queue MQD and update shader registers */
	q = f_gpq(pqm, qid);
	if (!q) { pr_err("hk_queue: get_user_queue failed\n"); return -EIO; }
	queue_mqd = *(void **)((char *)q + 16);  /* q->mqd at offset 16 */
	uint32_t *mqd = queue_mqd;

	uint32_t pgm_lo = (GPUVA_KERN >> 8) & 0xFFFFFFFF;
	uint32_t pgm_hi = (GPUVA_KERN >> 40) & 0xFF;

	pr_info("hk_queue: setting MQD: pgm=0x%08x_%02x rsrc1=0x%08x rsrc2=0x%08x\n",
		pgm_lo, pgm_hi, hello_gcn_rsrc1, hello_gcn_rsrc2);

	/* compute_pgm_lo at offset 0x44 bytes = 17 dwords */
	mqd[17] = pgm_lo;
	/* compute_pgm_hi at offset 0x48 = 18 dwords */
	mqd[18] = pgm_hi;
	/* compute_pgm_rsrc1 at offset 0x5C = 23 dwords */
	mqd[23] = hello_gcn_rsrc1;
	/* compute_pgm_rsrc2 at offset 0x60 = 24 dwords */
	mqd[24] = hello_gcn_rsrc2;

	/* compute_user_data_0/1 = GPUVA_ARGS (s[0:1] = args ptr) */
	mqd[33] = GPUVA_ARGS & 0xFFFFFFFF;
	mqd[34] = (GPUVA_ARGS >> 32) & 0xFFFFFFFF;

	/* Set MQD default dispatch initiator (offset 4 bytes = dword 1) */
	mqd[1] = (1 << 0) | (1 << 2); /* COMPUTE_SHADER_EN | FORCE_START_AT_000 */

	pr_info("hk_queue: MQD updated (dispatch_initiator=0x%x)\n", mqd[1]);

	/* Flush MQD to GPU via KFD's update_mqd */
	ret = f_upmqd(pqm, qid, NULL);
	pr_info("hk_queue: pqm_update_mqd: %d\n", ret);

	/* 9. Build Indirect Buffer (IB) + submit INDIRECT_BUFFER to ring */
	{
		void *ib_bo = find_bo(ib_mem), *ib_cpu;
		ret = f_bokmap(ib_bo, &ib_cpu);
		if (ret) { pr_err("hk_queue: ib kmap failed: %d\n", ret); return ret; }

		uint32_t pgm_lo = (GPUVA_KERN >> 8) & 0xFFFFFFFF;
		uint32_t pgm_hi = (GPUVA_KERN >> 40) & 0xFF;
		uint32_t udata_lo = GPUVA_ARGS & 0xFFFFFFFF;
		uint32_t udata_hi = (GPUVA_ARGS >> 32) & 0xFFFFFFFF;

		uint32_t *ib = (uint32_t *)ib_cpu;
		int ib_idx = 0;

		/* SET_SH_REG(5): COMPUTE_PGM_LO through COMPUTE_USER_DATA_1 */
		ib[ib_idx++] = PACKET3(PACKET3_SET_SH_REG, 5);
		ib[ib_idx++] = 0x2E0C - PACKET3_SET_SH_REG_START;
		ib[ib_idx++] = pgm_lo;
		ib[ib_idx++] = pgm_hi;
		ib[ib_idx++] = hello_gcn_rsrc1;
		ib[ib_idx++] = hello_gcn_rsrc2;
		ib[ib_idx++] = udata_lo;
		ib[ib_idx++] = udata_hi;

		/* SET_SH_REG(1): COMPUTE_DISPATCH_INITIATOR */
		ib[ib_idx++] = PACKET3(PACKET3_SET_SH_REG, 1);
		ib[ib_idx++] = 0x2E04 - PACKET3_SET_SH_REG_START;
		ib[ib_idx++] = (1 << 0) | (1 << 2);

		/* DISPATCH_DIRECT */
		ib[ib_idx++] = PACKET3(PACKET3_DISPATCH_DIRECT, 4);
		ib[ib_idx++] = 1 | (1 << 16);  /* dim_xy */
		ib[ib_idx++] = 1;              /* dim_z */
		ib[ib_idx++] = (1 << 0) | (1 << 2);  /* dispatch_initiator */
		ib[ib_idx++] = 0;              /* reserved */

		uint32_t ib_dw = ib_idx;
		pr_info("hk_queue: IB built: %u dwords at GPUVA 0x%llx\n",
			ib_dw, (unsigned long long)GPUVA_IB);

		/* Write INDIRECT_BUFFER to ring buffer */
		void *ring_bo = find_bo(ring_mem), *ring_cpu_raw;
		void *wptr_bo = find_bo(wptr_mem), *wptr_cpu_raw;
		ret = f_bokmap(ring_bo, &ring_cpu_raw);
		if (ret) return ret;
		ret = f_bokmap(wptr_bo, &wptr_cpu_raw);
		if (ret) return ret;

		uint32_t *ring_cpu = ring_cpu_raw;
		uint32_t *wptr_cpu = wptr_cpu_raw;
		uint32_t wptr_val = *wptr_cpu;
		uint32_t ridx = (wptr_val / 4) % (RING_BYTES / 4);

		/* INDIRECT_BUFFER packet (count=2, 3 data dwords) */
		ring_cpu[ridx + 0] = PACKET3(0x3F, 2);  /* IT_INDIRECT_BUFFER */
		ring_cpu[ridx + 1] = GPUVA_IB & 0xFFFFFFFC;
		ring_cpu[ridx + 2] = (GPUVA_IB >> 32) & 0xFFFF;
		ring_cpu[ridx + 3] = ib_dw;  /* length in dwords */

		/* Advance wptr (4 dwords) */
		wptr_val += 4 * 4;
		*wptr_cpu = wptr_val;

		/* Ring doorbell */
		int db_rung = 0;
		void *doorbell_bo = *(void **)((char *)pdd + 176);
		if (doorbell_bo) {
			void *db_cpu = f_bokptr(doorbell_bo);
			if (db_cpu && q) {
				uint32_t db_id = *(uint32_t *)((char *)q + 260);
				*(uint32_t *)((uint32_t *)db_cpu + db_id) = wptr_val;
				pr_info("hk_queue: doorbell RUNG id=%u val=0x%x\n",
					db_id, wptr_val);
				db_rung = 1;
			}
		}
		if (!db_rung)
			pr_info("hk_queue: doorbell skipped\n");

		pr_info("hk_queue: INDIRECT_BUFFER submitted, wptr=0x%x\n", wptr_val);
	}

	/* 10. Wait and verify result */
	msleep(500);  /* Give GPU time to execute (500ms) */
	dma_wmb();
	{
		void *r_bo = find_bo(result_mem), *r_cpu;
		ret = f_bokmap(r_bo, &r_cpu);
		if (ret == 0) {
			uint32_t magic = *(uint32_t *)r_cpu;
			pr_info("hk_queue: RESULT at GPUVA 0x%llx: 0x%08x %s\n",
				(unsigned long long)GPUVA_RESULT, magic,
				magic == 0x21544148 ? "MATCH!" : "(no match)");
		}
	}

	pr_info("hk_queue: ==================================\n");
	pr_info("hk_queue: QUEUE CREATED + DISPATCHED!\n");
	pr_info("hk_queue: ==================================\n");
	return 0;
}

static void __exit hk_queue_exit(void)
{
	if (q_ok) {
		void *pqm = find_pqm(kfd_proc);
		if (pqm) f_dqm(pqm, qid);
	}
	if (kfd_file) filp_close(kfd_file, NULL);
	if (drm_file) filp_close(drm_file, NULL);
	pr_info("hk_queue: unloaded\n");
}

module_init(hk_queue_init);
module_exit(hk_queue_exit);
