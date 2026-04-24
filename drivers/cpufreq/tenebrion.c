// tenebrion.c
// SPDX-License-Identifier: GPL-2.0-only
// Tenebrion — Suspend state based CPU frequency throttler
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
/* Screen OFF (Suspend) — drop max to hardware min                     */
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
            /* Save current user limits with fallback to cpuinfo */
            original_min[cpu] = policy->min ?
                                 policy->min : policy->cpuinfo.min_freq;
            original_max[cpu] = policy->max ?
                                 policy->max : policy->cpuinfo.max_freq;

            /* Drop both min and max to hardware minimum */
            policy->min = policy->cpuinfo.min_freq;
            policy->max = policy->cpuinfo.min_freq;
            cpufreq_driver_target(policy, policy->cpuinfo.min_freq,
                                  CPUFREQ_RELATION_L);
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
/* Screen ON (Resume) — restore previous freq range                    */
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
            /* Restore saved limits, fallback to cpuinfo if zero */
            policy->min = original_min[cpu] ?
                          original_min[cpu] : policy->cpuinfo.min_freq;
            policy->max = original_max[cpu] ?
                          original_max[cpu] : policy->cpuinfo.max_freq;
            cpufreq_driver_target(policy, policy->max,
                                  CPUFREQ_RELATION_H);
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u restored min=%u max=%u KHz\n",
                    cpu, policy->min, policy->max);
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
/* PM Notifier                                                          */
/* ------------------------------------------------------------------ */

static int tenebrion_pm_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
    switch (event) {
    case PM_SUSPEND_PREPARE:
        schedule_work(&min_freq_work);
        break;
    case PM_POST_SUSPEND:
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
/* Init / Exit                                                          */
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

    /* If unloading while throttled, restore synchronously to avoid
       kernel panic from workqueue executing after module unload */
    if (is_screen_off)
        tenebrion_restore_freq(NULL);

    cancel_work_sync(&min_freq_work);
    cancel_work_sync(&restore_freq_work);

    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion: Suspend state based CPU frequency throttler");