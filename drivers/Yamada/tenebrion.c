// tenebrion.c
// SPDX-License-Identifier: GPL-3.0-only
// Tenebrion — Screen state based CPU frequency throttler
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

#define DPMS_PATH           "/sys/class/drm/card0-DSI-1/dpms"
#define BACKLIGHT_PATH_1    "/sys/class/leds/lcd-backlight/brightness"
#define BACKLIGHT_PATH_2    "/sys/class/backlight/panel0-backlight/brightness"

#define POLL_INTERVAL_ON_MS  3000 // Slow poll when screen is ON
#define POLL_INTERVAL_OFF_MS 100  // Hair-trigger poll when screen is OFF

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

static void tenebrion_on_screen_off(void)
{
    if (!tenebrion_enabled) return;

    tenebrion_set_min_freq();
    is_screen_off = true;
}

static void tenebrion_on_screen_on(void)
{
    tenebrion_restore_freq();
    is_screen_off = false;
}

// Screen State Polling

static struct delayed_work tenebrion_poll_work;

static const char *active_backlight_path = NULL;

static void tenebrion_poll_worker(struct work_struct *work)
{
	char buf[16] = {0};
	bool currently_off = false;

	// 1. Primary check: Backlight brightness. "0" means screen off.
	if (active_backlight_path && tenebrion_read_file(active_backlight_path, buf, sizeof(buf)) > 0) {
		if (buf[0] == '0' && buf[1] == '\0') {
			currently_off = true;
		}
	}

	// 2. Secondary check: DPMS state. "Off" means screen off.
	if (tenebrion_read_file(DPMS_PATH, buf, sizeof(buf)) > 0) {
		if (strncmp(buf, "Off", 3) == 0) {
			currently_off = true;
		}
	}

	mutex_lock(&tenebrion_lock);
	if (currently_off && !is_screen_off) {
		tenebrion_on_screen_off();
	} else if (!currently_off && is_screen_off) {
		tenebrion_on_screen_on();
	}
	mutex_unlock(&tenebrion_lock);

	// Asymmetric Polling: If screen is off, check every 100ms for instant wake-up.
	// If screen is on, check every 3000ms because throttling delay on sleep doesn't matter.
	if (currently_off) {
		schedule_delayed_work(&tenebrion_poll_work, msecs_to_jiffies(POLL_INTERVAL_OFF_MS));
	} else {
		schedule_delayed_work(&tenebrion_poll_work, msecs_to_jiffies(POLL_INTERVAL_ON_MS));
	}
}

static struct delayed_work tenebrion_init_work;

static void tenebrion_init_worker(struct work_struct *work)
{
	struct file *f;
	int i;
	const char *backlight_paths[] = {
		BACKLIGHT_PATH_1,
		BACKLIGHT_PATH_2,
		NULL
	};

	pr_info("tenebrion: late init started\n");

	// Hardware detection: Find which backlight path exists
	for (i = 0; backlight_paths[i] != NULL; i++) {
		f = filp_open(backlight_paths[i], O_RDONLY, 0);
		if (!IS_ERR(f)) {
			filp_close(f, NULL);
			active_backlight_path = backlight_paths[i];
			break;
		}
	}

	if (!active_backlight_path) {
		pr_err("tenebrion: No valid BACKLIGHT path found! Aborting module execution.\n");
		return;
	}

	tenebrion_qos_init();
	
	INIT_DELAYED_WORK(&tenebrion_poll_work, tenebrion_poll_worker);
	schedule_delayed_work(&tenebrion_poll_work, msecs_to_jiffies(1000));
	pr_info("tenebrion: VFS polling registered. Screen state hooks active.\n");
}

// Init / Exit

static int __init tenebrion_init(void)
{
    memset(qos_initialized, 0, sizeof(qos_initialized));

    INIT_DELAYED_WORK(&tenebrion_init_work, tenebrion_init_worker);
    schedule_delayed_work(&tenebrion_init_work, msecs_to_jiffies(40000));

    pr_info("tenebrion: active — using VFS polling\n");
    return 0;
}

static void __exit tenebrion_exit(void)
{
    cancel_delayed_work_sync(&tenebrion_init_work);
    cancel_delayed_work_sync(&tenebrion_poll_work);

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
MODULE_DESCRIPTION("Tenebrion: Screen state based CPU frequency throttler");