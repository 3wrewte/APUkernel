/*
 * hk_diag.c — HeteroKern: diagnostic kernel module for KFD queue creation
 *
 * Opens /dev/kfd from kernel space, follows the exact same code path
 * as the userspace ioctl, but with printk at every step.
 *
 * Build: sudo make -C /lib/modules/$(uname -r)/build M=/tmp modules
 * Run:   sudo insmod hk_diag.ko kln_addr=0x$(sudo grep ' kallsyms_lookup_name$' /proc/kallsyms | awk '{print $1}')
 *        sudo dmesg | grep hk_diag
 *        sudo rmmod hk_diag
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fdtable.h>

MODULE_LICENSE("GPL");

static ulong kln_addr = 0;
module_param(kln_addr, ulong, 0);
MODULE_PARM_DESC(kln_addr, "Address of kallsyms_lookup_name");

static unsigned long (*kln)(const char *name);

/* ── Key KFD symbols ── */
static unsigned long sym_kfd_get_process;
static unsigned long sym_kfd_process_device_data_by_id;
static unsigned long sym_kfd_bind_process_to_device;
static unsigned long sym_pqm_create_queue;
static unsigned long sym_amdgpu_vm_bo_lookup_mapping;
static unsigned long sym_drm_priv_to_vm;

/* ── Load from GPU ID from topology ── */
static unsigned long GPU_ID = 43858;

static int __init hk_diag_init(void)
{
	if (!kln_addr) {
		pr_err("hk_diag: need kln_addr param\n");
		return -EINVAL;
	}
	kln = (void *)kln_addr;

#define L(sym) sym_##sym = kln(#sym); \
	pr_info("hk_diag: %-45s %s\n", #sym, sym_##sym ? "OK" : "NOT FOUND")

	L(kfd_get_process);
	L(kfd_process_device_data_by_id);
	L(kfd_bind_process_to_device);
	L(pqm_create_queue);
	L(amdgpu_vm_bo_lookup_mapping);
	L(drm_priv_to_vm);

	/* ── Get current KFD process ── */
	if (!sym_kfd_get_process) {
		pr_err("hk_diag: kfd_get_process not found\n");
		return -ENOENT;
	}

	/* kfd_get_process(struct task_struct *) → struct kfd_process * */
	typedef void *(*fn_t)(void *);
	fn_t get_proc = (fn_t)sym_kfd_get_process;
	void *kfd_proc = get_proc(current);
	if (!kfd_proc) {
		pr_err("hk_diag: current task is not a KFD process\n");
		pr_err("  (run insmod from a shell that has opened /dev/kfd)\n");
		return -EINVAL;
	}
	pr_info("hk_diag: KFD process at %px\n", kfd_proc);

	/* ── Get PDD ── */
	if (!sym_kfd_process_device_data_by_id || !sym_kfd_bind_process_to_device) {
		pr_err("hk_diag: missing device symbols\n");
		return -ENOENT;
	}

	/*
	 * KFD internal function signatures:
	 *   kfd_process_device_data_by_id(struct kfd_process *p, uint32_t gpu_id)
	 *       → struct kfd_process_device *
	 *   kfd_bind_process_to_device(struct kfd_node *dev, struct kfd_process *p)
	 *       → struct kfd_process_device *
	 */

	typedef void *(*pdd_fn_t)(void *, uint32_t);
	pdd_fn_t get_pdd = (pdd_fn_t)sym_kfd_process_device_data_by_id;
	void *pdd = get_pdd(kfd_proc, (uint32_t)GPU_ID);
	if (!pdd) {
		pr_err("hk_diag: PDD not found for GPU 0x%lx\n", GPU_ID);
		return -EINVAL;
	}
	pr_info("hk_diag: PDD at %px\n", pdd);

	/* Get kfd_node from PDD */
	/* struct kfd_process_device { ... struct kfd_node *dev; ... } */
	/* dev is at offset 0 in the struct (first member) */
	void *kfd_node = *(void **)pdd;
	pr_info("hk_diag: KFD node at %px\n", kfd_node);

	/* Bind process to device */
	typedef void *(*bind_fn_t)(void *, void *);
	bind_fn_t bind = (bind_fn_t)sym_kfd_bind_process_to_device;
	pdd = bind(kfd_node, kfd_proc);
	if (IS_ERR(pdd)) {
		pr_err("hk_diag: bind failed: %ld\n", PTR_ERR(pdd));
		return PTR_ERR(pdd);
	}
	pr_info("hk_diag: Bound PDD at %px\n", pdd);

	/* ── Get VM ── */
	/*
	 * PDD structure (approximate):
	 *   struct kfd_node *dev;
	 *   void *drm_priv;   // at offset sizeof(pointer)
	 *   ...                // the process_queue_manager is embedded
	 *
	 * kfd_priv.h line ~305:
	 *   struct kfd_process_device {
	 *       struct kfd_node *dev;
	 *       ...
	 *   }
	 *
	 * And kfd_process.h:
	 *   struct kfd_process {
	 *       ...
	 *       struct process_queue_manager pqm;
	 *       ...
	 *   }
	 */

	/* Get drm_priv from PDD: offset 8 (second pointer field) */
	void *drm_priv = *(void **)((char *)pdd + 8);
	pr_info("hk_diag: drm_priv at %px\n", drm_priv);

	/* drm_priv_to_vm(drm_priv) → struct amdgpu_vm * */
	if (sym_drm_priv_to_vm) {
		typedef void *(*vm_fn_t)(void *);
		vm_fn_t to_vm = (vm_fn_t)sym_drm_priv_to_vm;
		void *vm = to_vm(drm_priv);
		pr_info("hk_diag: GPU VM at %px\n", vm);

		/* Test: can we look up a known GPU VA in the VM? */
		if (sym_amdgpu_vm_bo_lookup_mapping && vm) {
			/*
			 * amdgpu_vm_bo_lookup_mapping(struct amdgpu_vm *vm,
			 *     uint64_t addr) → struct amdgpu_bo_va_mapping *
			 *
			 * addr is in GPU page units (already shifted >> AMDGPU_GPU_PAGE_SHIFT)
			 */
			typedef void *(*lookup_fn_t)(void *, uint64_t);
			lookup_fn_t lookup = (lookup_fn_t)sym_amdgpu_vm_bo_lookup_mapping;

			/* Try several GPU VAs to see if any are mapped */
			uint64_t test_vas[] = { 0x1000, 0x100000, 0x1000000, 0x1010000 };
			for (int i = 0; i < 4; i++) {
				void *map = lookup(vm, test_vas[i]);
				pr_info("hk_diag: VA 0x%llx (page %llu) → %s\n",
					(unsigned long long)(test_vas[i] << 12),
					(unsigned long long)test_vas[i],
					map ? "MAPPED" : "NOT FOUND");
			}
		}
	}

	pr_info("hk_diag: diagnostic complete\n");
	return 0;
}

static void __exit hk_diag_exit(void)
{
	pr_info("hk_diag: unloaded\n");
}

module_init(hk_diag_init);
module_exit(hk_diag_exit);
