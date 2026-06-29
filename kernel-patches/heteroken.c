/*
 * HeteroKern — GCN compute dispatch on GFX9 (Raven Ridge)
 *
 * ==========================================================================
 * PHASES COMPLETE:
 *   P1 — GPU wavefront executes + magic value
 *   P2 — Mailbox protocol + IH interrupt → CPU wakeup
 *   P3 — GCN task lifecycle (kthread → GPU → IH wake → CPU → repeat)
 *  +hello  — "hello from CU\n" shader + result_show sysfs node
 *
 * ==========================================================================
 * DISPATCH LAYER
 *   Current: kernel compute ring + IB (PACKET3, VMID 0)
 *   Future:  KFD PM4 queue (PACKET3_COMPUTE, VMID 8) — swap in hk_dispatch_ctx()
 * ==========================================================================
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sysfs.h>
#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <drm/drm_cache.h>

#include "kfd_priv.h"
#include "../amdgpu/amdgpu_amdkfd.h"
#include "gc/gc_9_0_offset.h"
#include "gc/gc_9_0_sh_mask.h"
#include "../amdgpu/soc15_common.h"
#include "../amdgpu/soc15d.h"
#include "../amdgpu/amdgpu.h"
#include "../amdgpu/amdgpu_ring.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern GCN compute dispatch on GFX9");

/* ==========================================================================
 * Shader binaries (built on dev machine 192.168.1.8, --mcpu=gfx902)
 * ========================================================================== */

/* 8-dword "hello": writes magic 0x21544148 to s[0:1] */
static const uint32_t hello_shader[] = {
	0xBF8CC07F, 0x7E000200, 0x7E020201,
	0x7E0402FF, 0x21544148,
	0xDC700000, 0x00000200,
	0xBF810000,
};

/* 23-dword "hello_string": writes "hello from CU\n" to s[0:1] */
static const uint32_t hello_string[] = {
	0xBF8CC07F,                /* s_waitcnt lgkmcnt(0) */
	0x7E000200,                /* v_mov_b32_e32 v0, s0 */
	0x7E020201,                /* v_mov_b32_e32 v1, s1 */
	0x7E0402FF, 0x6C6C6568,   /* v2="hell" → store */
	0xDC700000, 0x00000200,
	0x68000084,                /* v_add_u32_e32 v0,4,v0 */
	0x7E0402FF, 0x7266206F,   /* v2="o fr" → store */
	0xDC700000, 0x00000200,
	0x68000084,
	0x7E0402FF, 0x43206D6F,   /* v2="om C" → store */
	0xDC700000, 0x00000200,
	0x68000084,
	0x7E0402FF, 0x00000A55,   /* v2="U\n\0" → store */
	0xDC700000, 0x00000200,
	0xBF810000,                /* s_endpgm */
};
static const uint32_t mailbox_shader[] = {
	0xBF8CC07F,                /* s_waitcnt lgkmcnt(0) */
	0x7E000200,                /* v_mov_b32_e32 v0, s0 */
	0x7E020201,                /* v_mov_b32_e32 v1, s1 */
	0x7E0402FF, 0x21544148,    /* v2=magic → store */
	0xDC700000, 0x00000200,
	0x68000084,                /* v_add_u32_e32 v0,4,v0 */
	0x7E040281,                /* v2=1 → store state */
	0xDC700000, 0x00000200,
	0x68000084,
	0x7E0402AA,                /* v2=42 → store syscall */
	0xDC700000, 0x00000200,
	0x68000084,
	0x7E0402FF, 0xDEADBEEF,    /* v2=0xDEADBEEF → arg0 */
	0xDC700000, 0x00000200,
	0x68000084,
	0x7E0402FF, 0xCAFEBABE,    /* v2=0xCAFEBABE → arg1 */
	0xDC700000, 0x00000200,
	0xBF810000,                /* s_endpgm */
};

/* Minimal trap handler: return immediately from MSG_INTERRUPT trap. */
static const uint32_t trap_return_shader[] = {
	0xBE80016C, /* s_mov_b64 s[0:1], ttmp[0:1] */
	0xBE801F00, /* s_rfe_b64 s[0:1] */
};
#define MAGIC 0x21544148

/* Compute register defaults */
#define HK_VGPRS     1   /* (1+1)*4 = 8 VGPR */
#define HK_SGPRS     1
#define HK_USER_SGPR 2
#define HK_THREADS   64

/*
 * Extended mailbox layout (shared between GPU and CPU):
 *   +0x00: state        (u32)   0=idle, 1=syscall_pending, 3=exit
 *   +0x04: reserved
 *   +0x08: syscall_nr   (u32)
 *   +0x0C: reserved
 *   +0x10: arg0         (u64)   fd for write()
 *   +0x18: arg1         (u64)
 *   +0x20: arg2         (u64)   count for write()
 *   +0x28: arg3         (u64)
 *   +0x30: arg4         (u64)
 *   +0x38: retval       (u64)
 *   +0x40: data[128]            string/data buffer (0x40 - 0xBF)
 *   +0xC0: input_addr   (u64)   GPU address of input BO
 *   +0xC8: output_addr  (u64)   GPU address of output BO
 *   +0xD0: width        (u32)
 *   +0xD4: height       (u32)
 *   +0xD8: input_size   (u32)
 *   +0xDC: output_size  (u32)
 *
 * Minimum mailbox BO size: 4096 bytes (PAGE_SIZE).
 */
#define HK_MB_INPUT_ADDR   0xC0
#define HK_MB_OUTPUT_ADDR  0xC8
#define HK_MB_WIDTH        0xD0
#define HK_MB_HEIGHT       0xD4
#define HK_MB_INPUT_SIZE   0xD8
#define HK_MB_OUTPUT_SIZE  0xDC

/* Mailbox structure (shared between GPU and CPU) */
struct hk_mailbox {
	uint32_t magic;
	uint32_t state;
	uint32_t syscall_nr;
	uint32_t arg0;
	uint32_t arg1;
};

/* ==========================================================================
 * Device discovery
 * ========================================================================== */
static struct amdgpu_device *hk_get_adev(void)
{
	struct kfd_process *proc;
	struct file *f;

	f = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(f)) {
		pr_err("hk: /dev/kfd: %ld\n", PTR_ERR(f));
		return NULL;
	}
	proc = f->private_data;
	for (int i = 0; i < proc->n_pdds; i++)
		if (proc->pdds[i] && proc->pdds[i]->dev &&
		    proc->pdds[i]->dev->adev)
			return proc->pdds[i]->dev->adev;
	pr_err("hk: no PDD with adev\n");
	return NULL;
}

/* ==========================================================================
 * GCN task context — one per spawned GCN task
 *
 *   task_comm = thread name  (kthread → GCN → CPU → ...)
 *   mailbox  = GPU-accessible BO for GPU→CPU communication
 *   fence_cb = dma_fence callback — fires from IH IRQ context, wakes task
 * ========================================================================== */
struct hk_gcn_ctx {
	struct amdgpu_device *adev;

	/* Persistent mailbox BO */
	struct amdgpu_bo *mb_bo;
	void             *mb_cpu;
	u64               mb_gpu_addr;
	u32               mb_dwords;

	/* Owning task (NULL until spawned) */
	struct task_struct *task;

	/* Async completion: fence callback wakes the owning task */
	struct dma_fence_cb fence_cb;
	struct dma_fence   *fence;       /* held until callback fires */
	int                 fence_ret;   /* result of add_callback */
};

static void hk_gcn_wakeup(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct hk_gcn_ctx *ctx = container_of(cb, struct hk_gcn_ctx, fence_cb);

	pr_info("hk: *** IH → wake %s [%d] (IRQ context: %s) ***\n",
		ctx->task->comm, task_pid_nr(ctx->task),
		in_irq() ? "yes" : "no");

	dma_fence_put(f);
	ctx->fence = NULL;
	wake_up_process(ctx->task);
}

/* Allocate mailbox BO for a GCN task context */
static int hk_gcn_mailbox_alloc(struct hk_gcn_ctx *ctx, u32 dwords)
{
	int ret = amdgpu_bo_create_kernel(ctx->adev, dwords * 4, PAGE_SIZE,
					  AMDGPU_GEM_DOMAIN_GTT,
					  &ctx->mb_bo,
					  &ctx->mb_gpu_addr,
					  &ctx->mb_cpu);
	if (ret)
		return ret;
	ctx->mb_dwords = dwords;
	memset(ctx->mb_cpu, 0, dwords * 4);
	return 0;
}

static void hk_gcn_mailbox_free(struct hk_gcn_ctx *ctx)
{
	if (ctx->mb_bo)
		amdgpu_bo_free_kernel(&ctx->mb_bo,
				      &ctx->mb_gpu_addr,
				      &ctx->mb_cpu);
	ctx->mb_bo = NULL;
}

/* ==========================================================================
 * Dispatch: submit shader on kernel compute ring via IB.
 *
 * Uses the GCN context's pre-allocated mailbox BO (ctx->mb_gpu_addr →
 * COMPUTE_USER_DATA_0/1).  The calling task MUST be in TASK_UNINTERRUPTIBLE
 * state — this function calls schedule() after registering the fence
 * callback, and returns only after the callback fires (IH → wake_up_process).
 * ========================================================================== */
static int hk_dispatch_ctx(struct hk_gcn_ctx *ctx,
			   const uint32_t *shader, u32 shader_dwords)
{
	struct amdgpu_device *adev = ctx->adev;
	struct amdgpu_ring *kring = &adev->gfx.compute_ring[0];
	struct amdgpu_ib ib;
	u32 shader_off = 256, total_size = shader_off + 256;
	u64 pgm_addr;
	int ret;

	if (!kring->sched.ready)
		return -ENODEV;

	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size,
			    AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret)
		return ret;

	memset(ib.ptr, 0, total_size / 4);
	for (u32 i = 0; i < shader_dwords; i++)
		ib.ptr[i + (shader_off / 4)] = shader[i];

	/* --- PM4 command stream --- */
	ib.length_dw = 0;

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0;

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = HK_THREADS;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;

	{
		u32 rsrc1 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, HK_VGPRS);
		rsrc1 = REG_SET_FIELD(rsrc1, COMPUTE_PGM_RSRC1, SGPRS, HK_SGPRS);
		rsrc1 = REG_SET_FIELD(rsrc1, COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
		u32 rsrc2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, HK_USER_SGPR);
		ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
		ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1)
					- PACKET3_SET_SH_REG_START;
		ib.ptr[ib.length_dw++] = rsrc1;
		ib.ptr[ib.length_dw++] = rsrc2;
	}

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0xffffffff;
	ib.ptr[ib.length_dw++] = 0xffffffff;

	pgm_addr = (ib.gpu_addr + shader_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(ctx->mb_gpu_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(ctx->mb_gpu_addr);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR,
						COMPUTE_SHADER_EN, 1);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	pr_info("hk: IB %u dwords  gpu=0x%llx  mailbox=0x%llx\n",
		ib.length_dw, ib.gpu_addr, ctx->mb_gpu_addr);

	/* Submit IB, free it immediately (mailbox outlives it) */
	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &ctx->fence);
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret) {
		ctx->fence = NULL;
		return ret;
	}

	/* Register fence callback: IH fires → hk_gcn_wakeup → wake_up_process(this) */
	ctx->fence_ret = dma_fence_add_callback(ctx->fence, &ctx->fence_cb,
						hk_gcn_wakeup);
	if (ctx->fence_ret == -ENOENT) {
		/* Already signaled — wake ourselves immediately */
		hk_gcn_wakeup(ctx->fence, &ctx->fence_cb);
	} else if (ctx->fence_ret) {
		pr_err("hk: fence_add_callback: %d\n", ctx->fence_ret);
		dma_fence_put(ctx->fence);
		ctx->fence = NULL;
		return ctx->fence_ret;
	} else {
		/* Sleep until IH interrupt wakes us via hk_gcn_wakeup */
		pr_info("hk: %s [%d] → GPU (sleep)\n",
			current->comm, task_pid_nr(current));
		schedule();
		pr_info("hk: %s [%d] ← GPU (wake)\n",
			current->comm, task_pid_nr(current));
	}

	return 0;
}

/* ==========================================================================
 * GCN kthread — the "user-mode" GCN task lifecycle
 *
 *   while (alive):
 *     1. Write desired operation to mailbox (or let shader write it)
 *     2. Submit GCN dispatch → sleep (TASK_UNINTERRUPTIBLE)
 *     3. IH interrupt → fence callback → wake_up_process(this)
 *     4. Read mailbox on CPU, "execute" the requested syscall
 *     5. Loop or exit
 *
 * This is the precursor to clone3(CLONE_GCN) — the only difference is
 * how the task is created (kthread vs clone3). The lifecycle pattern
 * (sleep → GPU → IH → wake → mailbox → repeat) is the same.
 * ========================================================================== */
static int hk_gcn_thread(void *data)
{
	struct hk_gcn_ctx *ctx = data;

	ctx->task = current;  /* required for hk_gcn_wakeup → wake_up_process */

	pr_info("hk: GCN task %s [%d] born\n", current->comm, task_pid_nr(current));

	while (!kthread_should_stop()) {
		/* --- Step 1: prepare mailbox (GPU will write into it) --- */
		memset(ctx->mb_cpu, 0, ctx->mb_dwords * 4);

		/* --- Step 2: dispatch → sleep → IH wakes us --- */
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (hk_dispatch_ctx(ctx, mailbox_shader,
				    ARRAY_SIZE(mailbox_shader))) {
			__set_current_state(TASK_RUNNING);
			break;
		}
		__set_current_state(TASK_RUNNING);

		/* --- Step 3: IH woke us! Read the mailbox --- */
		{
			uint32_t *mb = ctx->mb_cpu;
			pr_info("hk: GCN task mailbox: magic=0x%x state=%u syscall=%u args=[0x%x,0x%x]\n",
				mb[0], mb[1], mb[2], mb[3], mb[4]);

			/* --- Step 4: "execute" the requested operation --- */
			if (mb[1] == 1) /* state == syscall_pending */
				pr_info("hk: GCN task would execute syscall %u\n", mb[2]);
		}

		/* --- Step 5: done with one GPU→CPU cycle --- */
	}

	pr_info("hk: GCN task %s [%d] exiting\n", current->comm, task_pid_nr(current));
	return 0;
}

/* ==========================================================================
 * sysfs interface
 * ========================================================================== */

/* Forward — defined below after mailbox alloc functions */
static char hk_last_result[256];
static DEFINE_MUTEX(hk_result_lock);

/* /sys/kernel/heteroken/run — single-shot sync dispatch (P1/P2 compat) */
static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	struct amdgpu_device *adev = hk_get_adev();
	struct amdgpu_ring *kring;
	struct amdgpu_ib ib;
	struct dma_fence *f = NULL;
	struct amdgpu_bo *mb_bo = NULL;
	void *mb_cpu = NULL;
	u64 mb_gpu_addr, pgm_addr;
	u32 shader_off = 256, total_size = shader_off + 256;
	int ret;

	if (!adev) return -ENODEV;
	kring = &adev->gfx.compute_ring[0];
	if (!kring->sched.ready) return -ENODEV;

	/* Mailbox BO */
	ret = amdgpu_bo_create_kernel(adev, 20, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_GTT,
				      &mb_bo, &mb_gpu_addr, &mb_cpu);
	if (ret) return ret;
	memset(mb_cpu, 0, 20);

	/* IB */
	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size,
			    AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret) goto out_free_bo;
	memset(ib.ptr, 0, total_size / 4);
	memcpy(ib.ptr + shader_off/4, mailbox_shader, sizeof(mailbox_shader));

	/* PM4 commands (same pattern as hk_dispatch_ctx) */
	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS) - PACKET3_SET_SH_REG_START; ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = HK_THREADS; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	{ u32 r1 = REG_SET_FIELD(REG_SET_FIELD(REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, HK_VGPRS), COMPUTE_PGM_RSRC1, SGPRS, HK_SGPRS), COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
	u32 r2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, HK_USER_SGPR);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = r1; ib.ptr[ib.length_dw++] = r2; }
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0xffffffff; ib.ptr[ib.length_dw++] = 0xffffffff;
	pgm_addr = (ib.gpu_addr + shader_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr); ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(mb_gpu_addr); ib.ptr[ib.length_dw++] = upper_32_bits(mb_gpu_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR, COMPUTE_SHADER_EN, 1);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &f);
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret) goto out_free_bo;

	ret = dma_fence_wait_timeout(f, false, msecs_to_jiffies(10000));
	dma_fence_put(f);
	if (ret <= 0) { ret = -ETIMEDOUT; goto out_free_bo; }

	{ uint32_t *mb = mb_cpu;
	pr_info("hk: run: magic=0x%x state=%u syscall=%u arg0=0x%x arg1=0x%x\n",
		mb[0], mb[1], mb[2], mb[3], mb[4]);
	mutex_lock(&hk_result_lock);
	scnprintf(hk_last_result, sizeof(hk_last_result),
		  "magic=0x%x state=%u syscall=%u arg0=0x%x arg1=0x%x",
		  mb[0], mb[1], mb[2], mb[3], mb[4]);
	mutex_unlock(&hk_result_lock); }
	ret = count;

out_free_bo:
	amdgpu_bo_free_kernel(&mb_bo, &mb_gpu_addr, &mb_cpu);
	return ret;
}
static struct kobj_attribute run_attr = __ATTR_WO(run);

/* /sys/kernel/heteroken/spawn — create a persistent GCN kthread */
static struct task_struct *hk_spawned_task;
static struct hk_gcn_ctx    hk_spawn_ctx;

static ssize_t spawn_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	if (hk_spawned_task) {
		pr_info("hk: GCN task already running, stop it first\n");
		return -EBUSY;
	}

	memset(&hk_spawn_ctx, 0, sizeof(hk_spawn_ctx));
	hk_spawn_ctx.adev = hk_get_adev();
	if (!hk_spawn_ctx.adev) return -ENODEV;

	if (hk_gcn_mailbox_alloc(&hk_spawn_ctx, 5))
		return -ENOMEM;

	hk_spawned_task = kthread_run(hk_gcn_thread, &hk_spawn_ctx,
				      "hk-gcn-0");
	if (IS_ERR(hk_spawned_task)) {
		pr_err("hk: kthread_run failed\n");
		hk_gcn_mailbox_free(&hk_spawn_ctx);
		return PTR_ERR(hk_spawned_task);
	}

	pr_info("hk: GCN task spawned: %s [%d]\n",
		hk_spawned_task->comm, task_pid_nr(hk_spawned_task));
	return count;
}
static struct kobj_attribute spawn_attr = __ATTR_WO(spawn);

/* /sys/kernel/heteroken/stop — stop the GCN kthread */
static ssize_t stop_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	if (!hk_spawned_task)
		return -ENOENT;

	pr_info("hk: stopping GCN task %s [%d]\n",
		hk_spawned_task->comm, task_pid_nr(hk_spawned_task));

	/* Wake the task if it's sleeping in schedule() */
	wake_up_process(hk_spawned_task);
	kthread_stop(hk_spawned_task);
	hk_spawned_task = NULL;
	hk_gcn_mailbox_free(&hk_spawn_ctx);

	pr_info("hk: GCN task stopped\n");
	return count;
}
static struct kobj_attribute stop_attr = __ATTR_WO(stop);

/* /sys/kernel/heteroken/result — read last dispatch output as string */
static ssize_t result_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t len;
	mutex_lock(&hk_result_lock);
	len = scnprintf(buf, PAGE_SIZE, "%s\n", hk_last_result);
	mutex_unlock(&hk_result_lock);
	return len;
}
static struct kobj_attribute result_attr = __ATTR_RO(result);

/* /sys/kernel/heteroken/hello — run hello_string shader, store output */
static ssize_t hello_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	struct amdgpu_device *adev = hk_get_adev();
	struct amdgpu_ring *kring;
	struct amdgpu_ib ib;
	struct dma_fence *f = NULL;
	struct amdgpu_bo *mb_bo = NULL;
	void *mb_cpu = NULL;
	u64 mb_gpu_addr, pgm_addr;
	u32 shader_off = 256, total_size = shader_off + 256;
	int ret;

	if (!adev) return -ENODEV;
	kring = &adev->gfx.compute_ring[0];
	if (!kring->sched.ready) return -ENODEV;

	ret = amdgpu_bo_create_kernel(adev, 64, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_GTT,
				      &mb_bo, &mb_gpu_addr, &mb_cpu);
	if (ret) return ret;
	memset(mb_cpu, 0, 64);

	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size,
			    AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret) goto out_free_bo;
	memset(ib.ptr, 0, total_size / 4);
	memcpy(ib.ptr + shader_off/4, hello_string, sizeof(hello_string));

	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS) - PACKET3_SET_SH_REG_START; ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = HK_THREADS; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	{ u32 r1 = REG_SET_FIELD(REG_SET_FIELD(REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, HK_VGPRS), COMPUTE_PGM_RSRC1, SGPRS, HK_SGPRS), COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
	u32 r2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, HK_USER_SGPR);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = r1; ib.ptr[ib.length_dw++] = r2; }
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0xffffffff; ib.ptr[ib.length_dw++] = 0xffffffff;
	pgm_addr = (ib.gpu_addr + shader_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr); ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(mb_gpu_addr); ib.ptr[ib.length_dw++] = upper_32_bits(mb_gpu_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR, COMPUTE_SHADER_EN, 1);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &f);
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret) goto out_free_bo;

	ret = dma_fence_wait_timeout(f, false, msecs_to_jiffies(10000));
	dma_fence_put(f);
	if (ret <= 0) { ret = -ETIMEDOUT; goto out_free_bo; }

	mutex_lock(&hk_result_lock);
	memcpy(hk_last_result, mb_cpu, 64);
	hk_last_result[63] = '\0';
	mutex_unlock(&hk_result_lock);
	pr_info("hk: hello: '%s'\n", hk_last_result);
	ret = count;

out_free_bo:
	amdgpu_bo_free_kernel(&mb_bo, &mb_gpu_addr, &mb_cpu);
	return ret;
}
static struct kobj_attribute hello_attr = __ATTR_WO(hello);

/* ==========================================================================
 * GPU-to-CPU address translation
 *
 * Under VMID 0, all GPU-accessible addresses are GTT BOs allocated by the
 * kernel.  We track each BO's GPU and CPU mappings so we can translate a
 * GPU virtual address back to a CPU kernel virtual address for syscalls
 * like read() that need the CPU to write into GPU-visible memory.
 * ========================================================================== */
struct hk_bo_map {
	u64   gpu_addr;
	void *cpu_addr;
	u32   size;
};

static void *hk_translate(u64 gpu_addr, struct hk_bo_map *maps, int n)
{
	for (int i = 0; i < n; i++) {
		if (maps[i].size && gpu_addr >= maps[i].gpu_addr &&
		    gpu_addr < maps[i].gpu_addr + maps[i].size)
			return (char *)maps[i].cpu_addr +
			       (gpu_addr - maps[i].gpu_addr);
	}
	return NULL;
}

/* ==========================================================================
 * Character device: /dev/heteroken
 *
 * ioctl interface for userspace host_runner:
 *   HK_IOCTL_RUN: copy shader from user, dispatch on GPU, return result
 *
 * This is the userspace API. The host_runner:
 *   1. fork() → child has dup'd mm (real task_struct, CoW)
 *   2. child: open("/dev/heteroken"), ioctl(HK_IOCTL_RUN, shader)
 *   3. kernel: dispatch shader on GPU, fence wait, copy result back
 *   4. child returns with result, parent waitpid()
 * ========================================================================== */
#define HK_IOCTL_RUN _IOWR('H', 1, struct hk_run_req)
#define HK_IOCTL_COMPUTE _IOWR('H', 2, struct hk_compute_req)

struct hk_run_req {
	__u64 shader_ptr;    /* user pointer to raw shader dwords */
	__u32 shader_size;   /* bytes */
	__u32 _pad;
	__u64 result_ptr;    /* user pointer for result output */
	__u32 result_size;   /* bytes available */
};

struct hk_compute_req {
	__u64 shader_ptr;     /* raw shader binary */
	__u32 shader_size;    /* bytes */
	__u32 vgprs;          /* VGPR field for RSRC1 (0=>1) */
	__u64 input_ptr;      /* user input data (NULL=none) */
	__u32 input_size;     /* input bytes (0=none) */
	__u32 dispatch_x;     /* grid X (0=>1) */
	__u64 output_ptr;     /* user output buffer (NULL=none) */
	__u32 output_size;    /* output bytes (0=none) */
	__u32 dispatch_y;     /* grid Y (0=>1) */
	__u64 mailbox_ptr;    /* user mailbox output (NULL=skip copy) */
	__u32 mailbox_size;   /* mailbox BO size (min 4096) */
	__u32 tgid_en;        /* 1=enable workgroup ID SGPRs */
};

static long hk_ioctl_compute(struct file *f, unsigned int cmd, unsigned long arg);

static long hk_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct amdgpu_device *adev = hk_get_adev();
	struct amdgpu_ring *kring;
	struct hk_run_req req;
	struct amdgpu_ib ib;
	struct dma_fence *_fence = NULL;
	struct amdgpu_bo *mb_bo = NULL;
	void *mb_cpu = NULL;
	u64 mb_gpu_addr, pgm_addr, tba_addr;
	u32 trap_off = 256, shader_off = 512, total_size;
	uint32_t *shader_copy = NULL;
	int ret;

	if (!adev) return -ENODEV;
	kring = &adev->gfx.compute_ring[0];
	if (!kring->sched.ready) return -ENODEV;

	if (cmd != HK_IOCTL_RUN && cmd != HK_IOCTL_COMPUTE) return -EINVAL;

	if (cmd == HK_IOCTL_COMPUTE)
		return hk_ioctl_compute(f, cmd, arg);

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.shader_size == 0 || req.shader_size > 4096)
		return -EINVAL;
	if (req.result_size == 0 || req.result_size > 4096)
		return -EINVAL;

	/* Copy shader binary from userspace */
	shader_copy = kmalloc(req.shader_size, GFP_KERNEL);
	if (!shader_copy) return -ENOMEM;
	if (copy_from_user(shader_copy, (void __user *)req.shader_ptr,
			   req.shader_size)) {
		ret = -EFAULT;
		goto out_free_shader;
	}
	pr_info("hk: ioctl: %u-byte shader from pid %d\n",
		req.shader_size, current->pid);

	/* Allocate mailbox BO for result */
	ret = amdgpu_bo_create_kernel(adev, req.result_size, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_GTT,
				      &mb_bo, &mb_gpu_addr, &mb_cpu);
	if (ret) goto out_free_shader;
	memset(mb_cpu, 0, req.result_size);

	/* Shader area size (used for all rounds) */
	total_size = shader_off + ALIGN(req.shader_size, 256);

	/* Single dispatch: live-wavefront syscall path uses s_sendmsg + polling. */
	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size,
				    AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret) goto out_free_bo;
	memset(ib.ptr, 0, total_size / 4);
	memcpy(ib.ptr + trap_off / 4, trap_return_shader,
	       sizeof(trap_return_shader));
	memcpy(ib.ptr + shader_off / 4, shader_copy, req.shader_size);

	/* Build PM4 */
	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS) - PACKET3_SET_SH_REG_START; ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = HK_THREADS; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	{ u32 r1 = REG_SET_FIELD(REG_SET_FIELD(REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, HK_VGPRS), COMPUTE_PGM_RSRC1, SGPRS, HK_SGPRS), COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
	u32 r2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, HK_USER_SGPR);
	r2 = REG_SET_FIELD(r2, COMPUTE_PGM_RSRC2, TRAP_PRESENT, 1);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = r1; ib.ptr[ib.length_dw++] = r2; }
	tba_addr = (ib.gpu_addr + trap_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 4);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmSQ_SHADER_TBA_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(tba_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(tba_addr);
	ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0xffffffff; ib.ptr[ib.length_dw++] = 0xffffffff;
	pgm_addr = (ib.gpu_addr + shader_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr); ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(mb_gpu_addr); ib.ptr[ib.length_dw++] = upper_32_bits(mb_gpu_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR, COMPUTE_SHADER_EN, 1);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &_fence);
	if (ret) {
		amdgpu_ib_free(adev, &ib, NULL);
		goto out_free_bo;
	}

	for (int poll = 0; poll < 10000; poll++) {
		/* Read mailbox state */
		u32 *mb32 = mb_cpu;
		u32 state = READ_ONCE(mb32[0]);
		u32 syscall_nr = READ_ONCE(mb32[2]);

		if (state || (poll % 1000) == 0)
			pr_info("hk: poll %d: state=%u syscall=%u fence=%d\n",
				poll, state, syscall_nr,
				dma_fence_is_signaled(_fence));

		if (state == 0) {
			if (dma_fence_is_signaled(_fence))
				break;
			msleep(1);
			continue;
		}

 		if (state == 1) {
			/* SYSCALL_PENDING */
			u64 arg0 = *(u64 *)&mb32[4];   /* +0x10: fd */
			u64 arg1 = *(u64 *)&mb32[6];   /* +0x18: buffer addr / arg1 */
			u64 arg2 = *(u64 *)&mb32[8];   /* +0x20: count */
			char *data = (char *)&mb32[16];
			s64 retval = 0;

			pr_info("hk: syscall %u(fd=%lld, buf=0x%llx, count=%lld)\n",
				syscall_nr, arg0, arg1, arg2);

			switch (syscall_nr) {
			case 0: { /* SYS_read — CPU reads file into GPU BO */
				struct fd f = fdget((int)arg0);
				if (!fd_empty(f)) {
					struct hk_bo_map maps[] = {
						{ mb_gpu_addr, mb_cpu, req.result_size },
					};
					void *buf = hk_translate(arg1, maps, 1);
					if (buf) {
						loff_t pos = 0;
						retval = kernel_read(fd_file(f),
								     buf,
								     (size_t)arg2,
								     &pos);
						if (retval > 0)
							drm_clflush_virt_range(buf, retval);
					} else
						retval = -EFAULT;
					fdput(f);
				} else
					retval = -EBADF;
				break;
			}
			case 1: { /* SYS_write */
				struct fd f = fdget((int)arg0);
				if (!fd_empty(f)) {
					char *wdata = data;
					if (arg1) {
						struct hk_bo_map wmaps[] = {
							{ mb_gpu_addr, mb_cpu, req.result_size },
						};
						wdata = hk_translate(arg1, wmaps, 1);
					}
					if (wdata) {
						loff_t pos = 0;
						retval = kernel_write(fd_file(f),
								      wdata,
								      (size_t)arg2, &pos);
					} else
						retval = -EFAULT;
					fdput(f);
				} else
					retval = -EBADF;
				break;
			}
			default:
				retval = -ENOSYS;
				break;
			}

			*(u64 *)&mb32[14] = retval;
			drm_clflush_virt_range(&mb32[14], sizeof(u64));
			smp_wmb();
			WRITE_ONCE(mb32[0], 0);
			drm_clflush_virt_range(&mb32[0], sizeof(u32));
			wmb();
			pr_info("hk: syscall returned %lld\n", retval);
			continue;
		}

		if (state == 3) {
			pr_info("hk: GCN task requested exit\n");
			if (dma_fence_is_signaled(_fence))
				break;
			msleep(1);
			continue;
		}

		break; /* unknown state */
	}

	ret = dma_fence_wait_timeout(_fence, false, msecs_to_jiffies(1000));
	dma_fence_put(_fence);
	_fence = NULL;
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret <= 0) { ret = -ETIMEDOUT; goto out_free_bo; }

	/* Copy result to userspace */
	if (copy_to_user((void __user *)req.result_ptr, mb_cpu,
			 min_t(u32, req.result_size, 4096))) {
		ret = -EFAULT;
		goto out_free_bo;
	}

	pr_info("hk: ioctl: done, result copied to user\n");
	ret = 0;

 out_free_bo:
	amdgpu_bo_free_kernel(&mb_bo, &mb_gpu_addr, &mb_cpu);
 out_free_shader:
	kfree(shader_copy);
	return ret;
}

/* ==========================================================================
 * HK_IOCTL_COMPUTE — extended ioctl with input/output BO support
 *
 * Allocates separate BOs for user-supplied input and output data,
 * writes their GPU addresses into the mailbox, and supports configurable
 * dispatch grid size and VGPR count for compute shaders.
 * ========================================================================== */

static long hk_ioctl_compute(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct amdgpu_device *adev = hk_get_adev();
	struct amdgpu_ring *kring;
	struct hk_compute_req req;
	struct amdgpu_ib ib;
	struct dma_fence *_fence = NULL;
	struct amdgpu_bo *mb_bo = NULL, *in_bo = NULL, *out_bo = NULL;
	void *mb_cpu = NULL, *in_cpu = NULL, *out_cpu = NULL;
	u64 mb_gpu_addr = 0, in_gpu_addr = 0, out_gpu_addr = 0;
	u64 pgm_addr, tba_addr;
	u32 trap_off = 256, shader_off = 512, total_size;
	u32 vgprs, dispatch_x, dispatch_y;
	uint32_t *shader_copy = NULL;
	int ret;

	if (!adev) return -ENODEV;
	kring = &adev->gfx.compute_ring[0];
	if (!kring->sched.ready) return -ENODEV;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.shader_size == 0 || req.shader_size > 4096)
		return -EINVAL;

	shader_copy = kmalloc(req.shader_size, GFP_KERNEL);
	if (!shader_copy) return -ENOMEM;
	if (copy_from_user(shader_copy, (void __user *)req.shader_ptr,
			   req.shader_size)) {
		ret = -EFAULT;
		goto out_free_shader;
	}

	/* Allocate mailbox BO (always 4096 for full layout) */
	ret = amdgpu_bo_create_kernel(adev, max_t(u32, req.mailbox_size, 4096),
				      PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				      &mb_bo, &mb_gpu_addr, &mb_cpu);
	if (ret) goto out_free_shader;
	memset(mb_cpu, 0, max_t(u32, req.mailbox_size, 4096));

	/* Copy initial mailbox data from user (width, height, etc.) */
	if (req.mailbox_ptr && req.mailbox_size > 0) {
		if (copy_from_user(mb_cpu, (void __user *)req.mailbox_ptr,
				   min_t(u32, req.mailbox_size, 4096))) {
			ret = -EFAULT;
			goto out_free_mb;
		}
	}

	/* Allocate input BO and copy user data */
	if (req.input_size > 0 && req.input_ptr) {
		ret = amdgpu_bo_create_kernel(adev, req.input_size, PAGE_SIZE,
					      AMDGPU_GEM_DOMAIN_GTT,
					      &in_bo, &in_gpu_addr, &in_cpu);
		if (ret) goto out_free_mb;
		if (copy_from_user(in_cpu, (void __user *)req.input_ptr,
				   req.input_size)) {
			ret = -EFAULT;
			goto out_free_in;
		}
		*(u64 *)((char *)mb_cpu + HK_MB_INPUT_ADDR) = in_gpu_addr;
		*(u32 *)((char *)mb_cpu + HK_MB_INPUT_SIZE) = req.input_size;
		drm_clflush_virt_range(in_cpu, req.input_size);
		wmb();
	}

	/* Allocate output BO (zeroed) */
	if (req.output_size > 0 && req.output_ptr) {
		ret = amdgpu_bo_create_kernel(adev, req.output_size, PAGE_SIZE,
					      AMDGPU_GEM_DOMAIN_GTT,
					      &out_bo, &out_gpu_addr, &out_cpu);
		if (ret) goto out_free_in;
		memset(out_cpu, 0, req.output_size);
		*(u64 *)((char *)mb_cpu + HK_MB_OUTPUT_ADDR) = out_gpu_addr;
		*(u32 *)((char *)mb_cpu + HK_MB_OUTPUT_SIZE) = req.output_size;
	}

	pr_info("hk: compute: shader=%u in=%u out=%u grid=%ux%u\n",
		req.shader_size, req.input_size, req.output_size,
		req.dispatch_x ? req.dispatch_x : 1,
		req.dispatch_y ? req.dispatch_y : 1);

	/* Flush mailbox so GPU sees input/output addresses */
	drm_clflush_virt_range(mb_cpu, max_t(u32, req.mailbox_size, 4096));
	wmb();

	vgprs = req.vgprs ? req.vgprs : HK_VGPRS;
	dispatch_x = req.dispatch_x ? req.dispatch_x : 1;
	dispatch_y = req.dispatch_y ? req.dispatch_y : 1;

	/* Build IB */
	total_size = shader_off + ALIGN(req.shader_size, 256);
	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size, AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret) goto out_free_out;
	memset(ib.ptr, 0, total_size / 4);
	memcpy(ib.ptr + trap_off / 4, trap_return_shader,
	       sizeof(trap_return_shader));
	memcpy(ib.ptr + shader_off / 4, shader_copy, req.shader_size);

	/* Build PM4 */
	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = HK_THREADS; ib.ptr[ib.length_dw++] = 1; ib.ptr[ib.length_dw++] = 1;
	{
		u32 r1 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, vgprs);
		r1 = REG_SET_FIELD(r1, COMPUTE_PGM_RSRC1, SGPRS, HK_SGPRS);
		r1 = REG_SET_FIELD(r1, COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
		u32 r2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, HK_USER_SGPR);
		r2 = REG_SET_FIELD(r2, COMPUTE_PGM_RSRC2, TRAP_PRESENT, 1);
		if (req.tgid_en)
			r2 = REG_SET_FIELD(r2, COMPUTE_PGM_RSRC2, TGID_X_EN, 1);
		ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
		ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1) - PACKET3_SET_SH_REG_START;
		ib.ptr[ib.length_dw++] = r1; ib.ptr[ib.length_dw++] = r2;
	}
	tba_addr = (ib.gpu_addr + trap_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 4);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmSQ_SHADER_TBA_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(tba_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(tba_addr);
	ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = 0;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0xffffffff; ib.ptr[ib.length_dw++] = 0xffffffff;
	pgm_addr = (ib.gpu_addr + shader_off) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr); ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0) - PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(mb_gpu_addr); ib.ptr[ib.length_dw++] = upper_32_bits(mb_gpu_addr);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = dispatch_x; ib.ptr[ib.length_dw++] = dispatch_y; ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR, COMPUTE_SHADER_EN, 1);
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &_fence);
	if (ret) {
		amdgpu_ib_free(adev, &ib, NULL);
		goto out_free_out;
	}

	/* Poll loop (same as HK_IOCTL_RUN) */
	for (int poll = 0; poll < 10000; poll++) {
		u32 *mb32 = mb_cpu;
		u32 state = READ_ONCE(mb32[0]);
		u32 syscall_nr = READ_ONCE(mb32[2]);

		if (state || (poll % 1000) == 0)
			pr_info("hk: compute poll %d: state=%u syscall=%u fence=%d\n",
				poll, state, syscall_nr,
				dma_fence_is_signaled(_fence));

		if (state == 0) {
			if (dma_fence_is_signaled(_fence))
				break;
			msleep(1);
			continue;
		}

 		if (state == 1) {
			u64 arg0 = *(u64 *)&mb32[4];   /* +0x10: fd */
			u64 arg1 = *(u64 *)&mb32[6];   /* +0x18: buffer addr */
			u64 arg2 = *(u64 *)&mb32[8];   /* +0x20: count */
			char *data = (char *)&mb32[16];
			s64 retval = 0;

			pr_info("hk: compute syscall %u(fd=%lld, buf=0x%llx, count=%lld)\n",
				syscall_nr, arg0, arg1, arg2);

			switch (syscall_nr) {
			case 0: { /* SYS_read — CPU reads into GPU BO */
				struct fd fdfd = fdget((int)arg0);
				if (!fd_empty(fdfd)) {
					struct hk_bo_map maps[3];
					int nmaps = 0;
					if (out_bo) {
						maps[nmaps].gpu_addr = out_gpu_addr;
						maps[nmaps].cpu_addr = out_cpu;
						maps[nmaps].size = req.output_size;
						nmaps++;
					}
					if (in_bo) {
						maps[nmaps].gpu_addr = in_gpu_addr;
						maps[nmaps].cpu_addr = in_cpu;
						maps[nmaps].size = req.input_size;
						nmaps++;
					}
					maps[nmaps].gpu_addr = mb_gpu_addr;
					maps[nmaps].cpu_addr = mb_cpu;
					maps[nmaps].size = 4096;
					nmaps++;
					{
						void *buf = hk_translate(arg1, maps, nmaps);
						if (buf) {
							loff_t pos = 0;
							retval = kernel_read(fd_file(fdfd), buf,
									     (size_t)arg2, &pos);
							if (retval > 0)
								drm_clflush_virt_range(buf, retval);
						} else
							retval = -EFAULT;
					}
					fdput(fdfd);
				} else
					retval = -EBADF;
				break;
			}
			case 1: {
				struct fd fdfd = fdget((int)arg0);
				if (!fd_empty(fdfd)) {
					char *wdata = data;
					if (arg1) {
						struct hk_bo_map wmaps[3];
						int wn = 0;
						if (out_bo) { wmaps[wn].gpu_addr = out_gpu_addr; wmaps[wn].cpu_addr = out_cpu; wmaps[wn].size = req.output_size; wn++; }
						if (in_bo)  { wmaps[wn].gpu_addr = in_gpu_addr;  wmaps[wn].cpu_addr = in_cpu;  wmaps[wn].size = req.input_size;  wn++; }
						wmaps[wn].gpu_addr = mb_gpu_addr; wmaps[wn].cpu_addr = mb_cpu; wmaps[wn].size = 4096; wn++;
						wdata = hk_translate(arg1, wmaps, wn);
					}
					if (wdata) {
						loff_t pos = 0;
						retval = kernel_write(fd_file(fdfd), wdata,
								      (size_t)arg2, &pos);
					} else
						retval = -EFAULT;
					fdput(fdfd);
				} else
					retval = -EBADF;
				break;
			}
			default:
				retval = -ENOSYS;
				break;
			}

			*(u64 *)&mb32[14] = retval;
			drm_clflush_virt_range(&mb32[14], sizeof(u64));
			smp_wmb();
			WRITE_ONCE(mb32[0], 0);
			drm_clflush_virt_range(&mb32[0], sizeof(u32));
			wmb();
			pr_info("hk: compute syscall returned %lld\n", retval);
			continue;
		}

		if (state == 3) {
			if (dma_fence_is_signaled(_fence))
				break;
			msleep(1);
			continue;
		}
		break;
	}

	ret = dma_fence_wait_timeout(_fence, false, msecs_to_jiffies(1000));
	dma_fence_put(_fence);
	_fence = NULL;
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret <= 0) { ret = -ETIMEDOUT; goto out_free_out; }

	pr_info("hk: compute: GPU done, copying results\n");

	/* Copy output BO to user */
	if (out_cpu && req.output_ptr) {
		drm_clflush_virt_range(out_cpu, req.output_size);
		if (copy_to_user((void __user *)req.output_ptr, out_cpu,
				 req.output_size)) {
			ret = -EFAULT;
			goto out_free_out;
		}
	}

	/* Copy mailbox to user */
	if (req.mailbox_ptr && req.mailbox_size > 0) {
		if (copy_to_user((void __user *)req.mailbox_ptr, mb_cpu,
				 min_t(u32, req.mailbox_size, 4096))) {
			ret = -EFAULT;
			goto out_free_out;
		}
	}

	pr_info("hk: compute: done\n");
	ret = 0;

 out_free_out:
	amdgpu_bo_free_kernel(&out_bo, &out_gpu_addr, &out_cpu);
 out_free_in:
	amdgpu_bo_free_kernel(&in_bo, &in_gpu_addr, &in_cpu);
 out_free_mb:
	amdgpu_bo_free_kernel(&mb_bo, &mb_gpu_addr, &mb_cpu);
 out_free_shader:
	kfree(shader_copy);
	return ret;
}

static const struct file_operations hk_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = hk_ioctl,
	.compat_ioctl   = hk_ioctl,
};

static struct miscdevice hk_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "heteroken",
	.fops  = &hk_fops,
};

/* ==========================================================================
 * Module init/exit
 * ========================================================================== */
static struct kobject *hk_kobj;

static int __init hk_init(void)
{
	int ret;
	pr_info("hk: HeteroKern ready (gfx902, phases 1-3)\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj) return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))    pr_warn("hk: run failed\n");
	if (sysfs_create_file(hk_kobj, &spawn_attr.attr))  pr_warn("hk: spawn failed\n");
	if (sysfs_create_file(hk_kobj, &stop_attr.attr))   pr_warn("hk: stop failed\n");
	if (sysfs_create_file(hk_kobj, &hello_attr.attr))  pr_warn("hk: hello failed\n");
	if (sysfs_create_file(hk_kobj, &result_attr.attr)) pr_warn("hk: result failed\n");
	ret = misc_register(&hk_miscdev);
	if (ret) pr_warn("hk: misc_register failed: %d\n", ret);
	else pr_info("hk: /dev/heteroken registered\n");
	return 0;
}

static void __exit hk_exit(void)
{
	misc_deregister(&hk_miscdev);
	if (hk_spawned_task) {
		wake_up_process(hk_spawned_task);
		kthread_stop(hk_spawned_task);
		hk_gcn_mailbox_free(&hk_spawn_ctx);
	}
	if (hk_kobj) kobject_put(hk_kobj);
	pr_info("hk: unloaded\n");
}

module_init(hk_init);
module_exit(hk_exit);
