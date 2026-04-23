// sandevistan_boot.c
// SPDX-License-Identifier: GPL-2.0-only
// Sandevistan Boot — safe delayed boost, GKI 2.0

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>

#define BOOST_DELAY_MS   15000   /* give CPUfreq 15s to fully initialize */
#define REVERT_DELAY_MS  30000  /* revert after 30s */

static bool sandevistan_enabled = true;
module_param(sandevistan_enabled, bool, 0644);
MODULE_PARM_DESC(sandevistan_enabled, "Enable Sandevistan boot boost (default: true)");

static struct delayed_work boost_work;
static struct delayed_work revert_work;

static void do_boost(struct work_struct *work)
{
    unsigned int cpu;
    int count = 0;

    pr_info("sandevistan_boot: do_boost fired\n");

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy) {
            pr_info("sandevistan_boot: cpu%u — no policy\n", cpu);
            continue;
        }

        pr_info("sandevistan_boot: cpu%u policy found, cur=%u min=%u max=%u\n",
                cpu, policy->cur, policy->min, policy->max);

        if (policy->cpu == cpu) {
            int ret = cpufreq_driver_target(policy, policy->max,
                                            CPUFREQ_RELATION_H);
            pr_info("sandevistan_boot: cpu%u target ret=%d\n", cpu, ret);
            count++;
        }

        cpufreq_cpu_put(policy);
    }

    pr_info("sandevistan_boot: boost applied to %d policies\n", count);

    schedule_delayed_work(&revert_work,
                          msecs_to_jiffies(REVERT_DELAY_MS));
}

static void do_revert(struct work_struct *work)
{
    unsigned int cpu;

    pr_info("sandevistan_boot: flatline — releasing boost\n");

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            cpufreq_driver_target(policy, policy->min,
                                  CPUFREQ_RELATION_L);
            pr_info("sandevistan_boot: cpu%u released\n", cpu);
        }

        cpufreq_cpu_put(policy);
    }
}

static int __init sandevistan_boot_init(void)
{
    if (!sandevistan_enabled) {
        pr_info("sandevistan_boot: disabled\n");
        return 0;
    }

    pr_info("sandevistan_boot: standing by — boost in %dms\n",
            BOOST_DELAY_MS);

    INIT_DELAYED_WORK(&boost_work, do_boost);
    INIT_DELAYED_WORK(&revert_work, do_revert);

    schedule_delayed_work(&boost_work,
                          msecs_to_jiffies(BOOST_DELAY_MS));
    return 0;
}

static void __exit sandevistan_boot_exit(void)
{
    cancel_delayed_work_sync(&boost_work);
    cancel_delayed_work_sync(&revert_work);
    pr_info("sandevistan_boot: unloaded\n");
}

module_init(sandevistan_boot_init);
module_exit(sandevistan_boot_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Sandevistan Boot — safe delayed freq boost, GKI 2.0");