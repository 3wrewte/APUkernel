/*
 * HeteroKern — PM4 compute dispatch on GFX9 (Raven Ridge)
 *
 * Uses PM4 SET_SH_REG to program compute registers and DISPATCH_DIRECT
 * to launch a GCN kernel that writes magic 0x21544148 ("HAT!") to a
 * result buffer. Uses hack WPTR (direct WPTR register write) for dispatch.
 *
 * PM4 Type 3 packet header: PACKET3_COMPUTE(op, n)
 *   n = (number_of_data_words - 1)
 *   For SET_SH_REG: data_words = 1 (reg_offset) + num_register_values
 *   So for 2 registers: n = (1 + 2) - 1 = 2
 *   For 3 registers: n = (1 + 3) - 1 = 3
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_topology.h"
#include "../amdgpu/amdgpu_amdkfd.h"
#include "../include/v9_structs.h"
#include "../amdgpu/amdgpu_amdkfd_gfx_v9.h"
#include "gc/gc_9_0_offset.h"
#include "gc/gc_9_0_sh_mask.h"
#include "../amdgpu/soc15_common.h"
#include "../amdgpu/soc15d.h"
#include "../amdgpu/amdgpu.h"
#include "../amdgpu/amdgpu_ring.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern PM4 compute dispatch on GFX9");

#define HK_GPUVA_RING    0x1000000ULL
#define HK_GPUVA_WPTR    0x2000000ULL
#define HK_GPUVA_RPTR    0x2001000ULL
#define HK_GPUVA_CTX     0x2002000ULL
#define HK_GPUVA_CODE    0x2400000ULL
#define HK_GPUVA_KERNARG 0x2500000ULL
#define HK_GPUVA_RESULT  0x2600000ULL
#define RING_BYTES  (64ULL * 1024)
#define PAGE        4096ULL

static struct kfd_process     *hk_proc;
static struct kgd_mem         *code_mem, *result_mem;

static uint32_t hello_gcn_gfx9[] = {
	0xbf8cc07f, /* s_waitcnt lgkmcnt(0) */
	0x7e000200, /* v_mov_b32_e32 v0, s0 */
	0x7e020201, /* v_mov_b32_e32 v1, s1 */
	0x7e0402ff, /* v_mov_b32_e32 v2, 0x21544148 */
	0x21544148, /* immediate: "HAT!" */
	0xdc700000, /* flat_store_dword v[0:1], v2 */
	0x00000200, /* flat_store offset=0 */
	0xbf810000, /* s_endpgm */
};

static int hk_run(void)
{
	struct kfd_process_device *pdd;
	struct amdgpu_device *adev;
	struct file *kfd_f;
	int ret;
	struct amdgpu_ring *kring;
	struct amdgpu_ib ib;
	struct dma_fence *f = NULL;
	uint32_t total_size, shader_offset, result_offset;
	int r2;

	kfd_f = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(kfd_f)) { pr_err("hk: /dev/kfd: %ld\n", PTR_ERR(kfd_f)); return PTR_ERR(kfd_f); }
	hk_proc = kfd_f->private_data;

	pdd = NULL;
	for (int i = 0; i < hk_proc->n_pdds; i++) {
		if (hk_proc->pdds[i] && hk_proc->pdds[i]->dev && hk_proc->pdds[i]->dev->adev) {
			pdd = hk_proc->pdds[i];
			break;
		}
	}
	if (!pdd) { pr_err("hk: PDD not found\n"); return -EINVAL; }

	adev = pdd->dev->adev;
	pr_info("hk: adev found  pasid=%d\n", hk_proc->pasid);

	kring = &adev->gfx.compute_ring[0];
	if (!kring->sched.ready) {
		pr_err("hk: compute ring not ready\n");
		return -ENODEV;
	}
	pr_info("hk: compute ring ready\n");

	total_size = 256;
	shader_offset = total_size;
	total_size += 256;
	result_offset = total_size;
	total_size += 256;

	memset(&ib, 0, sizeof(ib));
	r2 = amdgpu_ib_get(adev, NULL, total_size,
			   AMDGPU_IB_POOL_DIRECT, &ib);
	if (r2) { pr_err("hk: ib_get: %d\n", r2); return r2; }

	memset(ib.ptr, 0, total_size / 4);
	for (int i = 0; i < sizeof(hello_gcn_gfx9)/sizeof(uint32_t); i++)
		ib.ptr[i + (shader_offset / 4)] = hello_gcn_gfx9[i];

	ib.length_dw = 0;

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 1);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 0;

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 3);
	ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X)
				- PACKET3_SET_SH_REG_START;
	ib.ptr[ib.length_dw++] = 64;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;

	{
		uint32_t rsrc1 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC1, VGPRS, 0);
		rsrc1 = REG_SET_FIELD(rsrc1, COMPUTE_PGM_RSRC1, SGPRS, 1);
		rsrc1 = REG_SET_FIELD(rsrc1, COMPUTE_PGM_RSRC1, DX10_CLAMP, 1);
		uint32_t rsrc2 = REG_SET_FIELD(0, COMPUTE_PGM_RSRC2, USER_SGPR, 2);
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

	{
		u64 pgm_addr = (ib.gpu_addr + (u64)shader_offset) >> 8;
		ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
		ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO)
					- PACKET3_SET_SH_REG_START;
		ib.ptr[ib.length_dw++] = lower_32_bits(pgm_addr);
		ib.ptr[ib.length_dw++] = upper_32_bits(pgm_addr);
	}

	{
		u64 result_addr = ib.gpu_addr + (u64)result_offset;
		ib.ptr[ib.length_dw++] = PACKET3(PACKET3_SET_SH_REG, 2);
		ib.ptr[ib.length_dw++] = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0)
					- PACKET3_SET_SH_REG_START;
		ib.ptr[ib.length_dw++] = lower_32_bits(result_addr);
		ib.ptr[ib.length_dw++] = upper_32_bits(result_addr);
	}

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = 1;
	ib.ptr[ib.length_dw++] = REG_SET_FIELD(0, COMPUTE_DISPATCH_INITIATOR,
						COMPUTE_SHADER_EN, 1);

	ib.ptr[ib.length_dw++] = PACKET3(PACKET3_EVENT_WRITE, 0);
	ib.ptr[ib.length_dw++] = EVENT_TYPE(7) | EVENT_INDEX(4);

	pr_info("hk: IB built %u dwords, ib.gpu_addr=0x%llx shader=0x%llx result=0x%llx\n",
		ib.length_dw, ib.gpu_addr,
		ib.gpu_addr + shader_offset,
		ib.gpu_addr + result_offset);

	r2 = amdgpu_ib_schedule(kring, 1, &ib, NULL, &f);
	if (r2) {
		pr_err("hk: ib_schedule: %d\n", r2);
		amdgpu_ib_free(adev, &ib, NULL);
		return r2;
	}

	r2 = dma_fence_wait_timeout(f, false, msecs_to_jiffies(10000));
	dma_fence_put(f);
	if (r2 <= 0) {
		pr_err("hk: fence_wait: %d (timeout or error)\n", r2);
		amdgpu_ib_free(adev, &ib, NULL);
		return -ETIMEDOUT;
	}
	pr_info("hk: IB completed\n");

	{
		uint32_t val = ib.ptr[result_offset / 4];
		pr_info("hk: result=0x%08x %s\n", val,
			val == 0x21544148 ?
			">>> MAGIC MATCH! <<<" : "(no magic)");
	}

	amdgpu_ib_free(adev, &ib, NULL);
	return 0;
}

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
	pr_info("hk: HeteroKern PM4 compute dispatch test ready\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj) return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))
		pr_warn("hk: sysfs failed\n");
	return 0;
}

static void __exit hk_exit(void)
{
	if (hk_kobj) kobject_put(hk_kobj);
	pr_info("hk: unloaded\n");
}

module_init(hk_init);
module_exit(hk_exit);
