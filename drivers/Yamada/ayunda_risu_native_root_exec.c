// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/Yamada/ayunda_risu_native_root_exec.c
 * Ayunda Risu Native Root Execution Engine
 * Executes shell commands from kernel as root, bypassing SELinux via KernelSU
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/slab.h>

/**
 * ayunda_risu_exec - Execute a command natively as root
 * @cmd: The shell command to execute
 */
int ayunda_risu_exec(const char *cmd)
{
	int ret;
	char *argv[] = { "/system/bin/sh", "-c", (char *)cmd, NULL };
	char *envp[] = { 
		"HOME=/", 
		"PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin", 
		"ANDROID_ROOT=/system",
		"ANDROID_DATA=/data",
		NULL 
	};

	if (!cmd)
		return -EINVAL;

	pr_info("ayunda_risu: Executing native root command: %s\n", cmd);
	
	/* 
	 * call_usermodehelper executes as UID 0 in the kernel domain.
	 * The PavoliaReinePatch automatically injects SELinux rules into KernelSU 
	 * to grant the kernel domain full access to shell_exec and SU transition.
	 */
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);

	pr_info("ayunda_risu: Command finished with exit code %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(ayunda_risu_exec);

static int __init ayunda_risu_init(void)
{
	pr_info("ayunda_risu: Native Root Execution Engine initialized.\n");
	return 0;
}
late_initcall(ayunda_risu_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Ayunda Risu Native Root Exec");
