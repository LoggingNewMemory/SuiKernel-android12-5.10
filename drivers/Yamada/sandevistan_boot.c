// sandevistan_boot.c
// SPDX-License-Identifier: GPL-3.0-only
// Sandevistan Boot — min=max lock for 30s, GKI 2.0 safe (PM QoS Method)
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/pm_qos.h>

#define BOOST_DELAY_MS   10000
#define REVERT_DELAY_MS  60000

static bool sandevistan_enabled = true;
module_param(sandevistan_enabled, bool, 0644);
MODULE_PARM_DESC(sandevistan_enabled, "Enable Sandevistan boot boost (default: true)");

static struct delayed_work boost_work;
static struct delayed_work revert_work;

/* QoS requests per policy CPU */
static struct freq_qos_request sandevistan_min_req[NR_CPUS];
static struct freq_qos_request sandevistan_max_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

static void do_boost(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    pr_info("sandevistan_boot: jacking in — locking min=max via QoS\n");

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            if (!qos_initialized[cpu]) {
                /*
                 * Add max_req first (sets ceiling), then min_req
                 * (raises floor to meet it).  This avoids a transient
                 * min > max state in the QoS arbiter.
                 */
                freq_qos_add_request(&policy->constraints,
                                     &sandevistan_max_req[cpu],
                                     FREQ_QOS_MAX,
                                     policy->cpuinfo.max_freq);
                freq_qos_add_request(&policy->constraints,
                                     &sandevistan_min_req[cpu],
                                     FREQ_QOS_MIN,
                                     policy->cpuinfo.max_freq);
                qos_initialized[cpu] = true;
            } else {
                /* Raise ceiling before floor */
                freq_qos_update_request(&sandevistan_max_req[cpu],
                                        policy->cpuinfo.max_freq);
                freq_qos_update_request(&sandevistan_min_req[cpu],
                                        policy->cpuinfo.max_freq);
            }

            pr_info("sandevistan_boot: policy%u locked to %u KHz\n",
                    cpu, policy->cpuinfo.max_freq);
        }

        cpufreq_cpu_put(policy);
    }

    schedule_delayed_work(&revert_work,
                          msecs_to_jiffies(REVERT_DELAY_MS));
}

static void do_revert(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    pr_info("sandevistan_boot: flatline — restoring original mins via QoS\n");

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu && qos_initialized[cpu]) {
            /*
             * Lower min_req floor first so it can never exceed the
             * max_req value during the transition, then release the
             * max_req ceiling lock back to hardware max.
             */
            freq_qos_update_request(&sandevistan_min_req[cpu],
                                    policy->cpuinfo.min_freq);
            freq_qos_update_request(&sandevistan_max_req[cpu],
                                    policy->cpuinfo.max_freq);

            pr_info("sandevistan_boot: policy%u restored min=%u max=%u KHz\n",
                    cpu, policy->cpuinfo.min_freq, policy->cpuinfo.max_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

static void sandevistan_qos_cleanup(void)
{
    unsigned int cpu;

    for_each_possible_cpu(cpu) {
        if (qos_initialized[cpu]) {
            freq_qos_remove_request(&sandevistan_min_req[cpu]);
            freq_qos_remove_request(&sandevistan_max_req[cpu]);
            qos_initialized[cpu] = false;
        }
    }
}

static int __init sandevistan_boot_init(void)
{
    if (!sandevistan_enabled) {
        pr_info("sandevistan_boot: disabled\n");
        return 0;
    }

    memset(qos_initialized, 0, sizeof(qos_initialized));

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
    
    sandevistan_qos_cleanup();
    
    pr_info("sandevistan_boot: unloaded\n");
}

module_init(sandevistan_boot_init);
module_exit(sandevistan_boot_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sandevistan Boot — min=max freq lock during boot, GKI 2.0 safe (PM QoS Method)");