/*
 * HeteroKern — GCN compute dispatch on GFX9 (Raven Ridge)
 *
 * ==========================================================================
 * DISPATCH LAYER (swappable)
 *
 *   Current: kernel compute ring + IB  (PACKET3, VMID 0)
 *   Future:  KFD PM4 queue             (PACKET3_COMPUTE, VMID 8)
 *
 *   The dispatch function hk_dispatch() takes an adev + shader binary + size
 *   and returns the uint32_t written by the shader. The caller doesn't care
 *   about the underlying mechanism (IB vs KFD queue).
 *
 *   Swapping the back-end only needs to replace hk_dispatch() — the call
 *   signature stays the same: (adev, shader_dwords, code_ptr, code_dwords) → result.
 *
 * ==========================================================================
 * DEVICE DISCOVERY (also swappable)
 *
 *   Current: open /dev/kfd → find PDD → get adev
 *   Future:  amdgpu device iteration or direct pointer from clone3 hook
 *
 * ==========================================================================
 * PM4 packet reference:
 *   PACKET3(op, n):  op at bits[15:8], n = data_words - 1
 *   SET_SH_REG offset = SOC15_REG_OFFSET(GC,0,mmXXX) - PACKET3_SET_SH_REG_START (in bytes)
 *   COMPUTE_PGM_LO = (shader GPU VA) >> 8  (256-byte aligned)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sysfs.h>
#include <linux/completion.h>
#include <linux/dma-fence.h>

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

/* --------------------------------------------------------------------------
 * gfx902 mailbox shader — 26 dwords (104 bytes)
 *
 * HSA ABI (USER_SGPR=2): s[0:1] = COMPUTE_USER_DATA_0/1 = mailbox GPU VA
 *
 * Writes 5 dwords sequentially to the mailbox:
 *   [+0]  magic    = 0x21544148
 *   [+4]  state    = 1 (syscall_pending)
 *   [+8]  syscall  = 42
 *   [+12] arg0     = 0xDEADBEEF
 *   [+16] arg1     = 0xCAFEBABE
 *
 * Built on dev machine (192.168.1.8):
 *   clang --target=amdgcn-amd-amdhsa -mcpu=gfx902 -c hello_mailbox.S
 *   ld.lld -shared hello_mailbox.o -o hello_mailbox.hsaco
 *   Verified: llvm-objdump --disassemble --mcpu=gfx902
 */
static const uint32_t hello_mailbox[] = {
	0xBF8CC07F, /* s_waitcnt lgkmcnt(0) */
	0x7E000200, /* v_mov_b32_e32 v0, s0 */
	0x7E020201, /* v_mov_b32_e32 v1, s1 */
	0x7E0402FF, 0x21544148, /* v_mov_b32_e32 v2, 0x21544148 */
	0xDC700000, 0x00000200, /* flat_store_dword v[0:1], v2 — magic */
	0x68000084, /* v_add_u32_e32 v0, 4, v0 */
	0x7E040281, /* v_mov_b32_e32 v2, 1 */
	0xDC700000, 0x00000200, /* flat_store — state */
	0x68000084, /* v_add_u32_e32 v0, 4, v0 */
	0x7E0402AA, /* v_mov_b32_e32 v2, 42 */
	0xDC700000, 0x00000200, /* flat_store — syscall_nr */
	0x68000084, /* v_add_u32_e32 v0, 4, v0 */
	0x7E0402FF, 0xDEADBEEF, /* v_mov_b32_e32 v2, 0xDEADBEEF */
	0xDC700000, 0x00000200, /* flat_store — arg0 */
	0x68000084, /* v_add_u32_e32 v0, 4, v0 */
	0x7E0402FF, 0xCAFEBABE, /* v_mov_b32_e32 v2, 0xCAFEBABE */
	0xDC700000, 0x00000200, /* flat_store — arg1 */
	0xBF810000, /* s_endpgm */
};
#define MAILBOX_SHADER_DW ARRAY_SIZE(hello_mailbox)
#define MAGIC 0x21544148

/* --------------------------------------------------------------------------
 * Device discovery: get amdgpu_device pointer
 *
 * Current implementation: open /dev/kfd, walk process PDDs, extract adev.
 * This is the minimal path that doesn't require DRM render node or VM init.
 */
static struct amdgpu_device *hk_get_adev(void)
{
	struct kfd_process *proc;
	struct file *f;

	f = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(f)) {
		pr_err("hk: /dev/kfd open failed: %ld\n", PTR_ERR(f));
		return NULL;
	}
	proc = f->private_data;
	for (int i = 0; i < proc->n_pdds; i++) {
		if (proc->pdds[i] && proc->pdds[i]->dev &&
		    proc->pdds[i]->dev->adev)
			return proc->pdds[i]->dev->adev;
	}
	pr_err("hk: no PDD with adev found\n");
	return NULL;
}

/* --------------------------------------------------------------------------
 * Mailbox layout (simplified for Phase 2 test):
 *
 *   offset 0:  magic    (0x21544148 = completion signal)
 *   offset 4:  state    (1 = syscall_pending)
 *   offset 8:  syscall  (syscall number)
 *   offset 12: arg0     (first argument)
 *   offset 16: arg1     (second argument)
 */
struct hk_mailbox {
	uint32_t magic;
	uint32_t state;
	uint32_t syscall_nr;
	uint32_t arg0;
	uint32_t arg1;
};

/* Compute register defaults for minimal single-wavefront dispatch */
#define HK_VGPRS     0   /* (0+1)*4 = 4 VGPR */
#define HK_SGPRS     1   /* (1+1)*8 = 16 SGPR */
#define HK_USER_SGPR 2   /* s[0:1] = USER_DATA_0/1 */
#define HK_THREADS   64  /* threads per workgroup */

/* --------------------------------------------------------------------------
 * Interrupt-driven completion via dma_fence callback.
 *
 * Instead of polling dma_fence_wait_timeout(), we register a callback
 * that fires when the IB completes. Under the hood, amdgpu signals the
 * fence from the IH (Interrupt Handler) interrupt context — so this
 * callback proves the GPU→CPU interrupt path works.
 */
struct hk_fence_cb {
	struct dma_fence_cb base;
	struct completion   done;
	uint32_t           *mailbox;   /* CPU pointer to mailbox */
	uint32_t            dwords;
};

static void hk_fence_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct hk_fence_cb *hcb = container_of(cb, struct hk_fence_cb, base);

	pr_info("hk: *** IH interrupt fired! callback in %s context ***\n",
		in_task() ? "task" : in_irq() ? "irq" : in_softirq() ? "softirq" : "unknown");

	for (u32 i = 0; i < hcb->dwords; i++)
		pr_info("hk:   mailbox[%u] = 0x%08x\n", i, hcb->mailbox[i]);

	dma_fence_put(f);
	complete(&hcb->done);
}

/* --------------------------------------------------------------------------
 * Dispatch: run shader on kernel compute ring via IB.
 *
 * Uses dma_fence_add_callback for interrupt-driven completion.
 *
 * The mailbox buffer (result_dwords * 4 bytes) is allocated as a kernel BO
 * so it survives beyond IB lifetime. Its GPU address is passed to the shader
 * via COMPUTE_USER_DATA_0/1.
 */
static int hk_dispatch(struct amdgpu_device *adev,
		       const uint32_t *shader, uint32_t shader_dwords,
		       uint32_t *result, uint32_t result_dwords)
{
	struct amdgpu_ring *kring = &adev->gfx.compute_ring[0];
	struct amdgpu_ib ib;
	struct dma_fence *f = NULL;
	struct amdgpu_bo *mb_bo = NULL;
	void *mb_cpu = NULL;
	u64 mb_gpu_addr;
	u32 total_size, shader_offset;
	u64 pgm_addr;
	struct hk_fence_cb hcb;
	int ret;

	if (!kring->sched.ready) {
		pr_err("hk: compute ring not ready\n");
		return -ENODEV;
	}

	/* Allocate persistent mailbox BO (survives IB free) */
	ret = amdgpu_bo_create_kernel(adev, result_dwords * 4, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_GTT,
				      &mb_bo, &mb_gpu_addr, &mb_cpu);
	if (ret) {
		pr_err("hk: bo_create_kernel: %d\n", ret);
		return ret;
	}
	memset(mb_cpu, 0, result_dwords * 4);

	/* Lay out IB: commands (256B) + shader (256B) */
	shader_offset = 256;
	total_size    = shader_offset + 256;

	memset(&ib, 0, sizeof(ib));
	ret = amdgpu_ib_get(adev, NULL, total_size,
			    AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret) {
		pr_err("hk: ib_get: %d\n", ret);
		goto out_free_bo;
	}

	memset(ib.ptr, 0, total_size / 4);
	for (u32 i = 0; i < shader_dwords; i++)
		ib.ptr[i + (shader_offset / 4)] = shader[i];

	/* --- Build PM4 command stream --- */
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
		u32 rsrc2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR,
					  HK_USER_SGPR);
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

	pgm_addr = (ib.gpu_addr + (u64)shader_offset) >> 8;
	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = lower_32_bits(mb_gpu_addr);
	ib.ptr[ib.length_dw++] = upper_32_bits(mb_gpu_addr);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR,
						COMPUTE_SHADER_EN, 1);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	pr_info("hk: IB %u dwords  gpu=0x%llx  shader=0x%llx  mailbox=0x%llx\n",
		ib.length_dw, ib.gpu_addr,
		ib.gpu_addr + shader_offset, mb_gpu_addr);

	/* Submit IB */
	ret = amdgpu_ib_schedule(kring, 1, &ib, NULL, &f);
	amdgpu_ib_free(adev, &ib, NULL);
	if (ret) {
		pr_err("hk: ib_schedule: %d\n", ret);
		goto out_free_bo;
	}

	/* Register interrupt-driven callback */
	init_completion(&hcb.done);
	hcb.mailbox = mb_cpu;
	hcb.dwords  = result_dwords;

	ret = dma_fence_add_callback(f, &hcb.base, hk_fence_callback);
	if (ret == -ENOENT) {
		/* Fence already signaled — read immediately */
		pr_info("hk: fence already signaled, reading directly\n");
		hk_fence_callback(f, &hcb.base);
	} else if (ret) {
		pr_err("hk: fence_add_callback: %d\n", ret);
		dma_fence_put(f);
		goto out_free_bo;
	} else {
		/* Wait for the IH interrupt to fire the callback */
		pr_info("hk: waiting for IH interrupt...\n");
		wait_for_completion(&hcb.done);
	}

	/* Copy mailbox to caller's result buffer */
	memcpy(result, mb_cpu, result_dwords * sizeof(uint32_t));

	ret = 0;

out_free_bo:
	amdgpu_bo_free_kernel(&mb_bo, &mb_gpu_addr, &mb_cpu);
	return ret;
}

/* ==========================================================================
 * sysfs trigger: /sys/kernel/heteroken/run
 */
static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	struct amdgpu_device *adev;
	uint32_t result;
	int ret;

	adev = hk_get_adev();
	if (!adev)
		return -ENODEV;

	pr_info("hk: dispatching mailbox shader (%u dwords)\n",
		MAILBOX_SHADER_DW);
	{
		uint32_t mb[5];
		int ret = hk_dispatch(adev, hello_mailbox, MAILBOX_SHADER_DW, mb, 5);
		if (ret) {
			pr_err("hk: dispatch failed: %d\n", ret);
			return count;
		}
		pr_info("hk: mailbox: magic=0x%08x state=%u syscall=%u arg0=0x%x arg1=0x%x\n",
			mb[0], mb[1], mb[2], mb[3], mb[4]);
	}

	return count;
}

static struct kobj_attribute run_attr = __ATTR_WO(run);
static struct kobject *hk_kobj;

static int __init hk_init(void)
{
	pr_info("hk: HeteroKern GCN compute dispatch ready (gfx902)\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj)
		return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))
		pr_warn("hk: sysfs create failed\n");
	return 0;
}

static void __exit hk_exit(void)
{
	if (hk_kobj)
		kobject_put(hk_kobj);
	pr_info("hk: unloaded\n");
}

module_init(hk_init);
module_exit(hk_exit);
