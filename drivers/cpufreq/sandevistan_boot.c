// sandevistan_boot.c
// SPDX-License-Identifier: GPL-2.0-only
// Sandevistan Boot — notifier + frequency pinning, GKI 2.0 clean

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/mutex.h>

#define BOOST_DELAY_MS  30000

static bool sandevistan_enabled = true;
module_param(sandevistan_enabled, bool, 0644);
MODULE_PARM_DESC(sandevistan_enabled, "Enable Sandevistan boot boost (default: true)");

static bool boost_active = true;
static DEFINE_MUTEX(boost_lock);
static struct delayed_work revert_work;
static struct notifier_block cpufreq_nb;

/* Pin policy to max freq — works regardless of governor */
static void pin_policy_max(struct cpufreq_policy *policy)
{
    policy->user_policy.min = policy->max;
    policy->user_policy.max = policy->max;
    cpufreq_update_policy(policy->cpu);
    pr_info("sandevistan_boot: cpu%u pinned to %u KHz\n",
            policy->cpu, policy->max);
}

/* Release freq pin — restore original min */
static void unpin_policy(struct cpufreq_policy *policy)
{
    policy->user_policy.min = policy->cpuinfo.min_freq;
    policy->user_policy.max = policy->cpuinfo.max_freq;
    cpufreq_update_policy(policy->cpu);
    pr_info("sandevistan_boot: cpu%u released\n", policy->cpu);
}

static int cpufreq_sandevistan_notifier(struct notifier_block *nb,
                                         unsigned long event, void *data)
{
    struct cpufreq_policy *policy = data;

    if (event != CPUFREQ_CREATE_POLICY)
        return NOTIFY_OK;

    mutex_lock(&boost_lock);
    if (boost_active)
        pin_policy_max(policy);
    mutex_unlock(&boost_lock);

    return NOTIFY_OK;
}

static void revert_work_fn(struct work_struct *work)
{
    struct cpufreq_policy *policy;
    unsigned int cpu;

    pr_info("sandevistan_boot: flatline — releasing all CPU pins\n");

    mutex_lock(&boost_lock);
    boost_active = false;
    mutex_unlock(&boost_lock);

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;
        if (policy->cpu == cpu)
            unpin_policy(policy);
        cpufreq_cpu_put(policy);
    }
}

static int __init sandevistan_boot_init(void)
{
    if (!sandevistan_enabled) {
        pr_info("sandevistan_boot: disabled\n");
        return 0;
    }

    pr_info("sandevistan_boot: jacking in — pinning CPUs to max for %dms\n",
            BOOST_DELAY_MS);

    INIT_DELAYED_WORK(&revert_work, revert_work_fn);

    cpufreq_nb.notifier_call = cpufreq_sandevistan_notifier;
    cpufreq_register_notifier(&cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);

    schedule_delayed_work(&revert_work, msecs_to_jiffies(BOOST_DELAY_MS));
    return 0;
}

static void __exit sandevistan_boot_exit(void)
{
    cancel_delayed_work_sync(&revert_work);
    cpufreq_unregister_notifier(&cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
    pr_info("sandevistan_boot: unloaded\n");
}

module_init(sandevistan_boot_init);
module_exit(sandevistan_boot_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sandevistan Boot — max freq pin during boot, GKI 2.0 safe");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);