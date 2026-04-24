// tenebrion.c
// SPDX-License-Identifier: GPL-2.0-only
// Tenebrion — Screen state based CPU frequency throttler
// Ported from userspace daemon to kernel driver
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#define POLL_INTERVAL_MS    3000
#define DPMS_PATH           "/sys/class/drm/card0-DSI-1/dpms"
#define BACKLIGHT_PATH      "/sys/class/leds/lcd-backlight/brightness"

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

enum tenebrion_path {
    PATH_NONE       = 0,
    PATH_DPMS       = 1,
    PATH_BACKLIGHT  = 2,
};

static enum tenebrion_path active_path = PATH_NONE;
static bool is_screen_off = false;
static DEFINE_MUTEX(tenebrion_lock);
static struct task_struct *watcher_thread;
static unsigned int original_min[NR_CPUS];
static unsigned int original_max[NR_CPUS];

/* ------------------------------------------------------------------ */
/* File read helper                                                     */
/* ------------------------------------------------------------------ */

static int tenebrion_read_file(const char *path, char *buf, size_t size)
{
    struct file *f;
    loff_t pos = 0;
    int ret;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return -1;

    ret = kernel_read(f, buf, size - 1, &pos);
    filp_close(f, NULL);

    if (ret > 0)
        buf[ret] = '\0';
    else
        ret = -1;

    return ret;
}

/* ------------------------------------------------------------------ */
/* Path auto-detection                                                  */
/* ------------------------------------------------------------------ */

static enum tenebrion_path tenebrion_detect_path(void)
{
    char buf[64];

    /* Try DPMS first */
    if (tenebrion_read_file(DPMS_PATH, buf, sizeof(buf)) > 0) {
        pr_info("tenebrion: detected path → %s\n", DPMS_PATH);
        return PATH_DPMS;
    }

    /* Fall back to backlight brightness */
    if (tenebrion_read_file(BACKLIGHT_PATH, buf, sizeof(buf)) > 0) {
        pr_info("tenebrion: detected path → %s\n", BACKLIGHT_PATH);
        return PATH_BACKLIGHT;
    }

    return PATH_NONE;
}

/* ------------------------------------------------------------------ */
/* Screen state detection                                               */
/* Returns: 1 = on, 0 = off, -1 = unknown                             */
/* ------------------------------------------------------------------ */

static int tenebrion_get_screen_state(void)
{
    char buf[64];
    int len;

    switch (active_path) {
    case PATH_DPMS:
        len = tenebrion_read_file(DPMS_PATH, buf, sizeof(buf));
        if (len <= 0)
            return -1;
        if (strstr(buf, "On"))
            return 1;
        if (strstr(buf, "Off"))
            return 0;
        return -1;

    case PATH_BACKLIGHT:
        len = tenebrion_read_file(BACKLIGHT_PATH, buf, sizeof(buf));
        if (len <= 0)
            return -1;
        if (simple_strtol(buf, NULL, 10) > 0)
            return 1;
        return 0;

    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* CPUFreq — drop to min                                               */
/* ------------------------------------------------------------------ */

static void tenebrion_set_min_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu) {
            original_min[cpu] = policy->min ?
                                 policy->min : policy->cpuinfo.min_freq;
            original_max[cpu] = policy->max ?
                                 policy->max : policy->cpuinfo.max_freq;

            policy->min = policy->cpuinfo.min_freq;
            policy->max = policy->cpuinfo.min_freq;
            cpufreq_driver_target(policy, policy->cpuinfo.min_freq,
                                  CPUFREQ_RELATION_L);
            cpufreq_update_policy(cpu);

            pr_info("tenebrion: policy%u → %u KHz (screen off)\n",
                    cpu, policy->cpuinfo.min_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* CPUFreq — restore                                                    */
/* ------------------------------------------------------------------ */

static void tenebrion_restore_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu) {
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
}

/* ------------------------------------------------------------------ */
/* Watcher kthread                                                      */
/* ------------------------------------------------------------------ */

static int tenebrion_watcher(void *data)
{
    int current_state = -1;
    int last_state = -1;

    pr_info("tenebrion: watcher started, polling every %dms\n",
            POLL_INTERVAL_MS);
    
    msleep(15000);
    
    while (!kthread_should_stop()) {
        /* Retry path detection if not found at init */
        if (active_path == PATH_NONE)
            active_path = tenebrion_detect_path();

        current_state = tenebrion_get_screen_state();

        if (current_state != -1 && current_state != last_state) {
            mutex_lock(&tenebrion_lock);

            if (current_state == 0 && !is_screen_off) {
                tenebrion_set_min_freq();
                is_screen_off = true;
                pr_info("tenebrion: screen OFF → CPUs throttled\n");
            } else if (current_state == 1 && is_screen_off) {
                tenebrion_restore_freq();
                is_screen_off = false;
                pr_info("tenebrion: screen ON → CPUs restored\n");
            }

            mutex_unlock(&tenebrion_lock);
            last_state = current_state;
        }

        msleep_interruptible(POLL_INTERVAL_MS);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init tenebrion_init(void)
{
    active_path = tenebrion_detect_path();
    if (active_path == PATH_NONE)
        pr_warn("tenebrion: no screen state path found — will retry in watcher\n");

    watcher_thread = kthread_run(tenebrion_watcher, NULL, "tenebrion");
    if (IS_ERR(watcher_thread)) {
        pr_err("tenebrion: failed to start watcher thread: %ld\n",
               PTR_ERR(watcher_thread));
        return PTR_ERR(watcher_thread);
    }

    pr_info("tenebrion: active — path=%d poll=%dms\n",
            active_path, POLL_INTERVAL_MS);
    return 0;
}

static void __exit tenebrion_exit(void)
{
    kthread_stop(watcher_thread);

    if (is_screen_off) {
        mutex_lock(&tenebrion_lock);
        tenebrion_restore_freq();
        is_screen_off = false;
        mutex_unlock(&tenebrion_lock);
    }

    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion: Screen state based CPU frequency throttler");