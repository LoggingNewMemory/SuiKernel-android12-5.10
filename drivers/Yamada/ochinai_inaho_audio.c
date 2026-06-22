// SPDX-License-Identifier: GPL-3.0-only
/*
 * ochinai_inaho_audio.c
 * Ochinai Inaho Audio — SCHED_FIFO boost + PM QoS + Raco CPUSet API
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/pm_qos.h>
#include <linux/cpu.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/pid.h>
#include <uapi/linux/sched/types.h>

#define ENGAGE_DELAY_MS     20000
#define AUDIO_SCAN_MS        5000
#define PM_QOS_LATENCY_US     100   /* Max CPU latency — prevents deep idle */
#define MAX_AUDIO_PIDS         64   /* Upper bound for PID collection array  */

/* ------------------------------------------------------------------ */
/* Module parameter                                                     */
/* ------------------------------------------------------------------ */

static bool inaho_enabled = true;
module_param(inaho_enabled, bool, 0644);
MODULE_PARM_DESC(inaho_enabled, "Enable Ochinai Inaho Audio (default: true)");

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static struct pm_qos_request inaho_pm_qos;
static bool pm_qos_active;

static struct task_struct *inaho_thread;

/*
 * Watchdog thread settings
 */
#define AUDIO_SCAN_MS 5000

/* ------------------------------------------------------------------ */
/* Audio thread name table                                              */
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
/* Feature 1 — SCHED_FIFO boost for audio threads (RCU-safe)          */
/* ------------------------------------------------------------------ */

static void inaho_boost_audio_threads(void)
{
	struct task_struct *p;
	struct sched_param param = { .sched_priority = 2 };
	pid_t pids[MAX_AUDIO_PIDS];
	int count = 0;
	int boosted = 0;
	int i, j;

	/*
	 * Phase 1 — collect matching PIDs under RCU.
	 * We must NOT call sched_setscheduler_nocheck() here; it acquires
	 * pi_lock + rq->lock which violates the RCU read-side contract.
	 */
	rcu_read_lock();
	for_each_process(p) {
		if (count >= MAX_AUDIO_PIDS)
			break;
		for (i = 0; audio_threads[i]; i++) {
			if (strncmp(p->comm, audio_threads[i],
				    TASK_COMM_LEN) == 0) {
				pids[count++] = p->pid;
				break;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * Phase 2 — apply scheduler change outside RCU.
	 * Re-look up the task by PID under a fresh RCU critical section and
	 * pin it with get_task_struct() so it cannot be freed between the
	 * lookup and the sched_setscheduler_nocheck() call.
	 */
	for (j = 0; j < count; j++) {
		rcu_read_lock();
		p = find_task_by_vpid(pids[j]);
		if (p)
			get_task_struct(p);
		rcu_read_unlock();

		if (!p)
			continue;

		if (!rt_task(p)) {
			sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
			pr_info("inaho: boosted %s (pid %d) -> SCHED_FIFO\n",
				p->comm, p->pid);
			boosted++;
		}

		put_task_struct(p);
	}

	if (boosted > 0)
		pr_info("inaho: %d audio thread(s) boosted to SCHED_FIFO\n",
			boosted);
}

/* ------------------------------------------------------------------ */
/* Feature 2 — PM QoS latency guard                                    */
/* ------------------------------------------------------------------ */

static void inaho_pm_qos_engage(void)
{
	if (pm_qos_active)
		return;

	cpu_latency_qos_add_request(&inaho_pm_qos, PM_QOS_LATENCY_US);
	pm_qos_active = true;
	pr_info("inaho: PM QoS latency locked to %d us\n", PM_QOS_LATENCY_US);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                        */
/* ------------------------------------------------------------------ */

static int inaho_worker(void *data)
{
	pr_info("inaho: standing by — engaging in %d ms\n", ENGAGE_DELAY_MS);
	msleep(ENGAGE_DELAY_MS);

	inaho_pm_qos_engage();

	while (!kthread_should_stop()) {
		inaho_boost_audio_threads();

		msleep_interruptible(AUDIO_SCAN_MS);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init inaho_audio_enhance_init(void)
{
	if (!inaho_enabled) {
		pr_info("inaho: disabled via module param\n");
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

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Ochinai Inaho Audio");