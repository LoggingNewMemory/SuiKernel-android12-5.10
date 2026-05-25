// ochinai_inaho_audio.c
// SPDX-License-Identifier: GPL-2.0-only
// Ochinai Inaho Audio — SCHED_FIFO boost + PM QoS + CPUSet tuning
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

/* ------------------------------------------------------------------ */
/* Audio process names to boost                                         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* File helpers                                                         */
/* ------------------------------------------------------------------ */

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

static int inaho_read_file(const char *path, char *buf, size_t size)
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
/* Feature 1 — SCHED_FIFO boost for audio threads                      */
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
            if (strncmp(p->comm, audio_threads[i],
                        TASK_COMM_LEN) == 0) {
                /* Only boost if not already RT */
                if (!rt_task(p)) {
                    sched_setscheduler_nocheck(p,
                        SCHED_FIFO, &param);
                    pr_info("inaho: boosted %s (pid %d) → SCHED_FIFO\n",
                            p->comm, p->pid);
                    boosted++;
                }
                break;
            }
        }
    }
    rcu_read_unlock();

    pr_info("inaho: %d audio threads boosted to SCHED_FIFO\n", boosted);
}

/* ------------------------------------------------------------------ */
/* Feature 2 — PM QoS latency lock                                     */
/* ------------------------------------------------------------------ */

static void inaho_pm_qos_engage(void)
{
    if (pm_qos_active)
        return;

    pm_qos_add_request(&inaho_pm_qos,
                       PM_QOS_CPU_DMA_LATENCY,
                       PM_QOS_LATENCY_US);
    pm_qos_active = true;
    pr_info("inaho: PM QoS latency locked to %dus\n", PM_QOS_LATENCY_US);
}

/* ------------------------------------------------------------------ */
/* Feature 3 — CPUSet tuning                                           */
/* ------------------------------------------------------------------ */

static void inaho_cpuset_tune(void)
{
    char buf[32];
    int ret;

    /* Expand foreground cpuset to include cpu7 (big core) */
    ret = inaho_read_file("/dev/cpuset/foreground/cpus", buf, sizeof(buf));
    if (ret > 0) {
        if (strstr(buf, "0-7")) {
            pr_info("inaho: foreground cpuset already 0-7\n");
        } else {
            ret = inaho_write_file("/dev/cpuset/foreground/cpus", "0-7");
            if (ret < 0)
                pr_warn("inaho: foreground cpuset expand failed (%d)\n", ret);
            else
                pr_info("inaho: foreground cpuset expanded to 0-7\n");
        }
    }

    /* Expand top-app to all CPUs — already 0-7 on this device but
       write anyway to handle devices where it might be restricted */
    ret = inaho_read_file("/dev/cpuset/top-app/cpus", buf, sizeof(buf));
    if (ret > 0) {
        if (!strstr(buf, "0-7")) {
            ret = inaho_write_file("/dev/cpuset/top-app/cpus", "0-7");
            if (ret < 0)
                pr_warn("inaho: top-app cpuset expand failed (%d)\n", ret);
            else
                pr_info("inaho: top-app cpuset expanded to 0-7\n");
        } else {
            pr_info("inaho: top-app cpuset already 0-7\n");
        }
    }

    /* Expand boost-app to all CPUs */
    ret = inaho_read_file("/dev/cpuset/boost-app/cpus", buf, sizeof(buf));
    if (ret > 0) {
        if (!strstr(buf, "0-7")) {
            ret = inaho_write_file("/dev/cpuset/boost-app/cpus", "0-7");
            if (ret < 0)
                pr_warn("inaho: boost-app cpuset expand failed (%d)\n", ret);
            else
                pr_info("inaho: boost-app cpuset expanded to 0-7\n");
        } else {
            pr_info("inaho: boost-app cpuset already 0-7\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Periodic audio thread scanner                                        */
/* Rescans every AUDIO_SCAN_MS to catch newly spawned audio threads    */
/* ------------------------------------------------------------------ */

static int inaho_worker(void *data)
{
    pr_info("inaho: standing by — engaging in %dms\n", ENGAGE_DELAY_MS);

    /* Wait for SELinux + KernelSU rules */
    msleep(ENGAGE_DELAY_MS);

    /* One-shot: PM QoS + CPUSet */
    inaho_pm_qos_engage();
    inaho_cpuset_tune();

    /* Periodic: rescan audio threads */
    while (!kthread_should_stop()) {
        inaho_boost_audio_threads();
        msleep_interruptible(AUDIO_SCAN_MS);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init inaho_audio_enhance_init(void)
{
    if (!inaho_enabled) {
        pr_info("inaho: disabled\n");
        return 0;
    }

    inaho_thread = kthread_run(inaho_worker, NULL, "inaho_audio");
    if (IS_ERR(inaho_thread)) {
        pr_err("inaho: failed to start thread: %ld\n",
               PTR_ERR(inaho_thread));
        return PTR_ERR(inaho_thread);
    }

    pr_info("inaho: active\n");
    return 0;
}

static void __exit inaho_audio_enhance_exit(void)
{
    kthread_stop(inaho_thread);

    if (pm_qos_active) {
        pm_qos_remove_request(&inaho_pm_qos);
        pm_qos_active = false;
    }

    pr_info("inaho: unloaded\n");
}

module_init(inaho_audio_enhance_init);
module_exit(inaho_audio_enhance_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Inaho Audio Enhance — SCHED_FIFO + PM QoS + CPUSet tuning");