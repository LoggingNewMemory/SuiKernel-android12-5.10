// tenebrion.c
// SPDX-License-Identifier: GPL-2.0-only
// Tenebrion — Screen state based CPU frequency throttler
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>
#include <linux/mutex.h>

static unsigned int original_max[NR_CPUS];
static unsigned int original_min[NR_CPUS];
static bool is_screen_off = false;
static DEFINE_MUTEX(tenebrion_lock);

/* ------------------------------------------------------------------ */
/* Screen OFF (Suspend) — drop max to hardware min                    */
/* ------------------------------------------------------------------ */

static void tenebrion_set_min_freq(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    mutex_lock(&tenebrion_lock);

    if (is_screen_off)
        goto out;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            /* Save the CURRENT user limits, NOT the hardware max, 
               so we don't break user underclocks when restoring */
            original_min[cpu] = policy->min;
            original_max[cpu] = policy->max;

            /* Drop both min and max to hardware minimum */
            policy->min = policy->cpuinfo.min_freq;
            policy->max = policy->cpuinfo.min_freq;
            
            /* Updating policy is enough; the governor handles the target */
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u throttled to %u KHz\n",
                    cpu, policy->cpuinfo.min_freq);
        }

        cpufreq_cpu_put(policy);
    }

    is_screen_off = true;
    pr_info("tenebrion: suspend active — all CPUs at minimum\n");

out:
    mutex_unlock(&tenebrion_lock);
}

/* ------------------------------------------------------------------ */
/* Screen ON (Resume) — restore previous freq range                   */
/* ------------------------------------------------------------------ */

static void tenebrion_restore_freq(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    mutex_lock(&tenebrion_lock);

    if (!is_screen_off)
        goto out;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            /* Restore the limits the user actually had before sleep */
            policy->min = original_min[cpu];
            policy->max = original_max[cpu];
            
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u restored min=%u max=%u KHz\n",
                    cpu, original_min[cpu], original_max[cpu]);
        }

        cpufreq_cpu_put(policy);
    }

    is_screen_off = false;
    pr_info("tenebrion: resume active — all CPUs restored\n");

out:
    mutex_unlock(&tenebrion_lock);
}

static DECLARE_WORK(min_freq_work, tenebrion_set_min_freq);
static DECLARE_WORK(restore_freq_work, tenebrion_restore_freq);

/* ------------------------------------------------------------------ */
/* PM Notifier                                                        */
/* ------------------------------------------------------------------ */

static int tenebrion_pm_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
    switch (event) {
    case PM_SUSPEND_PREPARE:
        /* System going to sleep — screen is off */
        schedule_work(&min_freq_work);
        break;
    case PM_POST_SUSPEND:
        /* System resumed — screen coming back on */
        schedule_work(&restore_freq_work);
        break;
    }

    return NOTIFY_OK;
}

static struct notifier_block tenebrion_pm_nb = {
    .notifier_call = tenebrion_pm_notifier,
    .priority      = INT_MAX,
};

/* ------------------------------------------------------------------ */
/* Init / Exit                                                        */
/* ------------------------------------------------------------------ */

static int __init tenebrion_init(void)
{
    register_pm_notifier(&tenebrion_pm_nb);
    pr_info("tenebrion: active — watching suspend state\n");
    return 0;
}

static void __exit tenebrion_exit(void)
{
    unregister_pm_notifier(&tenebrion_pm_nb);
    
    /* If we are unloading while throttled, synchronously restore.
       Do not schedule_work here, as the module might unload before 
       the workqueue executes, causing a kernel panic. */
    if (is_screen_off) {
        tenebrion_restore_freq(NULL); 
    }

    cancel_work_sync(&min_freq_work);
    cancel_work_sync(&restore_freq_work);

    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion: Suspend state based CPU frequency throttler");