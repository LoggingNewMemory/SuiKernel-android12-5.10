// sandevistan_boot.c
// SPDX-License-Identifier: GPL-2.0-only
// Sandevistan Boot — Temporary CPUfreq performance boost for faster boot
// Sets 'performance' at boot, reverts to 'schedutil' after BOOST_DELAY_SEC
// Universal for GKI 2.0 / Android 5.10

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/cpu.h>

#define BOOST_DELAY_SEC   30
#define GOV_BOOST         "performance"
#define GOV_NORMAL        "schedutil"

static bool sandevistan_enabled = true;
module_param(sandevistan_enabled, bool, 0644);
MODULE_PARM_DESC(sandevistan_enabled, "Enable Sandevistan boot governor boost (default: true)");

static struct delayed_work revert_work;

static int set_governor_all(const char *gov_name)
{
    int cpu, ret = 0;

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (strcmp(policy->governor->name, gov_name) == 0) {
            cpufreq_cpu_put(policy);
            continue;
        }

        ret = cpufreq_set_policy_governor(policy, gov_name);
        if (ret)
            pr_warn("sandevistan_boot: failed to set %s on cpu%d: %d\n",
                    gov_name, cpu, ret);

        cpufreq_cpu_put(policy);
    }
    return ret;
}

static void revert_governor_work(struct work_struct *work)
{
    pr_info("sandevistan_boot: flatline — reverting to %s\n", GOV_NORMAL);
    set_governor_all(GOV_NORMAL);
}

static int __init sandevistan_boot_init(void)
{
    if (!sandevistan_enabled) {
        pr_info("sandevistan_boot: disabled\n");
        return 0;
    }

    pr_info("sandevistan_boot: jacking in — %s for %ds\n",
            GOV_BOOST, BOOST_DELAY_SEC);

    INIT_DELAYED_WORK(&revert_work, revert_governor_work);
    set_governor_all(GOV_BOOST);
    schedule_delayed_work(&revert_work,
                          msecs_to_jiffies(BOOST_DELAY_SEC * 1000));
    return 0;
}

static void __exit sandevistan_boot_exit(void)
{
    cancel_delayed_work_sync(&revert_work);
    set_governor_all(GOV_NORMAL);
    pr_info("sandevistan_boot: unloaded\n");
}

module_init(sandevistan_boot_init);
module_exit(sandevistan_boot_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Sandevistan Boot — temporary performance governor boost for faster boot");