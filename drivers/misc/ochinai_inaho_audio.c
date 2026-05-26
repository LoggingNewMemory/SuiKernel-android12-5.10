// ochinai_inaho_audio.c
// SPDX-License-Identifier: GPL-2.0-only
// Ochinai Inaho Audio — SCHED_FIFO boost + PM QoS + Raco CPUSet API Integration
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/pm_qos.h>
#include <linux/cpu.h>
#include <linux/rcupdate.h>
#include <linux/string.h>      
#include <uapi/linux/sched/types.h>
#include <linux/raco_override.h>  /* Raco Core API Header */

#define ENGAGE_DELAY_MS     20000
#define AUDIO_SCAN_MS       5000
#define PM_QOS_LATENCY_US   100   /* Max CPU latency — prevents deep idle */

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

static bool inaho_enabled = true;
module_param(inaho_enabled, bool, 0644);
MODULE_PARM_DESC(inaho_enabled, "Enable Ochinai Inaho Audio (default: true)");

/* PM QoS request — prevents CPU deep idle during audio */
static struct pm_qos_request inaho_pm_qos;
static bool pm_qos_active = false;

static struct task_struct *inaho_thread;

/* Control variable registered to Raco API */
static int raco_cpuset_trigger = 0; 
static char dynamic_cpu_mask[16];

static const char * const audio_threads[] = {
    "audioserver",
    "AudioOut",
    "AudioIn",
    "FastMixer",
    "FastCapture",
    "AudioFlinger",
    "AudioTrack",
    "AudioRecord",
    "audio.r_submix",
    "usb_audio_wq",
    NULL
};

/* Internal helper to safely write cpuset configs to the Virtual File System */
static int inaho_write_file(const char *path, const char *buf)
{
    struct file *f;
    loff_t pos = 0;
    int ret;

    f = filp_open(path, O_WRONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    ret = kernel_write(f, buf, strlen(buf), &pos);
    filp_close(f, NULL);

    return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* Dynamic CPUSet Allocator (Supports Any Core Count / Infinite Cores)*/
/* ------------------------------------------------------------------ */
static void inaho_execute_cpuset_override(void)
{
    int total_cores = num_possible_cpus(); /* Dynamically fetch true hardware core count */
    
    /* Format string dynamically: e.g., 8 cores -> "0-7", 12 cores -> "0-11" */
    snprintf(dynamic_cpu_mask, sizeof(dynamic_cpu_mask), "0-%d", total_cores - 1);

    pr_info("inaho: Dynamic core range detected: %s\n", dynamic_cpu_mask);

    /* Enforce maximum performance allocations across all Android cpusets */
    if (inaho_write_file("/dev/cpuset/foreground/cpus", dynamic_cpu_mask) == 0)
        pr_info("inaho: foreground cpuset locked to %s\n", dynamic_cpu_mask);

    if (inaho_write_file("/dev/cpuset/top-app/cpus", dynamic_cpu_mask) == 0)
        pr_info("inaho: top-app cpuset locked to %s\n", dynamic_cpu_mask);

    if (inaho_write_file("/dev/cpuset/boost-app/cpus", dynamic_cpu_mask) == 0)
        pr_info("inaho: boost-app cpuset locked to %s\n", dynamic_cpu_mask);
}

/* ------------------------------------------------------------------ */
/* Feature 1 — SCHED_FIFO boost for critical audio threads            */
/* ------------------------------------------------------------------ */
static void inaho_boost_audio_threads(void)
{
    struct task_struct *p;
    struct sched_param param = { .sched_priority = 2 };
    int boosted = 0;
    int i;

    rcu_read_lock();
    for_each_process(p) {
        for (i = 0; audio_threads[i]; i++) {
            if (strncmp(p->comm, audio_threads[i], TASK_COMM_LEN) == 0) {
                if (!rt_task(p)) {
                    sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
                    pr_info("inaho: boosted %s (pid %d) → SCHED_FIFO\n", p->comm, p->pid);
                    boosted++;
                }
                break;
            }
        }
    }
    rcu_read_unlock();

    // Only log to dmesg if we actually boosted new active threads!
    if (boosted > 0) {
        pr_info("inaho: %d audio threads boosted to SCHED_FIFO\n", boosted);
    }
}

static void inaho_pm_qos_engage(void)
{
    if (pm_qos_active)
        return;

    cpu_latency_qos_add_request(&inaho_pm_qos, PM_QOS_LATENCY_US);
    pm_qos_active = true;
    pr_info("inaho: PM QoS latency locked to %dus\n", PM_QOS_LATENCY_US);
}

static int inaho_worker(void *data)
{
    pr_info("inaho: standing by — engaging in %dms\n", ENGAGE_DELAY_MS);
    msleep(ENGAGE_DELAY_MS);

    inaho_pm_qos_engage();

    /* Initial enforcement right after boot completion delay */
    inaho_execute_cpuset_override();

    while (!kthread_should_stop()) {
        inaho_boost_audio_threads();
        
        if (raco_cpuset_trigger == 1) {
            pr_info("inaho: Raco Core pulse detected! Re-enforcing CPUSet guards.\n");
            inaho_execute_cpuset_override();
            raco_cpuset_trigger = 0;
        } else {
            /* Emergency validation watchdog check */
            struct file *f = filp_open("/dev/cpuset/foreground/cpus", O_RDONLY, 0);
            if (!IS_ERR(f)) {
                char current_mask[16] = {0};
                char *cleaned_mask;
                loff_t pos = 0;
                
                kernel_read(f, current_mask, sizeof(current_mask) - 1, &pos);
                filp_close(f, NULL);
                
                /* FIX: Clean trailing whitespaces/newlines using kernel's native strim() */
                cleaned_mask = strim(current_mask);
                
                if (strstr(cleaned_mask, dynamic_cpu_mask) == NULL) {
                    pr_info("inaho: Detected sneaky vendor rollback on foreground (%s)! Re-allocating all cores.\n", cleaned_mask);
                    inaho_execute_cpuset_override();
                }
            }
        }

        msleep_interruptible(AUDIO_SCAN_MS);
    }

    return 0;
}

static int __init inaho_audio_enhance_init(void)
{
    if (!inaho_enabled) {
        pr_info("inaho: disabled\n");
        return 0;
    }

    /* Register to Raco API */
    if (raco_register_rc_override(&raco_cpuset_trigger, 1, "audio.cpuset_lock") == 0) {
        pr_info("inaho: CPUSet automation hooked to Raco Global Sniper API\n");
    }

    inaho_thread = kthread_run(inaho_worker, NULL, "inaho_audio");
    if (IS_ERR(inaho_thread)) {
        pr_err("inaho: failed to start thread: %ld\n", PTR_ERR(inaho_thread));
        return PTR_ERR(inaho_thread);
    }

    pr_info("inaho: active\n");
    return 0;
}

static void __exit inaho_audio_enhance_exit(void)
{
    if (inaho_thread)
        kthread_stop(inaho_thread);

    if (pm_qos_active) {
        cpu_latency_qos_remove_request(&inaho_pm_qos);
        pm_qos_active = false;
    }

    pr_info("inaho: unloaded\n");
}

module_init(inaho_audio_enhance_init);
module_exit(inaho_audio_enhance_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("OCHINAI INAHO AUDIO — Raco API + Infinite Cores Support");