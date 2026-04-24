// tenebrion.c
// SPDX-License-Identifier: GPL-2.0-only
// Tenebrion — Screen state based CPU frequency throttler
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/pm_qos.h>
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

/* QoS requests per policy CPU */
static struct freq_qos_request tenebrion_min_req[NR_CPUS];
static struct freq_qos_request tenebrion_max_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

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

    if (tenebrion_read_file(DPMS_PATH, buf, sizeof(buf)) > 0) {
        pr_info("tenebrion: detected path → %s\n", DPMS_PATH);
        return PATH_DPMS;
    }

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
/* QoS init — add requests for all online policies                     */
/* ------------------------------------------------------------------ */

static void tenebrion_qos_init(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && !qos_initialized[cpu]) {
            /* Add min QoS request — start unconstrained */
            freq_qos_add_request(&policy->constraints,
                                 &tenebrion_min_req[cpu],
                                 FREQ_QOS_MIN,
                                 policy->cpuinfo.min_freq);

            /* Add max QoS request — start unconstrained */
            freq_qos_add_request(&policy->constraints,
                                 &tenebrion_max_req[cpu],
                                 FREQ_QOS_MAX,
                                 policy->cpuinfo.max_freq);

            qos_initialized[cpu] = true;

            pr_info("tenebrion: QoS initialized for policy%u\n", cpu);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* CPUFreq — drop to min via QoS                                       */
/* ------------------------------------------------------------------ */

static void tenebrion_set_min_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && qos_initialized[cpu]) {
            /* Pin max to hardware min — governor can't go above */
            freq_qos_update_request(&tenebrion_max_req[cpu],
                                    policy->cpuinfo.min_freq);
            /* Pin min to hardware min as well */
            freq_qos_update_request(&tenebrion_min_req[cpu],
                                    policy->cpuinfo.min_freq);

            pr_info("tenebrion: policy%u → %u KHz (screen off)\n",
                    cpu, policy->cpuinfo.min_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* CPUFreq — restore via QoS                                           */
/* ------------------------------------------------------------------ */

static void tenebrion_restore_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && qos_initialized[cpu]) {
            /* Restore max to hardware max */
            freq_qos_update_request(&tenebrion_max_req[cpu],
                                    policy->cpuinfo.max_freq);
            /* Restore min to hardware min */
            freq_qos_update_request(&tenebrion_min_req[cpu],
                                    policy->cpuinfo.min_freq);

            pr_info("tenebrion: policy%u restored min=%u max=%u KHz\n",
                    cpu,
                    policy->cpuinfo.min_freq,
                    policy->cpuinfo.max_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

/* ------------------------------------------------------------------ */
/* QoS cleanup                                                          */
/* ------------------------------------------------------------------ */

static void tenebrion_qos_cleanup(void)
{
    unsigned int cpu;

    for_each_possible_cpu(cpu) {
        if (qos_initialized[cpu]) {
            freq_qos_remove_request(&tenebrion_min_req[cpu]);
            freq_qos_remove_request(&tenebrion_max_req[cpu]);
            qos_initialized[cpu] = false;
        }
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

    /* Wait for Android SELinux policy + KernelSU rules to be applied */
    msleep(15000);

    /* Initialize QoS requests after delay */
    tenebrion_qos_init();

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
    memset(qos_initialized, 0, sizeof(qos_initialized));

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

    tenebrion_qos_cleanup();

    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion: Screen state based CPU frequency throttler");