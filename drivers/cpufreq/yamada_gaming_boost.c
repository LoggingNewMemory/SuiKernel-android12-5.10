// yamada_gaming_boost.c
// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — CPU Input Boost + Schedutil Rate Limit Tuning
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define BOOST_DURATION_MS   500

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);
MODULE_PARM_DESC(yamada_boost_enabled, "Enable Yamada Gaming Boost (default: true)");

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);
MODULE_PARM_DESC(boost_duration_ms, "Boost duration in ms after last input (default: 500)");

/* ------------------------------------------------------------------ */
/* Copied from kernel/sched/cpufreq_schedutil.c — not exported         */
/* ------------------------------------------------------------------ */

struct sugov_tunables {
    struct gov_attr_set     attr_set;
    unsigned int            rate_limit_us;
};

struct sugov_policy {
    struct cpufreq_policy   *policy;
    struct sugov_tunables   *tunables;
    /* We only need the above two fields — rest omitted intentionally */
};

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool boost_active = false;
static DEFINE_MUTEX(boost_lock);
static struct delayed_work boost_off_work;
static struct input_handler yamada_input_handler;

static unsigned int saved_rate_limit_us[NR_CPUS];
static unsigned int saved_min[NR_CPUS];

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
}

/* ------------------------------------------------------------------ */
/* Boost ON                                                             */
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

            /* Save current state */
            saved_min[cpu] = policy->min ?
                             policy->min : policy->cpuinfo.min_freq;

            /* Save schedutil rate_limit_us */
            if (sg_policy && sg_policy->tunables)
                saved_rate_limit_us[cpu] =
                    sg_policy->tunables->rate_limit_us;

            /* Pin CPU to max */
            policy->min = policy->cpuinfo.max_freq;
            cpufreq_driver_target(policy, policy->cpuinfo.max_freq,
                                  CPUFREQ_RELATION_H);
            cpufreq_update_policy(cpu);

            /* Set schedutil rate_limit_us to 0 for instant response */
            set_schedutil_rate_limit(policy, 0);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* Boost OFF                                                            */
/* ------------------------------------------------------------------ */

static void do_boost_off(struct work_struct *work)
{
    unsigned int cpu;

    mutex_lock(&boost_lock);
    boost_active = false;
    mutex_unlock(&boost_lock);

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            struct sugov_policy *sg_policy = policy->governor_data;

            /* Restore CPU min */
            policy->min = saved_min[cpu] ?
                          saved_min[cpu] : policy->cpuinfo.min_freq;
            cpufreq_update_policy(cpu);

            /* Restore schedutil rate_limit_us */
            if (sg_policy && sg_policy->tunables)
                sg_policy->tunables->rate_limit_us =
                    saved_rate_limit_us[cpu];
        }

        cpufreq_cpu_put(policy);
    }

    pr_debug("yamada_gaming_boost: boost off\n");
}

/* ------------------------------------------------------------------ */
/* Input handler                                                        */
/* ------------------------------------------------------------------ */

static void yamada_input_event(struct input_handle *handle,
                                unsigned int type,
                                unsigned int code,
                                int value)
{
    if (type != EV_ABS && type != EV_KEY)
        return;

    mutex_lock(&boost_lock);

    if (!boost_active) {
        boost_active = true;
        do_boost_on();
        pr_debug("yamada_gaming_boost: boost on\n");
    }

    /* Reset the off timer on every input event */
    mod_delayed_work(system_wq, &boost_off_work,
                     msecs_to_jiffies(boost_duration_ms));

    mutex_unlock(&boost_lock);
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
    cancel_delayed_work_sync(&boost_off_work);

    if (boost_active)
        do_boost_off(NULL);

    pr_info("yamada_gaming_boost: unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — CPU input boost + schedutil rate limit tuning");