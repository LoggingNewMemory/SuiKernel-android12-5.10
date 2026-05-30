// SPDX-License-Identifier: GPL-2.0-only
/*
 * schedutil_enforcer.c
 * Enforce schedutil as the CPUFreq governor against vendor init.rc overrides.
 * Author: Kanagawa Yamada
 *
 * Strategy — one-shot policy notifier:
 *
 *   Register a CPUFREQ_POLICY_NOTIFIER at module init.  On every
 *   CPUFREQ_CREATE_POLICY event, overwrite policy->governor with schedutil
 *   and mark that policy lead as seen.  Once every policy lead CPU has been
 *   enforced exactly once, unregister the notifier from within the notifier
 *   callback itself (safe via atomic + schedule_work) and the module steps
 *   aside permanently — all future userspace governor writes go through
 *   unobstructed.
 *
 *   cpufreq_default_governor() / cpufreq_fallback_governor() are declared
 *   in cpufreq.h and resolved by the module loader at load time; schedutil
 *   is guaranteed present (the kernel won't build without it).
 *
 *   No kthreads.  No workqueue polling.  No sysfs I/O.  No SELinux rules.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/raco_override.h>

static atomic_t raco_governor_trigger = ATOMIC_INIT(0);

/* Cached pointer to the schedutil governor struct. Set once at init. */
static struct cpufreq_governor *schedutil_gov;

/*
 * Bitmask of policy-lead CPUs that have already been enforced.
 * Once a CPU's bit is set it is never enforced again, so any subsequent
 * governor write from userspace passes straight through.
 */
static cpumask_var_t enforced_leads;

/*
 * Total number of policy leads on this SoC.  Counted at init by walking
 * cpufreq policies.  When enforced_leads reaches this count the notifier
 * unregisters itself.
 */
static unsigned int total_leads;

/* ------------------------------------------------------------------ */
/* Deferred self-unregistration                                        */
/* ------------------------------------------------------------------ */

/*
 * Unregistering a notifier from inside its own callback is not safe to
 * do synchronously (the notifier chain holds a read lock at call time).
 * Schedule the unregister as a one-shot work item instead.
 */
static struct notifier_block enforcer_policy_nb; /* forward decl */

static void enforcer_unregister_work(struct work_struct *work)
{
	cpufreq_unregister_notifier(&enforcer_policy_nb,
				    CPUFREQ_POLICY_NOTIFIER);
	raco_unregister_rc_override(&raco_governor_trigger);
	pr_info("schedutil_enforcer: All %u policy leads enforced. "
		"Notifier unregistered — userspace governor writes unblocked.\n",
		total_leads);
}

static DECLARE_WORK(enforcer_unregister, enforcer_unregister_work);

/* ------------------------------------------------------------------ */
/* Policy notifier                                                      */
/* ------------------------------------------------------------------ */

static int enforcer_policy_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu;

	if (event != CPUFREQ_CREATE_POLICY)
		return NOTIFY_DONE;

	if (!policy || !schedutil_gov)
		return NOTIFY_DONE;

	cpu = policy->cpu;

	/* Already enforced this lead — let userspace writes through */
	if (cpumask_test_cpu(cpu, enforced_leads))
		return NOTIFY_DONE;

	/* Mark enforced before doing anything else */
	cpumask_set_cpu(cpu, enforced_leads);

	if (policy->governor != schedutil_gov) {
		pr_info("schedutil_enforcer: policy%u '%s' -> schedutil (boot enforcement)\n",
			cpu,
			policy->governor ? policy->governor->name : "<none>");
		policy->governor = schedutil_gov;
	} else {
		pr_info("schedutil_enforcer: policy%u already on schedutil\n", cpu);
	}

	/* If every lead has been seen, schedule self-unregistration */
	if (cpumask_weight(enforced_leads) >= total_leads) {
		pr_info("schedutil_enforcer: All policy leads enforced, scheduling unregister.\n");
		schedule_work(&enforcer_unregister);
	}

	return NOTIFY_OK;
}

static struct notifier_block enforcer_policy_nb = {
	.notifier_call = enforcer_policy_notifier,
	.priority      = INT_MAX,
};

/* ------------------------------------------------------------------ */
/* Count policy leads at init                                           */
/* ------------------------------------------------------------------ */

static unsigned int count_policy_leads(void)
{
	unsigned int cpu, count = 0;
	struct cpufreq_policy *policy;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy->cpu == cpu)
			count++;
		cpufreq_cpu_put(policy);
	}
	return count;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init schedutil_enforcer_init(void)
{
	int ret;

	if (!zalloc_cpumask_var(&enforced_leads, GFP_KERNEL)) {
		pr_err("schedutil_enforcer: Failed to allocate enforced_leads mask\n");
		return -ENOMEM;
	}

	schedutil_gov = cpufreq_default_governor();
	if (!schedutil_gov ||
	    strncmp(schedutil_gov->name, "schedutil", CPUFREQ_NAME_LEN) != 0) {
		pr_warn("schedutil_enforcer: default governor is not schedutil, trying fallback\n");
		schedutil_gov = cpufreq_fallback_governor();
	}

	if (!schedutil_gov ||
	    strncmp(schedutil_gov->name, "schedutil", CPUFREQ_NAME_LEN) != 0) {
		pr_err("schedutil_enforcer: Cannot locate schedutil governor struct — aborting\n");
		free_cpumask_var(enforced_leads);
		return -ENODEV;
	}

	pr_info("schedutil_enforcer: Located schedutil governor @ %px\n",
		schedutil_gov);

	/*
	 * Count leads now.  On mt6833 this is 2 (policy0 + policy4).
	 * If cpufreq isn't ready yet the count may be 0; in that case
	 * the notifier will self-unregister after the first CPUFREQ_CREATE_
	 * POLICY batch completes naturally — total_leads is updated below
	 * to at least 1 so the termination condition still fires correctly.
	 */
	total_leads = count_policy_leads();
	if (total_leads == 0) {
		pr_warn("schedutil_enforcer: No cpufreq policies online yet at init, "
			"will unregister after first enforcement batch.\n");
		/*
		 * Set to UINT_MAX so the "all leads seen" check never fires
		 * prematurely; we'll rely on the notifier being called for
		 * every CREATE_POLICY and unregister after a fixed count
		 * is not possible — leave notifier active until module unload
		 * in this edge case.  In practice on mt6833 policies are
		 * online well before module init runs.
		 */
		total_leads = UINT_MAX;
	}
	pr_info("schedutil_enforcer: Expecting %u policy lead(s)\n", total_leads);

	ret = raco_register_rc_override(&raco_governor_trigger, 0,
					"schedutil.governor_lock");
	if (ret == 0)
		pr_info("schedutil_enforcer: Hooked into Raco Core API\n");
	else
		pr_warn("schedutil_enforcer: Raco hook failed (%d), continuing without it\n",
			ret);

	ret = cpufreq_register_notifier(&enforcer_policy_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("schedutil_enforcer: Failed to register policy notifier (%d)\n",
		       ret);
		free_cpumask_var(enforced_leads);
		raco_unregister_rc_override(&raco_governor_trigger);
		return ret;
	}

	pr_info("schedutil_enforcer: Armed. Will self-unregister after all %u lead(s) enforced.\n",
		total_leads);
	return 0;
}

static void __exit schedutil_enforcer_exit(void)
{
	/*
	 * Cancel any pending unregister work first.  If it already ran,
	 * cancel_work_sync() is a no-op.  If the notifier is still live
	 * (edge case: module unloaded before all leads were seen), unregister
	 * it now.
	 */
	cancel_work_sync(&enforcer_unregister);
	cpufreq_unregister_notifier(&enforcer_policy_nb,
				    CPUFREQ_POLICY_NOTIFIER);
	raco_unregister_rc_override(&raco_governor_trigger);
	free_cpumask_var(enforced_leads);
	pr_info("schedutil_enforcer: Module unloaded cleanly.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("One-shot schedutil enforcement at boot via cpufreq policy notifier");