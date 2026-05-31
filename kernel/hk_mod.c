/*
 * hk_mod.c — HeteroKern: minimal KFD dispatch test
 *
 * Receives kallsyms_lookup_name address as a module parameter,
 * found by parsing /proc/kallsyms in a helper script.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HeteroKern GPU test");

static ulong kln_addr = 0;
module_param(kln_addr, ulong, 0);
MODULE_PARM_DESC(kln_addr, "Address of kallsyms_lookup_name");

static unsigned long (*kln)(const char *name);

static int __init hk_mod_init(void)
{
	if (!kln_addr) {
		pr_err("hk_mod: kln_addr parameter required\n");
		pr_err("  sudo insmod hk_mod.ko kln_addr=$(sudo grep ' kallsyms_lookup_name$' /proc/kallsyms | awk '{print $1}')\n");
		return -EINVAL;
	}

	kln = (void *)kln_addr;
	pr_info("hk_mod: kallsyms_lookup_name at %px\n", (void *)kln_addr);

	static const char *syms[] = {
		"pqm_create_queue",
		"kfd_processes_table",
		"kfd_bind_process_to_device",
		"kfd_topology_enum_kfd_devices",
		"amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu",
		"amdgpu_amdkfd_gpuvm_map_memory_to_gpu",
		NULL
	};

	for (int i = 0; syms[i]; i++) {
		unsigned long a = kln(syms[i]);
		pr_info("hk_mod: %-45s %s\n", syms[i],
			a ? "FOUND" : "NOT FOUND");
	}

	pr_info("hk_mod: ready\n");
	return 0;
}

static void __exit hk_mod_exit(void)
{
	pr_info("hk_mod: unloaded\n");
}

module_init(hk_mod_init);
module_exit(hk_mod_exit);
