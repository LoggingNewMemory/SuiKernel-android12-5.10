// tenebrion.c
// SPDX-License-Identifier: GPL-3.0-only
// Tenebrion — Screen state based CPU frequency throttler + cpuset limiter
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/fb.h>


// cpuset paths that get restricted when screen is off.
// top-app / foreground are intentionally left alone — the system
// scheduler already won't run heavy foreground work while the
// screen is off, and touching those sets causes jank on wake.
#define CPUSET_SYSBG_PATH   "/dev/cpuset/system-background/cpus"

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

bool tenebrion_enabled = true;
static bool is_screen_off = false;
static DEFINE_MUTEX(tenebrion_lock);

/* QoS requests per policy CPU */
static struct freq_qos_request tenebrion_min_req[NR_CPUS];
static struct freq_qos_request tenebrion_max_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

// File helpers

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

    if (ret > 0) {
        /* strip trailing newline so comparisons are clean */
        if (buf[ret - 1] == '\n')
            buf[ret - 1] = '\0';
        else
            buf[ret] = '\0';
    } else {
        ret = -1;
    }

    return ret;
}

static int tenebrion_write_file(const char *path, const char *buf)
{
    struct file *f;
    loff_t pos = 0;
    int ret;

    f = filp_open(path, O_WRONLY, 0);
    if (IS_ERR(f)) {
        pr_warn("tenebrion: cannot open %s for write\n", path);
        return -1;
    }

    ret = kernel_write(f, buf, strlen(buf), &pos);
    filp_close(f, NULL);

    return ret > 0 ? 0 : -1;
}

// cpuset helpers

// Build a cpumask string that covers only CPU 0 — the safest single
// core to leave for background work regardless of topology.
// On screen-off we pin background and system-background cpusets to
// CPU0 only; everything else stays as-is so foreground/top-app are
// not affected.
#define CPUSET_SCREEN_OFF   "0\n"

static char saved_sysbg_cpus[32] = "";

static void tenebrion_cpuset_restrict(void)
{
    /* Save current mask before overriding */
    tenebrion_read_file(CPUSET_SYSBG_PATH, saved_sysbg_cpus, sizeof(saved_sysbg_cpus));

    tenebrion_write_file(CPUSET_SYSBG_PATH, CPUSET_SCREEN_OFF);
}

static void tenebrion_cpuset_restore(void)
{
    char sysbg_buf[32];
    char fallback_mask[32];
    int total_cores = num_possible_cpus();

    /* Forge the dynamic mask just in case the read failed (e.g. "0-7", "0-3") */
    snprintf(fallback_mask, sizeof(fallback_mask), "0-%d", total_cores - 1);

    /* tenebrion_read_file stripped the newline, so we must add it back */
    snprintf(sysbg_buf, sizeof(sysbg_buf), "%s\n", 
             saved_sysbg_cpus[0] ? saved_sysbg_cpus : fallback_mask);

    tenebrion_write_file(CPUSET_SYSBG_PATH, sysbg_buf);
}

// QoS init — add requests for all online policy CPUs

static void tenebrion_qos_init(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && !qos_initialized[cpu]) {
            freq_qos_add_request(&policy->constraints,
                                 &tenebrion_min_req[cpu],
                                 FREQ_QOS_MIN,
                                 policy->cpuinfo.min_freq);

            freq_qos_add_request(&policy->constraints,
                                 &tenebrion_max_req[cpu],
                                 FREQ_QOS_MAX,
                                 policy->cpuinfo.max_freq);

            qos_initialized[cpu] = true;

            pr_info("tenebrion: QoS initialized for policy%u "
                    "(min=%u max=%u KHz)\n",
                    cpu,
                    policy->cpuinfo.min_freq,
                    policy->cpuinfo.max_freq);
        }

        cpufreq_cpu_put(policy);
    }
}

// CPUFreq — drop to min via QoS

static void tenebrion_set_min_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && qos_initialized[cpu]) {
            // Order matters: bring min_req DOWN first so the QoS
// arbiter never sees min > max during the transition,
// then clamp max_req down to min_freq.
            freq_qos_update_request(&tenebrion_min_req[cpu],
                                    policy->cpuinfo.min_freq);
            freq_qos_update_request(&tenebrion_max_req[cpu],
                                    policy->cpuinfo.min_freq);
        }
        cpufreq_cpu_put(policy);
    }
}

// CPUFreq — restore via QoS

static void tenebrion_restore_freq(void)
{
    unsigned int cpu;
    struct cpufreq_policy *policy;

    for_each_online_cpu(cpu) {
        policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;

        if (policy->cpu == cpu && qos_initialized[cpu]) {
            // Order matters: raise the max_req ceiling first, then
// restore min_req floor.  Reversing this would momentarily
// set min > max on the QoS arbiter.
            freq_qos_update_request(&tenebrion_max_req[cpu],
                                    policy->cpuinfo.max_freq);
            freq_qos_update_request(&tenebrion_min_req[cpu],
                                    policy->cpuinfo.min_freq);
        }
        cpufreq_cpu_put(policy);
    }
}

// QoS cleanup

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

// Screen-off / screen-on actions                                       
// Both cpuset restriction and freq throttle happen together.

static void tenebrion_on_screen_off(void)
{
    if (!tenebrion_enabled) return;

    tenebrion_set_min_freq();
    tenebrion_cpuset_restrict();
    is_screen_off = true;
}

static void tenebrion_on_screen_on(void)
{
    tenebrion_restore_freq();
    tenebrion_cpuset_restore();
    is_screen_off = false;
}

// FB Notifier

static int tenebrion_fb_notifier_callback(struct notifier_block *self,
					 unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (event != FB_EVENT_BLANK)
		return 0;

	blank = evdata->data;

	mutex_lock(&tenebrion_lock);

	if (*blank == FB_BLANK_UNBLANK) {
		if (is_screen_off)
			tenebrion_on_screen_on();
	} else if (*blank == FB_BLANK_POWERDOWN) {
		if (!is_screen_off && tenebrion_enabled)
			tenebrion_on_screen_off();
	}

	mutex_unlock(&tenebrion_lock);
	return 0;
}

static struct notifier_block tenebrion_fb_notif = {
	.notifier_call = tenebrion_fb_notifier_callback,
};

static struct delayed_work tenebrion_init_work;

static void tenebrion_init_worker(struct work_struct *work)
{
    pr_info("tenebrion: late init started\n");
    tenebrion_qos_init();
    fb_register_client(&tenebrion_fb_notif);
    pr_info("tenebrion: fb_notifier registered. Screen state hooks active.\n");
}

// Init / Exit

static int __init tenebrion_init(void)
{
    memset(qos_initialized, 0, sizeof(qos_initialized));

    INIT_DELAYED_WORK(&tenebrion_init_work, tenebrion_init_worker);
    schedule_delayed_work(&tenebrion_init_work, msecs_to_jiffies(40000));

    pr_info("tenebrion: active — using fb_notifier\n");
    return 0;
}

static void __exit tenebrion_exit(void)
{
    cancel_delayed_work_sync(&tenebrion_init_work);
    fb_unregister_client(&tenebrion_fb_notif);

    if (is_screen_off) {
        mutex_lock(&tenebrion_lock);
        tenebrion_on_screen_on();
        mutex_unlock(&tenebrion_lock);
    }

    tenebrion_qos_cleanup();

    pr_info("tenebrion: unloaded\n");
}

module_init(tenebrion_init);
module_exit(tenebrion_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Tenebrion: Screen state based CPU frequency throttler + cpuset limiter");