/*
 * hk_queue.c — HeteroKern: create compute queue via KFD internals
 *
 * Calls pqm_create_queue directly after correctly locating pqm
 * in kfd_process (last self-pointer, typically around offset 800).
 * Uses exact queue_properties field offsets from kfd_priv.h.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/kfd_ioctl.h>

MODULE_LICENSE("GPL");

static ulong kln_addr;
module_param(kln_addr, ulong, 0);
static unsigned long (*kln)(const char *);

/* ── Function pointers ── */
static void *(*f_cp)(struct task_struct *);
static void  (*f_ur)(void *);
static void *(*f_pdd)(void *, uint32_t);
static int   (*f_ivm)(void *, struct file *);
static void *(*f_bind)(void *, void *);
static int   (*f_pqm)(void *, void *, void *, void *, unsigned int *,
		      const void *, const void *, const void *, uint32_t *);
static int   (*f_dqm)(void *, unsigned int);
static int   (*f_alloc)(void *, uint64_t, uint64_t, void *, void **,
			uint64_t *, uint32_t, bool);
static int   (*f_map)(void *, void *, void *);
static int   (*f_free)(void *, void *, void *, uint64_t *);
static int   (*f_doorbell)(void *, void *); /* kfd_alloc_process_doorbells */
static struct file *kfd_file, *drm_file;
static void *kfd_proc, *ring_mem, *wptr_mem, *rptr_mem;
static unsigned int qid;
static bool q_ok;

#define GPU_ID      43858
#define GPUVA_RING  0x1000000ULL
#define GPUVA_WPTR  0x1010000ULL
#define GPUVA_RPTR  0x1020000ULL
#define RING_BYTES  (32ULL * 1024)

/* ── Verified offsets for queue_properties on 6.12.86 ── */
enum { QP_TYPE=0, QP_FORMAT=4, QP_QUEUE_ADDR=16, QP_QUEUE_SIZE=24,
       QP_PRIORITY=32, QP_QUEUE_PERCENT=36, QP_READ_PTR=40, QP_WRITE_PTR=48,
       QP_WPTR_BO=160, QP_RPTR_BO=168, QP_RING_BO=176 };

static int resolve(void *name, void **fn)
{
	unsigned long a = kln(name);
	if (!a) { pr_err("hk_queue: %s not found\n", (char *)name); return -ENOENT; }
	*fn = (void *)a;
	return 0;
}

static void *find_pqm(void *proc)
{
	unsigned long pv = (unsigned long)proc;

	/*
	 * pqm layout: [process*, queues.next, queues.prev, bitmap*]
	 * An empty list_head has next==prev==&head.
	 * So we search for:
	 *   val[0] == proc
	 *   val[1] == proc + offset + 8  (self-ptr to queues head)
	 *   val[2] == proc + offset + 8  (same)
	 */
	for (int o = 64; o < 2048; o += 8) {
		unsigned long v0 = *(unsigned long *)((char *)proc + o);
		unsigned long v1 = *(unsigned long *)((char *)proc + o + 8);
		unsigned long v2 = *(unsigned long *)((char *)proc + o + 16);

		if (v0 == pv && v1 == pv + o + 8 && v2 == pv + o + 8) {
			pr_info("hk_queue: pqm at proc+%d (pattern match)\n", o);
			return (char *)proc + o;
		}
	}
	return NULL;
}

static void *find_bo(void *kmem)
{
	/* struct kgd_mem: mutex(32) + bo*(8) + dmabuf*(8) + ...
	 * bo is at offset 32 (sizeof(struct mutex)). */
	return *(void **)((char *)kmem + 32);
}

static int __init hk_queue_init(void)
{
	int ret;
	uint32_t fl = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
	uint64_t off;

	if (!kln_addr) { pr_err("need kln_addr\n"); return -EINVAL; }
	kln = (void *)kln_addr;

	if (resolve("kfd_create_process", (void **)&f_cp)) return -ENOENT;
	if (resolve("kfd_unref_process",  (void **)&f_ur)) return -ENOENT;
	if (resolve("kfd_process_device_data_by_id", (void **)&f_pdd)) return -ENOENT;
	if (resolve("kfd_process_device_init_vm", (void **)&f_ivm)) return -ENOENT;
	if (resolve("kfd_bind_process_to_device", (void **)&f_bind)) return -ENOENT;
	if (resolve("pqm_create_queue", (void **)&f_pqm)) return -ENOENT;
	if (resolve("pqm_destroy_queue", (void **)&f_dqm)) return -ENOENT;
	if (resolve("amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu", (void **)&f_alloc)) return -ENOENT;
	if (resolve("amdgpu_amdkfd_gpuvm_map_memory_to_gpu", (void **)&f_map)) return -ENOENT;
	if (resolve("amdgpu_amdkfd_gpuvm_free_memory_of_gpu", (void **)&f_free)) return -ENOENT;
	if (resolve("kfd_alloc_process_doorbells", (void **)&f_doorbell)) return -ENOENT;
	pr_info("hk_queue: symbols OK\n");

	/* 1. Open files, create process */
	kfd_file = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(kfd_file)) return PTR_ERR(kfd_file);
	drm_file = filp_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(drm_file)) return PTR_ERR(drm_file);

	kfd_proc = kfd_file->private_data;
	pr_info("hk_queue: proc=%px\n", kfd_proc);

	/* 2. Init VM first (sets drm_priv, required by bind) */
	void *pdd = f_pdd(kfd_proc, GPU_ID);
	if (!pdd) { pr_err("pdd\n"); return -EINVAL; }

	ret = f_ivm(pdd, drm_file);
	pr_info("hk_queue: init_vm: %d\n", ret);
	if (ret) return ret;

	/* 3. Bind (requires drm_priv to be set) */
	void *node = *(void **)pdd;
	void *pdd_bound = f_bind(node, kfd_proc);
	if (IS_ERR(pdd_bound)) {
		pr_err("hk_queue: bind failed: %ld\n", PTR_ERR(pdd_bound));
		return PTR_ERR(pdd_bound);
	}
	pr_info("hk_queue: bind OK, pdd=%px\n", pdd_bound);

	/* Get node + adev + drm_priv from PDD */
	void *adev = *(void **)((char *)node + 8);
	pr_info("hk_queue: node=%px adev=%px\n", node, adev);

	void *dpriv = NULL;
	for (int o = 200; o < 1000; o += 8) {
		if (*(void **)((char *)pdd + o) == drm_file) {
			dpriv = *(void **)((char *)pdd + o + 8);
			pr_info("hk_queue: drm_priv at PDD+%d = %px\n", o+8, dpriv);
			break;
		}
	}
	if (!dpriv) { pr_err("dpriv\n"); return -EIO; }

	/* 3. Alloc ring/wptr/rptr */
	ret = f_alloc(adev, GPUVA_RING, RING_BYTES, dpriv, &ring_mem, &off, fl, false);
	if (ret) { pr_err("alloc ring=%d\n", ret); return ret; }
	ret = f_alloc(adev, GPUVA_WPTR, 4096, dpriv, &wptr_mem, &off, fl, false);
	if (ret) { pr_err("alloc wptr=%d\n", ret); return ret; }
	ret = f_alloc(adev, GPUVA_RPTR, 4096, dpriv, &rptr_mem, &off, fl, false);
	if (ret) { pr_err("alloc rptr=%d\n", ret); return ret; }

	/* Also allocate EOP buffer — CIK compute queues require it */
	void *eop_mem;
	uint64_t eop_gpuva = 0x1030000ULL;
	ret = f_alloc(adev, eop_gpuva, 4096, dpriv, &eop_mem, &off, fl, false);
	if (ret) { pr_err("alloc eop=%d\n", ret); return ret; }
	ret = f_map(adev, eop_mem, dpriv);
	if (ret) { pr_err("map eop=%d\n", ret); return ret; }
	pr_info("hk_queue: EOP alloc+map OK\n");
	pr_info("hk_queue: alloc OK\n");

	/* 4. Map to GPU VM */
	ret = f_map(adev, ring_mem, dpriv);
	if (ret) { pr_err("map ring=%d\n", ret); return ret; }
	ret = f_map(adev, wptr_mem, dpriv);
	if (ret) { pr_err("map wptr=%d\n", ret); return ret; }
	ret = f_map(adev, rptr_mem, dpriv);
	if (ret) { pr_err("map rptr=%d\n", ret); return ret; }
	pr_info("hk_queue: map OK\n");

	/* 5. Build queue_properties with verified offsets */
	char qp[256];
	memset(qp, 0, 256);
	*(int *)(qp + QP_TYPE)          = 0; /* COMPUTE */
	*(int *)(qp + QP_FORMAT)        = 0; /* PM4 */
	*(uint64_t *)(qp + QP_QUEUE_ADDR)  = GPUVA_RING;
	*(uint64_t *)(qp + QP_QUEUE_SIZE)  = RING_BYTES;
	*(uint32_t *)(qp + QP_PRIORITY)     = 1;
	*(uint32_t *)(qp + QP_QUEUE_PERCENT)= 100;
	*(uint64_t *)(qp + QP_READ_PTR)     = GPUVA_RPTR;
	*(uint64_t *)(qp + QP_WRITE_PTR)    = GPUVA_WPTR;

	/* EOP buffer */
	*(void **)(qp + QP_WPTR_BO) = find_bo(wptr_mem);
	*(void **)(qp + QP_RPTR_BO) = find_bo(rptr_mem);
	*(void **)(qp + QP_RING_BO) = find_bo(ring_mem);
	*(void **)(qp + 184)        = find_bo(eop_mem);  /* eop_buf_bo at offset 184 */
	*(uint64_t *)(qp + 104)     = eop_gpuva;         /* eop_ring_buffer_address at offset 104 */
	*(uint32_t *)(qp + 112)     = 4096;               /* eop_ring_buffer_size at offset 112 */

	pr_info("hk_queue: BOs: wptr=%px rptr=%px ring=%px\n",
		*(void **)(qp + QP_WPTR_BO),
		*(void **)(qp + QP_RPTR_BO),
		*(void **)(qp + QP_RING_BO));

	/* 6. Find the REAL pqm (last self-ptr in kfd_process) */
	void *pqm = find_pqm(kfd_proc);
	if (!pqm) { pr_err("pqm not found\n"); return -EIO; }

	/* Allocate process doorbells (required before create_queue on CIK) */
	/* kfd_dev at node+216 (verified from dump: local_mem_info is 16 bytes) */
	void *kfd_dev = *(void **)((char *)node + 216);
	pr_info("hk_queue: kfd_dev at node+216 = %px\n", kfd_dev);
	if (!kfd_dev) { pr_err("kfd_dev is NULL at node+224\n"); return -EIO; }

	ret = f_doorbell(kfd_dev, pdd);
	pr_info("hk_queue: doorbell alloc: %d\n", ret);
	if (ret) { pr_err("doorbell failed: %d\n", ret); return ret; }

	/* 7. Create queue */
	uint32_t db;
	ret = f_pqm(pqm, node, NULL, qp, &qid, NULL, NULL, NULL, &db);
	if (ret) {
		pr_err("hk_queue: pqm_create_queue FAILED: %d\n", ret);
		return ret;
	}
	q_ok = true;
	pr_info("hk_queue: ==================================\n");
	pr_info("hk_queue: QUEUE CREATED! id=%u db_off=0x%x\n", qid, db);
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
