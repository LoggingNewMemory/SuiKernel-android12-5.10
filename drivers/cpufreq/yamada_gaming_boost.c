// yamada_gaming_boost.c
// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — Boost CPU when get touch
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/input.h>

#define BOOST_DURATION_MS   500

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);

static bool is_boosted = false;
static DEFINE_SPINLOCK(boost_lock);

static struct freq_qos_request boost_min_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

static struct work_struct boost_on_work;
static struct delayed_work boost_off_work;

/* Hook dari drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

static void do_boost_on(struct work_struct *work) {
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (policy) {
            if (policy->cpu == cpu) {
                if (!qos_initialized[cpu]) {
                    freq_qos_add_request(&policy->constraints,
                                         &boost_min_req[cpu],
                                         FREQ_QOS_MIN,
                                         policy->cpuinfo.min_freq);
                    qos_initialized[cpu] = true;
                }
                /* Tarik batas minimal ke puncak! */
                freq_qos_update_request(&boost_min_req[cpu], policy->cpuinfo.max_freq);
            }
            cpufreq_cpu_put(policy);
        }
    }
    pr_info("yamada_gaming_boost: touch boost ON\n");
}

static void do_boost_off(struct work_struct *work) {
    unsigned int cpu;
    struct cpufreq_policy *policy;
    unsigned long flags;

    spin_lock_irqsave(&boost_lock, flags);
    is_boosted = false;
    spin_unlock_irqrestore(&boost_lock, flags);

    for_each_online_cpu(cpu) {
        if (!qos_initialized[cpu]) continue;
        
        policy = cpufreq_cpu_get(cpu);
        if (policy) {
            if (policy->cpu == cpu) {
                freq_qos_update_request(&boost_min_req[cpu], policy->cpuinfo.min_freq);
            }
            cpufreq_cpu_put(policy);
        }
    }
    pr_info("yamada_gaming_boost: touch boost OFF\n");
}

static void kobo_trigger_boost(void) {
    unsigned long flags;
    bool need_boost_on = false;

    if (!yamada_boost_enabled) return;

    spin_lock_irqsave(&boost_lock, flags);
    if (!is_boosted) {
        is_boosted = true;
        need_boost_on = true;
    }
    spin_unlock_irqrestore(&boost_lock, flags);

    if (need_boost_on) {
        schedule_work(&boost_on_work);
    }

    mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(boost_duration_ms));
}

static void boost_qos_cleanup(void) {
    unsigned int cpu;
    for_each_possible_cpu(cpu) {
        if (qos_initialized[cpu]) {
            freq_qos_remove_request(&boost_min_req[cpu]);
            qos_initialized[cpu] = false;
        }
    }
}

static int __init yamada_gaming_boost_init(void) {
    if (!yamada_boost_enabled) return 0;

    memset(qos_initialized, 0, sizeof(qos_initialized));

    INIT_WORK(&boost_on_work, do_boost_on);
    INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

    yamada_boost_hook = kobo_trigger_boost;

    pr_info("yamada_gaming_boost: active — JIT PM QoS Edition\n");
    return 0;
}

static void __exit yamada_gaming_boost_exit(void) {
    yamada_boost_hook = NULL;

    cancel_work_sync(&boost_on_work);
    cancel_delayed_work_sync(&boost_off_work);
    boost_qos_cleanup();

    pr_info("yamada_gaming_boost: unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — Boost CPU when get touch");