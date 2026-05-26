// schedutil_enforcer.c
// SPDX-License-Identifier: GPL-2.0-only
// Schedutil Enforcer — Enforce Schedutil as Default Governor
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/cpufreq.h>
#include <linux/string.h>
#include <linux/raco_override.h>  /* Raco API header */

#define ENFORCER_DELAY_MS 25000   /* Wake up right as vendor services deploy */
#define ENFORCER_SCAN_MS  5000

static struct task_struct *enforcer_thread;

/* Control variable registered to Raco API */
static int raco_governor_trigger = 0;

/* ------------------------------------------------------------------ */
/* Core Governor Enforcement Engine                                    */
/* ------------------------------------------------------------------ */
static void enforce_schedutil_governor(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;
    struct cpufreq_governor *default_gov;

    /* 1. Natively query the verified default scheduler governor structure pointer */
    default_gov = cpufreq_default_governor();
    if (!default_gov) {
        pr_err("schedutil_enforcer: Default governor structure is unavailable!\n");
        return;
    }

    /* Double check to ensure the default governor is indeed schedutil */
    if (strncmp(default_gov->name, "schedutil", 9) != 0) {
        pr_warn("schedutil_enforcer: Warning: Default governor is '%s', not stock schedutil.\n", default_gov->name);
    }

    /* 2. Loop through every logical CPU core on the device */
    for_each_possible_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        /* Check if the policy is currently active and running a rogue governor */
        if (policy->governor && strncmp(policy->governor->name, "schedutil", 9) != 0) {
            pr_info("schedutil_enforcer: Caught rogue governor '%s' on CPU %d! Purging...\n", 
                    policy->governor->name, cpu);

            /* Acquire the internal policy mutex lock safely as defined in cpufreq.h */
            down_write(&policy->rwsem);

            /* 🪄 THE STRUCT INJECTION:
             * Directly override the target governor slot with the verified default structural object
             */
            policy->governor = default_gov;
            
            /* Re-initialize the native governor handlers if present */
            if (policy->governor->init)
                policy->governor->init(policy);

            up_write(&policy->rwsem);
            
            /* Trigger an asynchronous policy update to recalculate frequency mappings */
            cpufreq_update_policy(cpu);
            
            pr_info("schedutil_enforcer: Successfully enforced default governor pointer on CPU %d!\n", cpu);
        }
        
        /* Always release the cpufreq reference counter to prevent memory tracking leaks */
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