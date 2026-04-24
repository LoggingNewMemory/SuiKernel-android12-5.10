// tenebrion.c
// SPDX-License-Identifier: GPL-2.0-only
// Tenebrion — CPU freq floor drop on screen off
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/fb.h>

static bool tenebrion_enabled = true;
module_param(tenebrion_enabled, bool, 0644);
MODULE_PARM_DESC(tenebrion_enabled, "Enable Tenebrion (default: true)");

static struct work_struct screen_off_work;
static struct work_struct screen_on_work;
static struct notifier_block fb_notif;

/* ------------------------------------------------------------------ */
/* Screen OFF — set all CPU min to cpuinfo.min_freq                    */
/* ------------------------------------------------------------------ */

static void do_screen_off(struct work_struct *work)
{
    unsigned int cpu;

    pr_info("tenebrion: screen off — dropping CPU floors\n");

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            policy->min = policy->cpuinfo.min_freq;
            policy->max = policy->cpuinfo.min_freq;
            cpufreq_driver_target(policy, policy->cpuinfo.min_freq,
                                  CPUFREQ_RELATION_L);
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u → min=%u max=%u KHz\n",
                    cpu,
                    policy->cpuinfo.min_freq,
                    policy->cpuinfo.min_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* Screen ON — restore full freq range                                  */
/* ------------------------------------------------------------------ */

static void do_screen_on(struct work_struct *work)
{
    unsigned int cpu;

    pr_info("tenebrion: screen on — restoring CPU freq range\n");

    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            policy->min = policy->cpuinfo.min_freq;
            policy->max = policy->cpuinfo.max_freq;
            cpufreq_driver_target(policy, policy->cpuinfo.max_freq,
                                  CPUFREQ_RELATION_H);
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u → min=%u max=%u KHz\n",
                    cpu,
                    policy->cpuinfo.min_freq,
                    policy->cpuinfo.max_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* FB Notifier                                                          */
/* ------------------------------------------------------------------ */

static int tenebrion_fb_notifier_call(struct notifier_block *nb,
                                       unsigned long action,
                                       void *data)
{
    struct fb_event *evdata = data;
    int *blank;

    if (action != FB_EVENT_BLANK)
        return NOTIFY_OK;

    if (!evdata || !evdata->data)
        return NOTIFY_OK;

    blank = evdata->data;

    if (*blank == FB_BLANK_UNBLANK) {
        /* Screen on */
        schedule_work(&screen_on_work);
    } else if (*blank == FB_BLANK_POWERDOWN) {
        /* Screen off */
        schedule_work(&screen_off_work);
    }

    return NOTIFY_OK;
}

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init tenebrion_init(void)
{
    if (!tenebrion_enabled) {
        pr_info("tenebrion: disabled\n");
        return 0;
    }

    INIT_WORK(&screen_off_work, do_screen_off);
    INIT_WORK(&screen_on_work, do_screen_on);

    fb_notif.notifier_call = tenebrion_fb_notifier_call;
    fb_register_client(&fb_notif);

    pr_info("tenebrion: active — watching screen state\n");
    return 0;
}

static void __exit tenebrion_exit(void)
{
    fb_unregister_client(&fb_notif);
    cancel_work_sync(&screen_off_work);
    cancel_work_sync(&screen_on_work);
    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion — CPU freq floor drop on screen off");