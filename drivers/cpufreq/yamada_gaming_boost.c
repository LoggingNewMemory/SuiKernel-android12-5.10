// yamada_gaming_boost.c
// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — CPU Input Boost + Schedutil Rate Limit Tuning (PM QoS Method)
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>

#define BOOST_DURATION_MS   500

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);
MODULE_PARM_DESC(yamada_boost_enabled, "Enable Yamada Gaming Boost (default: true)");

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);
MODULE_PARM_DESC(boost_duration_ms, "Boost duration in ms after last input (default: 500)");

/* ------------------------------------------------------------------ */
/* Copied from kernel/sched/cpufreq_schedutil.c — not exported         */
/*                                                                      */
/* WARNING: must stay in sync with the compiled kernel's layout.       */
/* A field inserted before rate_limit_us or tunables in the upstream   */
/* structs will cause silent memory corruption at runtime.             */
/* Verified against android12-5.10 cpufreq_schedutil.c                */
/* ------------------------------------------------------------------ */

struct sugov_tunables {
    struct gov_attr_set     attr_set;       /* MUST remain first member */
    unsigned int            rate_limit_us;
};

struct sugov_policy {
    struct cpufreq_policy   *policy;        /* MUST remain first member */
    struct sugov_tunables   *tunables;
    struct list_head         tunables_hook;

    raw_spinlock_t           update_lock;
    u64                      last_freq_update_time;
    s64                      freq_update_delay_ns;  /* derived: rate_limit_us * NSEC_PER_USEC */
    /* remaining fields intentionally omitted */
};

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool boost_active = false;

/*
 * Use a spinlock, not a mutex.  yamada_input_event() is called from
 * the input core which can run in softirq context on some drivers.
 * Mutexes may sleep; spinlocks are safe in all contexts.
 */
static DEFINE_SPINLOCK(boost_lock);
static struct delayed_work boost_off_work;
static struct input_handler yamada_input_handler;

/*
 * Save rate_limit_us per policy-CPU (the first CPU of each cpufreq
 * policy cluster).  Indexed by policy->cpu, not by every sibling CPU,
 * to avoid restoring a stale zero on hotplug.
 */
static unsigned int saved_rate_limit_us[NR_CPUS];

/* QoS requests per policy CPU */
static struct freq_qos_request yamada_min_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

/* ------------------------------------------------------------------ */
/* Schedutil rate_limit_us helper                                       */
/* ------------------------------------------------------------------ */

static void set_schedutil_rate_limit(struct cpufreq_policy *policy,
                                     unsigned int rate_limit_us)
{
    struct sugov_policy *sg_policy = policy->governor_data;

    if (!sg_policy || !sg_policy->tunables)
        return;

    sg_policy->tunables->rate_limit_us = rate_limit_us;
    WRITE_ONCE(sg_policy->freq_update_delay_ns,
               (s64)rate_limit_us * NSEC_PER_USEC);
}

/* ------------------------------------------------------------------ */
/* Boost ON — called from workqueue (process context), never from      */
/* the input event directly.  Safe to call sleepable APIs here.        */
/* ------------------------------------------------------------------ */

static void do_boost_on(void)
{
    unsigned int cpu;

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            struct sugov_policy *sg_policy = policy->governor_data;

            /*
             * Save rate_limit_us keyed by the policy's representative
             * CPU so we never accidentally restore a cold-boot zero
             * caused by a sibling CPU that was offline at save time.
             */
            if (sg_policy && sg_policy->tunables)
                saved_rate_limit_us[cpu] = sg_policy->tunables->rate_limit_us;

            /* Pin CPU to max via QoS */
            if (!qos_initialized[cpu]) {
                freq_qos_add_request(&policy->constraints,
                                     &yamada_min_req[cpu],
                                     FREQ_QOS_MIN,
                                     policy->cpuinfo.max_freq);
                qos_initialized[cpu] = true;
            } else {
                freq_qos_update_request(&yamada_min_req[cpu],
                                        policy->cpuinfo.max_freq);
            }

            /* Set schedutil rate_limit_us to 0 for instant response */
            set_schedutil_rate_limit(policy, 0);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* Boost OFF — runs from system_wq (process context), safe to sleep.  */
/* ------------------------------------------------------------------ */

static void do_boost_off(struct work_struct *work)
{
    unsigned int cpu;
    unsigned long flags;

    spin_lock_irqsave(&boost_lock, flags);
    boost_active = false;
    spin_unlock_irqrestore(&boost_lock, flags);

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            struct sugov_policy *sg_policy = policy->governor_data;

            /* Restore CPU min via QoS */
            if (qos_initialized[cpu]) {
                freq_qos_update_request(&yamada_min_req[cpu],
                                        policy->cpuinfo.min_freq);
            }

            /* Restore schedutil rate_limit_us AND freq_update_delay_ns */
            if (sg_policy && sg_policy->tunables) {
                sg_policy->tunables->rate_limit_us = saved_rate_limit_us[cpu];
                WRITE_ONCE(sg_policy->freq_update_delay_ns,
                           (s64)saved_rate_limit_us[cpu] * NSEC_PER_USEC);
            }
        }

        cpufreq_cpu_put(policy);
    }

    pr_debug("yamada_gaming_boost: boost off\n");
}

/* ------------------------------------------------------------------ */
/* QoS cleanup                                                          */
/* ------------------------------------------------------------------ */

static void yamada_qos_cleanup(void)
{
    unsigned int cpu;

    for_each_possible_cpu(cpu) {
        if (qos_initialized[cpu]) {
            freq_qos_remove_request(&yamada_min_req[cpu]);
            qos_initialized[cpu] = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input handler                                                        */
/* ------------------------------------------------------------------ */

static void yamada_input_event(struct input_handle *handle,
                                unsigned int type,
                                unsigned int code,
                                int value)
{
    unsigned long flags;
    bool need_boost_on = false;

    if (type != EV_ABS && type != EV_KEY)
        return;

    spin_lock_irqsave(&boost_lock, flags);

    if (!boost_active) {
        boost_active = true;
        need_boost_on = true;
    }

    spin_unlock_irqrestore(&boost_lock, flags);

    /*
     * Schedule do_boost_on in process context so it can safely call
     * freq_qos_add/update_request() which may sleep internally.
     * mod_delayed_work with delay=0 queues it immediately.
     *
     * Also reset the off-timer on every event so the boost window
     * extends from the last touch — but only reschedule if already
     * boosting to avoid redundant timer resets on every EV_ABS flood.
     */
    if (need_boost_on) {
        do_boost_on();
        pr_debug("yamada_gaming_boost: boost on\n");
    }

    mod_delayed_work(system_wq, &boost_off_work,
                     msecs_to_jiffies(boost_duration_ms));
}

static int yamada_input_connect(struct input_handler *handler,
                                 struct input_dev *dev,
                                 const struct input_device_id *id)
{
    struct input_handle *handle;
    int ret;

    handle = kzalloc(sizeof(*handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;

    handle->dev     = dev;
    handle->handler = handler;
    handle->name    = "yamada_gaming_boost";

    ret = input_register_handle(handle);
    if (ret)
        goto err_free;

    ret = input_open_device(handle);
    if (ret)
        goto err_unregister;

    return 0;

err_unregister:
    input_unregister_handle(handle);
err_free:
    kfree(handle);
    return ret;
}

static void yamada_input_disconnect(struct input_handle *handle)
{
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

static const struct input_device_id yamada_input_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_ABS) },
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_KEY) },
    },
    { },
};
MODULE_DEVICE_TABLE(input, yamada_input_ids);

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init yamada_gaming_boost_init(void)
{
    int ret;

    if (!yamada_boost_enabled) {
        pr_info("yamada_gaming_boost: disabled\n");
        return 0;
    }

    memset(qos_initialized, 0, sizeof(qos_initialized));
    INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

    yamada_input_handler.event      = yamada_input_event;
    yamada_input_handler.connect    = yamada_input_connect;
    yamada_input_handler.disconnect = yamada_input_disconnect;
    yamada_input_handler.name       = "yamada_gaming_boost";
    yamada_input_handler.id_table   = yamada_input_ids;

    ret = input_register_handler(&yamada_input_handler);
    if (ret) {
        pr_err("yamada_gaming_boost: failed to register input handler: %d\n",
               ret);
        return ret;
    }

    pr_info("yamada_gaming_boost: active — boost duration %dms\n",
            boost_duration_ms);
    return 0;
}

static void __exit yamada_gaming_boost_exit(void)
{
    input_unregister_handler(&yamada_input_handler);

    /*
     * cancel_delayed_work_sync ensures the off-work has either
     * completed or been cancelled before we proceed to cleanup.
     * If boost is still active, run do_boost_off manually to restore
     * all frequencies before tearing down QoS requests.
     */
    cancel_delayed_work_sync(&boost_off_work);

    if (boost_active)
        do_boost_off(NULL);

    yamada_qos_cleanup();

    pr_info("yamada_gaming_boost: unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — CPU input boost + schedutil rate limit tuning (PM QoS Method)");