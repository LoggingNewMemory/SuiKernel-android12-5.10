// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/raco_override.h>

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

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

/* Enlarged to 32 bytes — handles ≥ 10-core SoCs safely */
static char dynamic_cpu_mask[32];

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
/* VFS helper                                                           */
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

/* ------------------------------------------------------------------ */
/* Feature 1 — Dynamic CPUSet override                                 */
/* ------------------------------------------------------------------ */

static void inaho_execute_cpuset_override(void)
{
	int total_cores = num_possible_cpus();

	snprintf(dynamic_cpu_mask, sizeof(dynamic_cpu_mask),
		 "0-%d", total_cores - 1);

	pr_info("inaho: Dynamic core range: %s\n", dynamic_cpu_mask);

	if (inaho_write_file("/dev/cpuset/foreground/cpus", dynamic_cpu_mask) == 0)
		pr_info("inaho: foreground cpuset -> %s\n", dynamic_cpu_mask);

	if (inaho_write_file("/dev/cpuset/top-app/cpus", dynamic_cpu_mask) == 0)
		pr_info("inaho: top-app cpuset -> %s\n", dynamic_cpu_mask);

	if (inaho_write_file("/dev/cpuset/boost-app/cpus", dynamic_cpu_mask) == 0)
		pr_info("inaho: boost-app cpuset -> %s\n", dynamic_cpu_mask);
}

/* ------------------------------------------------------------------ */
/* Feature 2 — SCHED_FIFO boost for audio threads (RCU-safe)          */
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
/* Feature 3 — PM QoS latency guard                                    */
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
	inaho_execute_cpuset_override();

	while (!kthread_should_stop()) {
		inaho_boost_audio_threads();

		/*
		 * Watchdog: read the live cpuset value and re-enforce
		 * if a vendor service rolled it back without triggering
		 * Raco (e.g. direct sysfs write after Raco's window).
		 */
		{
			struct file *f = filp_open("/dev/cpuset/foreground/cpus",
						   O_RDONLY, 0);
			if (!IS_ERR(f)) {
				char current_mask[32] = {0};
				char *cleaned;
				loff_t pos = 0;

				kernel_read(f, current_mask,
					    sizeof(current_mask) - 1, &pos);
				filp_close(f, NULL);

				cleaned = strim(current_mask);
				if (strstr(cleaned, dynamic_cpu_mask) == NULL) {
					pr_info("inaho: Watchdog caught vendor rollback ('%s')! Re-enforcing.\n",
						cleaned);
					inaho_execute_cpuset_override();
				}
			}
		}

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

	if (raco_register_rc_override(inaho_execute_cpuset_override, "audio.cpuset_lock") == 0)
		pr_info("inaho: CPUSet guard hooked to Raco Global Sniper\n");
	else
		pr_warn("inaho: Raco hook failed, continuing without it\n");

	inaho_thread = kthread_run(inaho_worker, NULL, "inaho_audio");
	if (IS_ERR(inaho_thread)) {
		pr_err("inaho: failed to start thread: %ld\n",
		       PTR_ERR(inaho_thread));
		raco_unregister_rc_override(inaho_execute_cpuset_override);
		return PTR_ERR(inaho_thread);
	}

	pr_info("inaho: active\n");
	return 0;
}

static void __exit inaho_audio_enhance_exit(void)
{
	if (inaho_thread)
		kthread_stop(inaho_thread);

	/*
	 * Unregister AFTER stopping the thread so the worker cannot race
	 * with Raco's sniper during teardown.
	 */
	raco_unregister_rc_override(inaho_execute_cpuset_override);

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
MODULE_DESCRIPTION("Ochinai Inaho Audio — Raco API + Infinite Cores Support");