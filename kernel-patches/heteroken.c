/*
 * HeteroKern — CP ring test with kfd_queue_acquire_buffers
 *
 * The key insight: pqm_create_queue does NOT call kfd_queue_acquire_buffers.
 * That's only done in the chardev ioctl path. Without it, the HQD is loaded
 * with addresses that the CP can't access. We must call it ourselves.
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

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern CP ring test with buffer acquisition");

#define HK_GPUVA_RING    0x1000000ULL
#define HK_GPUVA_WPTR    0x2000000ULL
#define HK_GPUVA_RPTR    0x2001000ULL
#define HK_GPUVA_CTX     0x2002000ULL
#define RING_BYTES  (64ULL * 1024)
#define PAGE        4096ULL

static struct kfd_process     *hk_proc;
static struct kgd_mem         *ring_mem, *wptr_mem, *rptr_mem, *ctx_mem;
static unsigned int            hk_qid;
static bool                    hk_ready;

static int hk_run(void)
{
	struct kfd_process_device *pdd;
	struct amdgpu_device *adev;
	void *drm_priv;
	struct file *kfd_f, *drm_f;
	uint32_t db_off;
	uint64_t mmap_off;
	uint32_t fl = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
		      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
	int ret;

	kfd_f = filp_open("/dev/kfd", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(kfd_f)) { pr_err("hk: /dev/kfd: %ld\n", PTR_ERR(kfd_f)); return PTR_ERR(kfd_f); }
	hk_proc = kfd_f->private_data;

	drm_f = filp_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(drm_f)) { pr_err("hk: renderD128: %ld\n", PTR_ERR(drm_f)); return PTR_ERR(drm_f); }

	pdd = NULL;
	for (int i = 0; i < hk_proc->n_pdds; i++) {
		if (hk_proc->pdds[i] && hk_proc->pdds[i]->dev && hk_proc->pdds[i]->dev->adev) {
			pdd = hk_proc->pdds[i];
			break;
		}
	}
	if (!pdd) { pr_err("hk: PDD not found\n"); return -EINVAL; }

	adev = pdd->dev->adev;
	ret = kfd_process_device_init_vm(pdd, drm_f);
	if (ret) { pr_err("hk: init_vm: %d\n", ret); return ret; }
	drm_priv = pdd->drm_priv;

	pdd = kfd_bind_process_to_device(pdd->dev, hk_proc);
	if (IS_ERR(pdd)) { pr_err("hk: bind: %ld\n", PTR_ERR(pdd)); return PTR_ERR(pdd); }
	pr_info("hk: process ready  pasid=%d\n", hk_proc->pasid);
	pr_info("hk: pdd lds_base=0x%llx scratch_base=0x%llx gpuvm_base=0x%llx\n",
		pdd->lds_base, pdd->scratch_base, pdd->gpuvm_base);
	pr_info("hk: qpd sh_mem_config=0x%x sh_mem_bases=0x%x\n",
		pdd->qpd.sh_mem_config, pdd->qpd.sh_mem_bases);

	{
		struct kfd_topology_device *topo_dev;
		u32 total_cwsr;
		topo_dev = kfd_topology_device_by_id(pdd->dev->id);
		if (!topo_dev) { pr_err("hk: no topo\n"); ret = -EINVAL; goto out; }
		total_cwsr = (topo_dev->node_props.cwsr_size +
			      topo_dev->node_props.debug_memory_size) *
			     NUM_XCC(pdd->dev->xcc_mask);
		total_cwsr = ALIGN(total_cwsr, PAGE);
		pr_info("hk: cwsr=0x%x debug=0x%x total=0x%x\n",
			topo_dev->node_props.cwsr_size,
			topo_dev->node_props.debug_memory_size,
			total_cwsr);

#define HK_ALLOC_MAP(mem, va, sz, fl) do {					\
		ret = amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu(				\
			adev, va, sz, drm_priv, &mem, &mmap_off, fl, false);		\
		if (ret) { pr_err("hk: alloc " #mem ": %d\n", ret); goto out; }		\
		ret = amdgpu_amdkfd_gpuvm_map_memory_to_gpu(adev, mem, drm_priv);	\
		if (ret) { pr_err("hk: map " #mem ": %d\n", ret); goto out; }		\
	} while (0)

		HK_ALLOC_MAP(ring_mem, HK_GPUVA_RING, RING_BYTES, fl);
		HK_ALLOC_MAP(wptr_mem, HK_GPUVA_WPTR, PAGE,       fl);
		HK_ALLOC_MAP(rptr_mem, HK_GPUVA_RPTR, PAGE,       fl);
		HK_ALLOC_MAP(ctx_mem,  HK_GPUVA_CTX,  total_cwsr, fl);
#undef HK_ALLOC_MAP
		pr_info("hk: 4 buffers allocated\n");
	}

	{
		struct queue_properties qp;
		struct kfd_topology_device *topo_dev;
		u32 total_cwsr;
		memset(&qp, 0, sizeof(qp));
		qp.type = KFD_QUEUE_TYPE_COMPUTE;
		qp.format = KFD_QUEUE_FORMAT_PM4;
		qp.queue_address = HK_GPUVA_RING;
		qp.queue_size    = RING_BYTES;
		qp.priority = 1;
		qp.queue_percent = 100;
		qp.read_ptr  = (void __user *)HK_GPUVA_RPTR;
		qp.write_ptr = (void __user *)HK_GPUVA_WPTR;
		qp.wptr_bo = wptr_mem->bo;
		qp.rptr_bo = rptr_mem->bo;
		qp.ring_bo = ring_mem->bo;

		topo_dev = kfd_topology_device_by_id(pdd->dev->id);
		if (topo_dev) {
			total_cwsr = (topo_dev->node_props.cwsr_size +
				      topo_dev->node_props.debug_memory_size) *
				     NUM_XCC(pdd->dev->xcc_mask);
			total_cwsr = ALIGN(total_cwsr, PAGE);
			qp.ctx_save_restore_area_address = HK_GPUVA_CTX;
			qp.ctx_save_restore_area_size = topo_dev->node_props.cwsr_size;
			qp.ctl_stack_size = topo_dev->node_props.ctl_stack_size;
		}

		ret = kfd_queue_acquire_buffers(pdd, &qp);
		if (ret) {
			struct amdgpu_vm *vm = drm_priv_to_vm(pdd->drm_priv);
			struct amdgpu_bo_va_mapping *m;
			pr_err("hk: acquire_buffers: %d\n", ret);
			amdgpu_bo_reserve(vm->root.bo, false);
			m = amdgpu_vm_bo_lookup_mapping(vm, (uint64_t)qp.write_ptr >> AMDGPU_GPU_PAGE_SHIFT);
			pr_err("hk: wptr mapping: %s start=0x%llx last=0x%llx\n",
				m?"Y":"N", m?m->start:0, m?m->last:0);
			m = amdgpu_vm_bo_lookup_mapping(vm, (uint64_t)qp.read_ptr >> AMDGPU_GPU_PAGE_SHIFT);
			pr_err("hk: rptr mapping: %s start=0x%llx last=0x%llx\n",
				m?"Y":"N", m?m->start:0, m?m->last:0);
			m = amdgpu_vm_bo_lookup_mapping(vm, (uint64_t)qp.queue_address >> AMDGPU_GPU_PAGE_SHIFT);
			pr_err("hk: ring mapping: %s start=0x%llx last=0x%llx\n",
				m?"Y":"N", m?m->start:0, m?m->last:0);
			m = amdgpu_vm_bo_lookup_mapping(vm, (uint64_t)qp.ctx_save_restore_area_address >> AMDGPU_GPU_PAGE_SHIFT);
			pr_err("hk: ctx mapping: %s start=0x%llx last=0x%llx\n",
				m?"Y":"N", m?m->start:0, m?m->last:0);
			pr_err("hk: qp: wptr=0x%llx rptr=0x%llx ring=0x%llx ctx=0x%llx\n",
				(uint64_t)qp.write_ptr, (uint64_t)qp.read_ptr,
				(uint64_t)qp.queue_address, (uint64_t)qp.ctx_save_restore_area_address);
			pr_err("hk: qp: queue_size=0x%llx cwsr_size=0x%x ctl_stack=0x%x\n",
				(uint64_t)qp.queue_size, qp.ctx_save_restore_area_size, qp.ctl_stack_size);
			amdgpu_bo_unreserve(vm->root.bo);
			goto out;
		}
		pr_info("hk: acquire_buffers OK  ring_bo=%px wptr_bo=%px rptr_bo=%px\n",
			qp.ring_bo, qp.wptr_bo, qp.rptr_bo);

		ret = kfd_alloc_process_doorbells(adev->kfd.dev, pdd);
		if (ret) { pr_err("hk: doorbell: %d\n", ret); goto out; }

		ret = pqm_create_queue(&hk_proc->pqm, pdd->dev, NULL, &qp,
				       &hk_qid, NULL, NULL, NULL, &db_off);
		if (ret) { pr_err("hk: create_queue: %d\n", ret); goto out; }
		hk_ready = true;
		pr_info("hk: after create_queue: sh_mem_config=0x%x sh_mem_bases=0x%x vmid=%u\n",
			pdd->qpd.sh_mem_config, pdd->qpd.sh_mem_bases, pdd->qpd.vmid);

		{
			struct queue *q = pqm_get_user_queue(&hk_proc->pqm, hk_qid);
			struct device_queue_manager *dqm = pdd->dev->dqm;
			pr_info("hk: QUEUE id=%u active=%d doorbell_off=%u pipe=%u queue=%u\n",
				hk_qid, q ? q->properties.is_active : -1,
				q ? q->properties.doorbell_off : 0,
				q ? q->pipe : 0,
				q ? q->queue : 0);
			pr_info("hk: DQM sched_running=%d active_runlist=%d active_qcount=%d\n",
				dqm->sched_running, dqm->active_runlist,
				dqm->active_queue_count);
			pr_info("hk: q properties.vmid=%u is_active=%d is_evicted=%d\n",
				q ? q->properties.vmid : 0xFF,
				q ? q->properties.is_active : -1,
				q ? q->properties.is_evicted : -1);
		}
	}

	amdgpu_amdkfd_gpuvm_sync_memory(adev, ring_mem, false);
	amdgpu_amdkfd_gpuvm_sync_memory(adev, wptr_mem, false);
	amdgpu_amdkfd_gpuvm_sync_memory(adev, rptr_mem, false);

	{
		struct queue *q = pqm_get_user_queue(&hk_proc->pqm, hk_qid);
		uint32_t doorbell_idx = q ? q->properties.doorbell_off : 0;
		struct v9_mqd *m = (struct v9_mqd *)q->mqd;
		pr_info("hk: MQD: pq_base=0x%x%08x pq_rptr=0x%x%08x pq_wptr=0x%x%08x\n",
			m->cp_hqd_pq_base_hi, m->cp_hqd_pq_base_lo,
			m->cp_hqd_pq_rptr_report_addr_hi, m->cp_hqd_pq_rptr_report_addr_lo,
			m->cp_hqd_pq_wptr_poll_addr_hi, m->cp_hqd_pq_wptr_poll_addr_lo);
		pr_info("hk: MQD: pq_control=0x%x doorbell_control=0x%x active=0x%x vmid=%u\n",
			m->cp_hqd_pq_control, m->cp_hqd_pq_doorbell_control, m->cp_hqd_active, m->cp_hqd_vmid);
		pr_info("hk: MQD: pq_wptr_lo=0x%x pq_wptr_hi=0x%x pq_rptr=0x%x\n",
			m->cp_hqd_pq_wptr_lo, m->cp_hqd_pq_wptr_hi, m->cp_hqd_pq_rptr);
		pr_info("hk: QUEUE pipe=%u queue=%u doorbell_off=%u\n",
			q->pipe, q->queue, doorbell_idx);

		{
			uint32_t hqd_active, hqd_rptr, hqd_wptr_lo, db_ctrl, pq_ctrl, vmid_reg;
			uint32_t sh_mem_config, sh_mem_bases;
			kgd_gfx_v9_acquire_queue(adev, q->pipe, q->queue, 0);
			hqd_active = RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE);
			hqd_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);
			hqd_wptr_lo = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO);
			db_ctrl = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL);
			pq_ctrl = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL);
			vmid_reg = RREG32_SOC15(GC, 0, mmCP_HQD_VMID);
			kgd_gfx_v9_release_queue(adev, 0);

			pr_info("hk: HW: ACTIVE=0x%x RPTR=0x%x WPTR=0x%x VMID_REG=0x%x\n",
				hqd_active, hqd_rptr, hqd_wptr_lo, vmid_reg);
			pr_info("hk: HW: DB_CTRL=0x%x PQ_CTRL=0x%x\n", db_ctrl, pq_ctrl);
		}
	}

	{
		uint32_t *r, *w, *rp;
		uint32_t rptr_before, rptr_after;
		struct queue *q = pqm_get_user_queue(&hk_proc->pqm, hk_qid);
		uint32_t doorbell_idx = q ? q->properties.doorbell_off : 0;

		ret = amdgpu_bo_kmap(ring_mem->bo, (void **)&r);
		if (ret) goto out;
		ret = amdgpu_bo_kmap(wptr_mem->bo, (void **)&w);
		if (ret) goto out;

		memset(r, 0, RING_BYTES);
		*w = 0;

		if (amdgpu_bo_kmap(rptr_mem->bo, (void **)&rp) == 0) {
			rptr_before = *rp;
			amdgpu_bo_kunmap(rptr_mem->bo);
		} else {
			rptr_before = 0;
		}

		r[0] = 0xC0001000U;
		dma_wmb();
		*w = 1;
		__sync_synchronize();
		WDOORBELL32(doorbell_idx, 1);

		kgd_gfx_v9_acquire_queue(adev, q->pipe, q->queue, 0);
		WREG32_SOC15_RLC(GC, 0, mmCP_HQD_PQ_WPTR_LO, 1);
		WREG32_SOC15_RLC(GC, 0, mmCP_HQD_PQ_WPTR_HI, 0);
		kgd_gfx_v9_release_queue(adev, 0);
		pr_info("hk: NOP + direct WPTR=1\n");

		msleep(1000);

		{
			uint32_t hqd_active, hqd_rptr, hqd_wptr_lo;
			kgd_gfx_v9_acquire_queue(adev, q->pipe, q->queue, 0);
			hqd_active = RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE);
			hqd_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);
			hqd_wptr_lo = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO);
			kgd_gfx_v9_release_queue(adev, 0);
			pr_info("hk: after NOP: CP_HQD_ACTIVE=0x%x RPTR=0x%x WPTR_LO=0x%x\n",
				hqd_active, hqd_rptr, hqd_wptr_lo);
		}

		if (amdgpu_bo_kmap(rptr_mem->bo, (void **)&rp) == 0) {
			rptr_after = *rp;
			amdgpu_bo_kunmap(rptr_mem->bo);
		} else {
			rptr_after = 0;
		}

		pr_info("hk: rptr %u→%u %s\n", rptr_before, rptr_after,
			rptr_after > rptr_before ? "CP ALIVE!" : "(no change)");
	}

	return 0;
out:
	pr_err("hk: FAILED (ret=%d)\n", ret);
	return ret;
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
	pr_info("hk: HeteroKern CP ring test ready\n");
	hk_kobj = kobject_create_and_add("heteroken", kernel_kobj);
	if (!hk_kobj) return -ENOMEM;
	if (sysfs_create_file(hk_kobj, &run_attr.attr))
		pr_warn("hk: sysfs failed\n");
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
