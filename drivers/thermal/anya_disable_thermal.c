// anya_disable_thermal.c
// SPDX-License-Identifier: GPL-2.0-only
// Anya Disable Thermal — Kernel-side thermal zone disabler
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>

#define THERMAL_ZONE_MAX    64
#define DISABLE_DELAY_MS    20000
#define THERMAL_BASE        "/sys/class/thermal/thermal_zone"

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

static bool anya_thermal_enabled = true;
module_param(anya_thermal_enabled, bool, 0644);
MODULE_PARM_DESC(anya_thermal_enabled, "Enable Anya Disable Thermal (default: true)");

static struct task_struct *anya_thermal_thread;

/* ------------------------------------------------------------------ */
/* File write helper                                                    */
/* ------------------------------------------------------------------ */

static int anya_write_file(const char *path, const char *buf)
{
    struct file *f;
    loff_t pos = 0;
    int ret;

    f = filp_open(path, O_WRONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    ret = kernel_write(f, buf, strlen(buf), &pos);
    filp_close(f, NULL);

    return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* File read helper                                                     */
/* ------------------------------------------------------------------ */

static int anya_read_file(const char *path, char *buf, size_t size)
{
    struct file *f;
    loff_t pos = 0;
    int ret;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return -1;

    ret = kernel_read(f, buf, size - 1, &pos);
    filp_close(f, NULL);

    if (ret > 0)
        buf[ret] = '\0';
    else
        ret = -1;

    return ret;
}

/* ------------------------------------------------------------------ */
/* Disable all thermal zones                                            */
/* ------------------------------------------------------------------ */

static void anya_disable_all_zones(void)
{
    char path[128];
    char buf[32];
    int i, ret;
    int disabled_count = 0;

    for (i = 0; i < THERMAL_ZONE_MAX; i++) {
        snprintf(path, sizeof(path), "%s%d/mode", THERMAL_BASE, i);

        /* Check if zone exists */
        ret = anya_read_file(path, buf, sizeof(buf));
        if (ret < 0)
            break;

        /* Skip if already disabled */
        if (strstr(buf, "disabled")) {
            disabled_count++;
            continue;
        }

        /* Write disabled */
        ret = anya_write_file(path, "disabled");
        if (ret < 0) {
            pr_warn("anya_disable_thermal: zone%d failed (%d)\n", i, ret);
            continue;
        }

        pr_info("anya_disable_thermal: zone%d disabled\n", i);
        disabled_count++;
    }

    pr_info("anya_disable_thermal: %d thermal zones disabled\n",
            disabled_count);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                        */
/* ------------------------------------------------------------------ */

static int anya_thermal_worker(void *data)
{
    pr_info("anya_disable_thermal: standing by — engaging in %dms\n",
            DISABLE_DELAY_MS);

    /* Wait for KernelSU SELinux rules to be applied */
    msleep(DISABLE_DELAY_MS);

    pr_info("anya_disable_thermal: disabling all thermal zones\n");
    anya_disable_all_zones();

    return 0;
}

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init anya_disable_thermal_init(void)
{
    if (!anya_thermal_enabled) {
        pr_info("anya_disable_thermal: disabled\n");
        return 0;
    }

    anya_thermal_thread = kthread_run(anya_thermal_worker, NULL,
                                      "anya_disable_thermal");
    if (IS_ERR(anya_thermal_thread)) {
        pr_err("anya_disable_thermal: failed to start thread: %ld\n",
               PTR_ERR(anya_thermal_thread));
        return PTR_ERR(anya_thermal_thread);
    }

    pr_info("anya_disable_thermal: active\n");
    return 0;
}

static void __exit anya_disable_thermal_exit(void)
{
    pr_info("anya_disable_thermal: unloaded\n");
}

module_init(anya_disable_thermal_init);
module_exit(anya_disable_thermal_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Anya Disable Thermal — kernel-side thermal zone disabler");