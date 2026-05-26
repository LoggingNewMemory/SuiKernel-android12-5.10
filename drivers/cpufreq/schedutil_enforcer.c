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

#define ENFORCER_DELAY_MS  25000
#define ENFORCER_SCAN_MS    5000

static struct task_struct *enforcer_thread;
static struct workqueue_struct *enforcer_wq;

static atomic_t raco_governor_trigger = ATOMIC_INIT(0);

/* ------------------------------------------------------------------ */
/* VFS helpers                                                          */
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

static int enforcer_read_file(const char *path, char *buf, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	int ret;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	ret = kernel_read(f, buf, len - 1, &pos);
	filp_close(f, NULL);

	if (ret >= 0)
		buf[ret] = '\0';

	return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* Core enforcement                                                     */
/* ------------------------------------------------------------------ */

/*
 * enforce_schedutil_governor - Switch every policy-lead CPU that is not
 * already on schedutil to schedutil via the sysfs write path.
 *
 * Policy-lead loop: cpufreq_cpu_get(cpu) returns the policy whose .cpu
 * field is the lead CPU for that cluster.  We skip any cpu where
 * policy->cpu != cpu to avoid writing to non-existent sysfs nodes
 * (e.g. policy1, policy2 don't exist on mt6833 — only policy0 and
 * policy4 do).
 *
 * MUST be called from a sleepable context.
 * MUST NOT hold policy->rwsem.
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

		/*
		 * Skip non-lead CPUs.  On mt6833, cpufreq_cpu_get(1) returns
		 * the policy for cluster 0 whose .cpu = 0.  Writing to
		 * policy1/scaling_governor would hit -ENOENT because that
		 * sysfs node does not exist.
		 */
		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		/* Already on schedutil — nothing to do */
		if (policy->governor &&
		    strncmp(policy->governor->name, "schedutil",
			    CPUFREQ_NAME_LEN) == 0) {
			cpufreq_cpu_put(policy);
			continue;
		}

		pr_info("schedutil_enforcer: policy%u running '%s', switching to schedutil\n",
			cpu,
			policy->governor ? policy->governor->name : "<none>");

		/* Release ref before the sysfs write — the write path
		 * re-acquires the policy internally.                         */
		cpufreq_cpu_put(policy);

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor",
			 cpu);

		ret = enforcer_write_file(path, "schedutil");
		if (ret)
			pr_warn("schedutil_enforcer: Failed to switch policy%u (err %d)\n",
				cpu, ret);
		else
			pr_info("schedutil_enforcer: policy%u -> schedutil\n", cpu);
	}
}

/* ------------------------------------------------------------------ */
/* Readback watchdog                                                    */
/* ------------------------------------------------------------------ */

/*
 * check_governors_and_enforce - Read back the live scaling_governor for
 * every policy lead and re-enforce if any has drifted away from schedutil.
 *
 * This catches init.cgroup.rc writes that happen after Raco's active
 * window, direct sysfs writes from vendor daemons, and any other path
 * that bypasses the Raco pulse mechanism entirely.
 */
static void check_governors_and_enforce(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	char path[64];
	char current_gov[CPUFREQ_NAME_LEN + 4]; /* +4 for possible newline + nul */
	char *trimmed;
	bool needs_enforce = false;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}
		cpufreq_cpu_put(policy);

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor",
			 cpu);

		memset(current_gov, 0, sizeof(current_gov));
		if (enforcer_read_file(path, current_gov, sizeof(current_gov)) < 0)
			continue;

		trimmed = strim(current_gov);
		if (strncmp(trimmed, "schedutil", CPUFREQ_NAME_LEN) != 0) {
			pr_info("schedutil_enforcer: Watchdog — policy%u drifted to '%s'! Re-enforcing.\n",
				cpu, trimmed);
			needs_enforce = true;
			break; /* One bad policy is enough signal — enforce all */
		}
	}

	if (needs_enforce)
		enforce_schedutil_governor();
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

		/*
		 * Primary detection: readback watchdog.
		 * Reads the live scaling_governor sysfs node every scan cycle
		 * and re-enforces if any policy lead has drifted.  This catches
		 * init.cgroup.rc and vendor daemon writes that the Raco pulse
		 * mechanism has no visibility into.
		 */
		check_governors_and_enforce();

		/*
		 * Secondary: Raco pulse path — kept for symmetry with other
		 * modules in this suite, but enforcement no longer depends on
		 * it since nothing external writes 1 to raco_governor_trigger.
		 */
		if (atomic_cmpxchg(&raco_governor_trigger, 1, 0) == 1) {
			pr_info("schedutil_enforcer: Raco pulse received, running extra enforcement pass.\n");
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

	ret = raco_register_rc_override(&raco_governor_trigger, 0,
					"schedutil.governor_lock");
	if (ret == 0)
		pr_info("schedutil_enforcer: Hooked into Raco Core API\n");
	else
		pr_warn("schedutil_enforcer: Raco hook failed (%d), continuing without it\n",
			ret);

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
	if (enforcer_thread)
		kthread_stop(enforcer_thread);

	cancel_delayed_work_sync(&enforcer_first_run);
	destroy_workqueue(enforcer_wq);

	raco_unregister_rc_override(&raco_governor_trigger);

	pr_info("schedutil_enforcer: Module unloaded cleanly.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Enforce schedutil as default CPUFreq governor");