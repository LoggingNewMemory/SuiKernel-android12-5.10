// schedutil_enforcer.c
// SPDX-License-Identifier: GPL-2.0-only
// Schedutil Enforcer — Enforce Schedutil as Default Governor
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/raco_override.h>  /* Raco API header */

#define ENFORCER_DELAY_MS 25000   /* Wake up right as vendor services deploy */
#define ENFORCER_SCAN_MS  5000

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

static struct task_struct *enforcer_thread;

/* Control variable registered to Raco API */
static int raco_governor_trigger = 0;

/* Internal helper to safely write configurations to the Virtual File System */
static int enforcer_write_file(const char *path, const char *buf)
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
/* Core Governor Enforcement Engine                                    */
/* ------------------------------------------------------------------ */
static void enforce_schedutil_governor(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;
    char path[64];

    /* Loop through every logical CPU core on the device */
    for_each_possible_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        /* Check if the policy is currently active and not already running stock schedutil */
        if (policy->governor && strncmp(policy->governor->name, "schedutil", 9) != 0) {
            pr_info("schedutil_enforcer: Caught rogue governor '%s' on CPU %d! Purging...\n", 
                    policy->governor->name, cpu);

            /* Generate path dynamically for each CPU policy node block */
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);

            /* Safely force standard 'schedutil' using our VFS helper system */
            if (enforcer_write_file(path, "schedutil") == 0) {
                pr_info("schedutil_enforcer: Successfully enforced standard 'schedutil' on CPU %d!\n", cpu);
            } else {
                pr_err("schedutil_enforcer: Failed to enforce governor on CPU %d\n", cpu);
            }
        }
        
        /* Always release the cpufreq reference counter to avoid kernel memory leaks */
        cpufreq_cpu_put(policy);
    }
}

static int enforcer_worker(void *data)
{
    pr_info("schedutil_enforcer: standing by — engaging in %dms\n", ENFORCER_DELAY_MS);
    msleep(ENFORCER_DELAY_MS);

    /* Initial execution right after boot stabilization */
    enforce_schedutil_governor();

    while (!kthread_should_stop()) {
        /* If Raco Sniper resets our trigger back to 1, it means a boot-time
         vendor script tried to hijack our system layout. We catch the signal,
         force our clean schedutil layout back on, and reset it to 0!
         */
        if (raco_governor_trigger == 1) {
            pr_info("schedutil_enforcer: Raco Core pulse detected! Overwriting rogue vendor governors.\n");
            enforce_schedutil_governor();
            raco_governor_trigger = 0; /* Re-arm the snaring mechanism */
        }

        msleep_interruptible(ENFORCER_SCAN_MS);
    }

    return 0;
}

static int __init schedutil_enforcer_init(void)
{
    if (raco_register_rc_override(&raco_governor_trigger, 1, "schedutil.governor_lock") == 0) {
        pr_info("schedutil_enforcer: Governor automation successfully hooked to Raco Core API\n");
    } else {
        pr_err("schedutil_enforcer: Critical failure hooking to Raco Engine\n");
    }

    enforcer_thread = kthread_run(enforcer_worker, NULL, "sugov_enforcer");
    if (IS_ERR(enforcer_thread)) {
        pr_err("schedutil_enforcer: Failed to start background worker thread\n");
        return PTR_ERR(enforcer_thread);
    }

    pr_info("schedutil_enforcer: Module armed and active.\n");
    return 0;
}

static void __exit schedutil_enforcer_exit(void)
{
    if (enforcer_thread)
        kthread_stop(enforcer_thread);

    pr_info("schedutil_enforcer: Module unloaded cleanly.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Enforce Schedutil as Default Governor");