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

bool inaho_enabled = true;

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static struct pm_qos_request inaho_pm_qos;
static bool pm_qos_active;

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

static void inaho_boost_task(pid_t pid)
{
	struct task_struct *p;
	struct sched_param param = { .sched_priority = 2 };

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (p)
		get_task_struct(p);
	rcu_read_unlock();

	if (!p)
		return;

	if (!rt_task(p)) {
		sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
		pr_info("inaho: upgraded thread %s (PID %d) to SCHED_FIFO\n", p->comm, pid);
	}

	put_task_struct(p);
}

/* 
 * Async work to apply scheduler changes safely outside of atomic contexts
 */
struct inaho_boost_work {
	struct work_struct work;
	pid_t pid;
};

static void inaho_boost_work_func(struct work_struct *work)
{
	struct inaho_boost_work *bw = container_of(work, struct inaho_boost_work, work);
	inaho_boost_task(bw->pid);
	kfree(bw);
}

static void inaho_queue_boost(pid_t pid)
{
	struct inaho_boost_work *bw = kmalloc(sizeof(*bw), GFP_ATOMIC);
	if (bw) {
		bw->pid = pid;
		INIT_WORK(&bw->work, inaho_boost_work_func);
		schedule_work(&bw->work);
	}
}

/* ------------------------------------------------------------------ */
/* Kprobe on __set_task_comm                                            */
/* ------------------------------------------------------------------ */

#include <linux/kprobes.h>

static int inaho_set_task_comm_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* ARM64 calling convention: arg0 is x0, arg1 is x1 */
	struct task_struct *tsk = (struct task_struct *)regs->regs[0];
	const char *buf = (const char *)regs->regs[1];
	int i;

	if (!inaho_enabled || !tsk || !buf)
		return 0;

	for (i = 0; audio_threads[i]; i++) {
		if (strncmp(buf, audio_threads[i], TASK_COMM_LEN) == 0) {
			inaho_queue_boost(tsk->pid);
			break;
		}
	}
	return 0;
}

static struct kprobe inaho_kprobe = {
	.symbol_name = "__set_task_comm",
	.pre_handler = inaho_set_task_comm_pre,
};

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
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init inaho_audio_enhance_init(void)
{
	int ret;

	if (!inaho_enabled) {
		pr_info("inaho: disabled via module param\n");
		return 0;
	}

	inaho_pm_qos_engage();

	ret = register_kprobe(&inaho_kprobe);
	if (ret < 0) {
		pr_err("inaho: failed to register kprobe: %d\n", ret);
		return ret;
	}

	pr_info("inaho: active — kprobes hooked\n");
	return 0;
}

static void __exit inaho_audio_enhance_exit(void)
{
	unregister_kprobe(&inaho_kprobe);

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