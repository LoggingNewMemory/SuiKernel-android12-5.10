// SPDX-License-Identifier: GPL-3.0-only
// drivers/Yamada/suikernel_manager_version_verifier.c
// SuiKernel Manager Version Verifier
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define SUIKERNEL_MANAGER_VERSION "2.0"

static int suikernel_manager_version_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", SUIKERNEL_MANAGER_VERSION);
	return 0;
}

static int __init suikernel_manager_version_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create_single("suikernel_manager_version", 0444, NULL, suikernel_manager_version_show);
	if (!entry) {
		pr_err("suikernel_manager_version: Failed to create /proc/suikernel_manager_version\n");
		return -ENOMEM;
	}

	pr_info("suikernel_manager_version: Initialized. Supported Manager Version: %s\n", SUIKERNEL_MANAGER_VERSION);
	return 0;
}

static void __exit suikernel_manager_version_exit(void)
{
	remove_proc_entry("suikernel_manager_version", NULL);
}

module_init(suikernel_manager_version_init);
module_exit(suikernel_manager_version_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("SuiKernel Manager Version Verifier");
