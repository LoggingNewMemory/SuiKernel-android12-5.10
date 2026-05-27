// yamada_gaming_boost.c
// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — CPU Input Boost via PM QoS
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define BOOST_DURATION_MS   500

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);
MODULE_PARM_DESC(yamada_boost_enabled, "Enable Yamada Gaming Boost (default: true)");

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);
MODULE_PARM_DESC(boost_duration_ms, "Boost duration in ms after last input (default: 500)");

/* ------------------------------------------------------------------ */
/* State & QoS Requests                                               */
/* ------------------------------------------------------------------ */

static bool is_boosted = false;
static DEFINE_SPINLOCK(boost_lock);

static struct freq_qos_request boost_min_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

static struct work_struct boost_on_work;
static struct delayed_work boost_off_work;
static struct input_handler yamada_input_handler;

/* ------------------------------------------------------------------ */
/* QoS Helpers                                                        */
/* ------------------------------------------------------------------ */

static void boost_qos_init(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && !qos_initialized[cpu]) {
            freq_qos_add_request(&policy->constraints,
                                 &boost_min_req[cpu],
                                 FREQ_QOS_MIN,
                                 policy->cpuinfo.min_freq);
            qos_initialized[cpu] = true;
        }
        cpufreq_cpu_put(policy);
    }
}

static void boost_qos_cleanup(void)
{
    unsigned int cpu;

    for_each_possible_cpu(cpu) {
        if (qos_initialized[cpu]) {
            freq_qos_remove_request(&boost_min_req[cpu]);
            qos_initialized[cpu] = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Boost ON / OFF Workqueues                                          */
/* ------------------------------------------------------------------ */

static void do_boost_on(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        if (!qos_initialized[cpu])
            continue;

        policy = cpufreq_cpu_get(cpu);
        if (policy) {
            if (policy->cpu == cpu) {
                freq_qos_update_request(&boost_min_req[cpu], policy->cpuinfo.max_freq);
            }
            cpufreq_cpu_put(policy);
        }
    }
    pr_debug("yamada_gaming_boost: touch boost ON\n");
}

static void do_boost_off(struct work_struct *work)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;
    unsigned long flags;

    spin_lock_irqsave(&boost_lock, flags);
    is_boosted = false;
    spin_unlock_irqrestore(&boost_lock, flags);

    for_each_online_cpu(cpu) {
        if (!qos_initialized[cpu])
            continue;

        policy = cpufreq_cpu_get(cpu);
        if (policy) {
            if (policy->cpu == cpu) {
                freq_qos_update_request(&boost_min_req[cpu], policy->cpuinfo.min_freq);
            }
            cpufreq_cpu_put(policy);
        }
    }
    pr_debug("yamada_gaming_boost: touch boost OFF\n");
}

/* ------------------------------------------------------------------ */
/* Input Handler                                                      */
/* ------------------------------------------------------------------ */

static void yamada_input_event(struct input_handle *handle,
                               unsigned int type,
                               unsigned int code,
                               int value)
{
    unsigned long flags;
    bool need_boost_on = false;

    /* We only care about touch (EV_ABS) and physical keys (EV_KEY) */
    if (type != EV_ABS && type != EV_KEY)
        return;

    if (!yamada_boost_enabled)
        return;

    spin_lock_irqsave(&boost_lock, flags);
    
    /* If we aren't already boosted, flag it so we can schedule the ON work */
    if (!is_boosted) {
        is_boosted = true;
        need_boost_on = true;
    }
    
    spin_unlock_irqrestore(&boost_lock, flags);

    /* Only schedule the ON work if this is the very first touch */
    if (need_boost_on) {
        schedule_work(&boost_on_work);
    }

    /* 
     * Every single touch event pushes the timer back another 500ms.
     * As long as you hold the virtual joystick, it NEVER drops!
     * But 500ms after you lift your thumb... boom, back to normal.
     */
    mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(boost_duration_ms));
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
    { .driver_info = 1 },
    { },
};
MODULE_DEVICE_TABLE(input, yamada_input_ids);

/* ------------------------------------------------------------------ */
/* Init / Exit                                                        */
/* ------------------------------------------------------------------ */

static int __init yamada_gaming_boost_init(void)
{
    int ret;

    if (!yamada_boost_enabled) {
        pr_info("yamada_gaming_boost: disabled\n");
        return 0;
    }

    memset(qos_initialized, 0, sizeof(qos_initialized));
    
    /* Initialize QoS requests */
    boost_qos_init();

    INIT_WORK(&boost_on_work, do_boost_on);
    INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

    yamada_input_handler.event      = yamada_input_event;
    yamada_input_handler.connect    = yamada_input_connect;
    yamada_input_handler.disconnect = yamada_input_disconnect;
    yamada_input_handler.name       = "yamada_gaming_boost";
    yamada_input_handler.id_table   = yamada_input_ids;

    ret = input_register_handler(&yamada_input_handler);
    if (ret) {
        pr_err("yamada_gaming_boost: failed to register input handler: %d\n", ret);
        boost_qos_cleanup();
        return ret;
    }

    pr_info("yamada_gaming_boost: active — PM QoS boost duration %dms\n", boost_duration_ms);
    return 0;
}

static void __exit yamada_gaming_boost_exit(void)
{
    input_unregister_handler(&yamada_input_handler);

    /* Safely drain workqueues so we don't crash if unloading while touched */
    cancel_work_sync(&boost_on_work);
    cancel_delayed_work_sync(&boost_off_work);

    /* Clean up QoS limits */
    boost_qos_cleanup();

    pr_info("yamada_gaming_boost: unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — CPU input boost via PM QoS");