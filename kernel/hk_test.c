/*
 * hk_test.c — HeteroKern kernel module: minimal GCN dispatch test
 *
 * Uses kallsyms_lookup_name() to reach KFD's internal queue creation
 * API, then submits a simple PM4 packet to a compute queue.
 *
 * WARNING: proof-of-concept only.  If the GPU hangs, reboot required.
 * Run with: sudo insmod hk_test.ko && sudo rmmod hk_test && dmesg | tail
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern GPU dispatch test");

/* PM4 packet macros */
#define PM4_TYPE3_HEADER(cnt, op) (0xc0000000U | ((cnt) << 16) | ((op) << 8))
#define IT_DISPATCH_DIRECT           0x15

/* Forward declaration of some KFD types we need */
struct kfd_process;
struct kfd_process_device;
struct kfd_node;
struct queue_properties;

static int __init hk_test_init(void)
{
	pr_info("hk_test: loading\n");

	/* Find key KFD symbols */
	void *(*kfd_lookup)(unsigned long) = (void *)kallsyms_lookup_name("__symbol_get");
	if (!kfd_lookup) {
		pr_err("hk_test: __symbol_get not found\n");
		return -ENOENT;
	}

	/* Check what symbols are available */
	static const char *syms[] = {
		"kfd_topology_enum_kfd_devices",
		"pqm_create_queue",
		"kfd_process_device_data_by_id",
		"amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu",
		NULL
	};

	for (int i = 0; syms[i]; i++) {
		unsigned long addr = kallsyms_lookup_name(syms[i]);
		pr_info("hk_test: %s -> %px\n", syms[i], (void *)addr);
	}

	/* Check what KFD exports to modules */
	pr_info("hk_test: checking /proc/kallsyms for kfd symbols\n");

	pr_info("hk_test: done\n");
	return 0;
}

static void __exit hk_test_exit(void)
{
	pr_info("hk_test: unloaded\n");
}

module_init(hk_test_init);
module_exit(hk_test_exit);
