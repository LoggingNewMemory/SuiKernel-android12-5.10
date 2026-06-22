// SPDX-License-Identifier: GPL-3.0-only
/*
 * ambatubank.c
 * Generic Banking App Hide
 * Hardcodes SUSFS path hiding for strict banking apps.
 */

#include <linux/module.h>
#include <linux/init.h>

extern int ksusfs_add_sus_path_kernel(const char *path);
extern void ksusfs_enable_mount_hiding(void);
extern int ksusfs_set_fake_cmdline_kernel(const char *cmdline);

static const char * const banking_banned_paths[] = {
    "/data/adb",
    "/data/adb/ksud",
    "/data/adb/ksu",
    "/data/adb/modules",
    "/data/adb/lspd",
    "/data/adb/zygisk",
    "/data/adb/shamiko",
    "/system/bin/su",
    "/system/xbin/su",
    "/sbin/su",
    "/sdcard/TWRP",
    "/sdcard/Fox",
    "/sdcard/TitaniumBackup",
    NULL
};

static int __init ambatubank_init(void)
{
    int i = 0;
    pr_info("ambatubank: Hiding Start\n");

    // 1. Hide sensitive paths
    while (banking_banned_paths[i]) {
        if (ksusfs_add_sus_path_kernel(banking_banned_paths[i]) == 0) {
            pr_info("ambatubank: Hide path -> %s\n", banking_banned_paths[i]);
        } else {
            pr_err("ambatubank: Failed to Hide -> %s\n", banking_banned_paths[i]);
        }
        i++;
    }

    // 2. Hide KernelSU Magic Mounts
    ksusfs_enable_mount_hiding();
    pr_info("ambatubank: Magic mount hiding enabled!\n");

    // 3. Spoof the raw kernel command line
    // We replace 'orange' (unlocked) with 'green' (locked) to defeat deep bootloader checks
    ksusfs_set_fake_cmdline_kernel("androidboot.verifiedbootstate=green androidboot.flash.locked=1 androidboot.vbmeta.device_state=locked");
    pr_info("ambatubank: Boot cmdline spoofed!\n");

    pr_info("ambatubank: Hide active. Make sure to 'Unmount Modules' for the Banking App in KernelSU Next!\n");
    return 0;
}

static void __exit ambatubank_exit(void)
{
    pr_info("ambatubank: Hide disabled.\n");
}

module_init(ambatubank_init);
module_exit(ambatubank_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Generic Banking App SUSFS Hide");
