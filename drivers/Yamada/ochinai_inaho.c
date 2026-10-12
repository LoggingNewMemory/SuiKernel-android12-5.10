// SPDX-License-Identifier: GPL-3.0-only
// ochinai_inaho.c
// Ochinai Inaho — SCHED_FIFO boost + PM QoS
// Author: Kanagawa Yamada

#include <linux/kprobes.h>
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
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/pid.h>
#include <uapi/linux/sched/types.h>

#define ENGAGE_DELAY_MS     20000
#define AUDIO_SCAN_MS        5000
#define PM_QOS_LATENCY_US     100
#define MAX_AUDIO_PIDS         64

bool inaho_enabled = true;

static struct pm_qos_request inaho_pm_qos;
static bool pm_qos_active;

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

	if (ret > 0) {
		if (buf[ret - 1] == '\n')
			buf[ret - 1] = '\0';
		else
			buf[ret] = '\0';
	} else {
		ret = -1;
	}

	return ret;
}

static void inaho_pm_qos_engage(void)
{
	if (pm_qos_active)
		return;

	cpu_latency_qos_add_request(&inaho_pm_qos, PM_QOS_LATENCY_US);
	pm_qos_active = true;
}

void inaho_set_enabled(bool enabled)
{
	inaho_enabled = enabled;
	if (enabled) {
		inaho_pm_qos_engage();
	} else if (pm_qos_active) {
		cpu_latency_qos_remove_request(&inaho_pm_qos);
		pm_qos_active = false;
	}
}
EXPORT_SYMBOL(inaho_set_enabled);

static struct delayed_work inaho_init_work;

static void inaho_init_worker(struct work_struct *work)
{
	char cfg[16];

	if (inaho_read_file("/data/SuiKernel/inaho", cfg, sizeof(cfg)) > 0) {
		if (cfg[0] == '0') {
			inaho_set_enabled(false);
			return;
		} else if (cfg[0] == '1') {
			inaho_set_enabled(true);
		}
	}
}

// Audio threads
static const char * const audio_threads[] = {
	"audioserver",
	"AudioOut",
	"AudioIn",
	"FastMixer",
	"FastCapture",
	NULL
};

// SCHED_FIFO boost for audio threads
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
	}

	put_task_struct(p);
}

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

static int inaho_set_task_comm_pre(struct kprobe *p, struct pt_regs *regs)
{
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

	INIT_DELAYED_WORK(&inaho_init_work, inaho_init_worker);
	schedule_delayed_work(&inaho_init_work, msecs_to_jiffies(15000));

	pr_info("inaho: active — kprobes hooked\n");
	return 0;
}

static void __exit inaho_audio_enhance_exit(void)
{
	cancel_delayed_work_sync(&inaho_init_work);
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
MODULE_DESCRIPTION("Ochinai Inaho");