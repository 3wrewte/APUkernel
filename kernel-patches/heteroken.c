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
#define MAGIC 0x21544148

/* Compute register defaults */
#define HK_VGPRS     0
#define HK_SGPRS     1
#define HK_USER_SGPR 2
#define HK_THREADS   64

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
 * Module init/exit
 * ========================================================================== */
static struct kobject *hk_kobj;

static int __init hk_init(void)
{
	pr_info("hk: HeteroKern ready (gfx902, phases 1-3)\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj) return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))    pr_warn("hk: run failed\n");
	if (sysfs_create_file(hk_kobj, &spawn_attr.attr))  pr_warn("hk: spawn failed\n");
	if (sysfs_create_file(hk_kobj, &stop_attr.attr))   pr_warn("hk: stop failed\n");
	if (sysfs_create_file(hk_kobj, &hello_attr.attr))  pr_warn("hk: hello failed\n");
	if (sysfs_create_file(hk_kobj, &result_attr.attr)) pr_warn("hk: result failed\n");
	return 0;
}

static void __exit hk_exit(void)
{
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
