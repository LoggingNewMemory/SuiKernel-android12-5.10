// SPDX-License-Identifier: GPL-2.0-only
/*
 * schedutil_enforcer.c
 * Enforce schedutil as the CPUFreq governor against vendor init.rc overrides.
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/cpufreq.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/raco_override.h>

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

/* How long to wait after boot before first enforcement pass (ms).
 * Vendor services on mt6833 finish deploying around 20-25 s.          */
#define ENFORCER_DELAY_MS  25000

/* How often the background thread re-checks governors (ms).           */
#define ENFORCER_SCAN_MS    5000

static struct task_struct *enforcer_thread;
static struct workqueue_struct *enforcer_wq;

/*
 * Raco pulse trigger — atomic_t, shared between enforcer_worker and the
 * Raco Sniper kthread.  Registered at desired_val = 0 (armed/idle).
 * The Sniper resets it to 0 if anything flips it; we use atomic_cmpxchg
 * in the worker to catch the 0->1 transitions that signal a vendor write.
 *
 * Note on the desired_val = 0 registration: Raco guards the shadow at 0,
 * meaning it resets it back to 0 if anything changes it.  The enforcer
 * worker uses atomic_cmpxchg(trigger, 1, 0) to catch any brief excursion
 * to 1 before Raco resets it — this is intentional; the window is wide
 * enough at ENFORCER_SCAN_MS granularity.  If you need a guaranteed-latch
 * pulse mechanism, use a separate unguarded atomic and flip it manually
 * from a notifier.  For this use case (cpufreq governor watchdog) the
 * current approach is sufficient.
 */
static atomic_t raco_governor_trigger = ATOMIC_INIT(0);

/* ------------------------------------------------------------------ */
/* VFS write helper — same pattern as inaho, already proven working    */
/* ------------------------------------------------------------------ */

static int enforcer_write_file(const char *path, const char *buf)
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
/* Core enforcement                                                     */
/* ------------------------------------------------------------------ */

/*
 * enforce_schedutil_governor - Walk every cpufreq policy and switch any
 * non-schedutil governor to schedutil via the sysfs write path.
 *
 * WHY sysfs write and not cpufreq_set_policy() / store_scaling_governor():
 *
 *   - cpufreq_set_policy() expects a fully populated cpufreq_policy with
 *     a resolved governor pointer.  Shallow-copying the live policy and
 *     patching the name field corrupts the live struct (both share the
 *     same governor pointer).
 *
 *   - store_scaling_governor() is not guaranteed to be exported as a GKI
 *     stable symbol on all 5.10 vendor trees.
 *
 *   - Writing to scaling_governor via kernel_write() is the exact code
 *     path used by userspace "echo schedutil > scaling_governor" — it
 *     calls cpufreq_set_policy() internally with a correctly resolved
 *     governor pointer, runs the full stop/exit -> init/start transition
 *     under the right locks, and requires no unexported symbols.
 *
 * MUST be called from a sleepable context (workqueue or kthread).
 * MUST NOT hold policy->rwsem — the sysfs path acquires it internally.
 */
static void enforce_schedutil_governor(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	char path[64];
	int ret;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* Skip CPUs already running schedutil */
		if (policy->governor &&
		    strncmp(policy->governor->name, "schedutil",
			    CPUFREQ_NAME_LEN) == 0) {
			cpufreq_cpu_put(policy);
			continue;
		}

		pr_info("schedutil_enforcer: CPU%u running '%s', switching to schedutil\n",
			cpu,
			policy->governor ? policy->governor->name : "<none>");

		/*
		 * Release the reference before the sysfs write.
		 * The write path re-acquires the policy internally via
		 * cpufreq_cpu_get().  Holding it here causes a refcount
		 * imbalance on some kernel configs.
		 */
		cpufreq_cpu_put(policy);

		/*
		 * Write "schedutil" to the per-policy sysfs node.
		 * This is the same path used by init.rc and adb shell.
		 * It resolves the governor by name, calls the old governor's
		 * ->stop() and ->exit(), then the new governor's ->init()
		 * and ->start(), all under policy->rwsem.
		 */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor",
			 cpu);

		ret = enforcer_write_file(path, "schedutil");
		if (ret)
			pr_warn("schedutil_enforcer: Failed to switch CPU%u governor (err %d)\n",
				cpu, ret);
		else
			pr_info("schedutil_enforcer: CPU%u governor -> schedutil\n", cpu);
	}
}

/* ------------------------------------------------------------------ */
/* Deferred first-run on private workqueue                             */
/* ------------------------------------------------------------------ */

static void enforcer_first_run_work(struct work_struct *work)
{
	pr_info("schedutil_enforcer: First enforcement pass starting.\n");
	enforce_schedutil_governor();
}

static DECLARE_DELAYED_WORK(enforcer_first_run, enforcer_first_run_work);

/* ------------------------------------------------------------------ */
/* Background monitoring thread                                         */
/* ------------------------------------------------------------------ */

static int enforcer_worker(void *data)
{
	pr_info("schedutil_enforcer: Worker active, scanning every %d ms\n",
		ENFORCER_SCAN_MS);

	while (!kthread_should_stop()) {
		msleep_interruptible(ENFORCER_SCAN_MS);

		if (kthread_should_stop())
			break;

		if (atomic_cmpxchg(&raco_governor_trigger, 1, 0) == 1) {
			pr_info("schedutil_enforcer: Raco pulse — vendor governor override caught! Re-enforcing.\n");
			enforce_schedutil_governor();
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init schedutil_enforcer_init(void)
{
	int ret;

	/* Register with Raco. desired_val = 0 keeps the trigger in the
	 * armed/idle state; the enforcer worker catches any excursion to 1. */
	ret = raco_register_rc_override(&raco_governor_trigger, 0,
					"schedutil.governor_lock");
	if (ret == 0)
		pr_info("schedutil_enforcer: Hooked into Raco Core API\n");
	else
		pr_warn("schedutil_enforcer: Raco hook failed (%d), continuing without it\n",
			ret);

	/*
	 * Private single-thread workqueue — avoids the system_wq deadlock
	 * that can occur when cpufreq notifiers re-enter system_wq while
	 * our enforcement work is still running on it.
	 */
	enforcer_wq = create_singlethread_workqueue("sugov_enforcer_wq");
	if (!enforcer_wq) {
		pr_err("schedutil_enforcer: Failed to create workqueue\n");
		raco_unregister_rc_override(&raco_governor_trigger);
		return -ENOMEM;
	}

	queue_delayed_work(enforcer_wq, &enforcer_first_run,
			   msecs_to_jiffies(ENFORCER_DELAY_MS));
	pr_info("schedutil_enforcer: First pass scheduled in %d ms\n",
		ENFORCER_DELAY_MS);

	/* Start the background monitoring thread */
	enforcer_thread = kthread_run(enforcer_worker, NULL, "sugov_enforcer");
	if (IS_ERR(enforcer_thread)) {
		pr_err("schedutil_enforcer: Failed to start background worker\n");
		cancel_delayed_work_sync(&enforcer_first_run);
		destroy_workqueue(enforcer_wq);
		raco_unregister_rc_override(&raco_governor_trigger);
		return PTR_ERR(enforcer_thread);
	}

	pr_info("schedutil_enforcer: Module armed and active.\n");
	return 0;
}

static void __exit schedutil_enforcer_exit(void)
{
	/* Stop the monitor thread first so it cannot queue new work */
	if (enforcer_thread)
		kthread_stop(enforcer_thread);

	/* Drain and destroy the private workqueue */
	cancel_delayed_work_sync(&enforcer_first_run);
	destroy_workqueue(enforcer_wq);

	/* Unregister from Raco AFTER all threads are stopped — prevents the
	 * Sniper from dereferencing raco_governor_trigger after it is freed */
	raco_unregister_rc_override(&raco_governor_trigger);

	pr_info("schedutil_enforcer: Module unloaded cleanly.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Enforce schedutil as default CPUFreq governor");