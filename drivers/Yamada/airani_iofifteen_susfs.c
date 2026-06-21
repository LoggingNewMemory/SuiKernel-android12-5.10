// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/Yamada/airani_iofifteen_susfs.c
 * Airani Iofifteen SUSFS Modifier
 * Dynamically hides paths, spoofs files, and manages SUSFS from the kernel
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>

extern int ayunda_risu_exec(const char *cmd);

/**
 * airani_iofifteen_susfs_hide_path - Hides a specific path using SUSFS
 * @path: The absolute path to hide
 */
int airani_iofifteen_susfs_hide_path(const char *path)
{
	char cmd[256];

	if (!path)
		return -EINVAL;

	/* 
	 * Instead of fighting with __user pointers and ioctls in kernel space,
	 * we brilliantly chain this into our Ayunda Risu Native Root Exec!
	 * We just instruct ksud (which supports susfs via susfs4ksu) to hide it.
	 */
	snprintf(cmd, sizeof(cmd), "/data/adb/ksud susfs add_sus_path %s", path);
	
	pr_info("airani_iofifteen: Hiding path via SUSFS -> %s\n", path);
	return ayunda_risu_exec(cmd);
}
EXPORT_SYMBOL_GPL(airani_iofifteen_susfs_hide_path);

/**
 * airani_iofifteen_susfs_hide_mount - Hides a specific mount using SUSFS
 * @path: The mount path to hide
 */
int airani_iofifteen_susfs_hide_mount(const char *path)
{
	char cmd[256];

	if (!path)
		return -EINVAL;

	snprintf(cmd, sizeof(cmd), "/data/adb/ksud susfs add_sus_mount %s", path);
	
	pr_info("airani_iofifteen: Hiding mount via SUSFS -> %s\n", path);
	return ayunda_risu_exec(cmd);
}
EXPORT_SYMBOL_GPL(airani_iofifteen_susfs_hide_mount);

static int __init airani_iofifteen_init(void)
{
	pr_info("airani_iofifteen: SUSFS API Initialized.\n");
	return 0;
}
late_initcall(airani_iofifteen_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Airani Iofifteen SUSFS Modifier");
