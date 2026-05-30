// SPDX-License-Identifier: GPL-2.0-only
/*
 * schedutil_enforcer.c
 * Enforce schedutil as the CPUFreq governor against vendor init.rc overrides.
 * Author: Kanagawa Yamada
 *
 * Built-in only (obj-y). Not safe as a loadable module — cpufreq_set_policy(),
 * cpufreq_start_governor(), and cpufreq_default_governor() are not exported
 * and cannot be resolved by the module loader.
 *
 * Strategy — one-shot CPUFREQ_CREATE_POLICY notifier:
 *
 *   CPUFREQ_CREATE_POLICY fires inside cpufreq_online() after the policy
 *   struct is fully initialised but before cpufreq_set_policy() is called
 *   for the first time.  At this point policy->governor holds whatever the
 *   driver or default selected.  We overwrite it with schedutil and mark
 *   that policy lead as done.
 *
 *   CPUFREQ_CREATE_POLICY does NOT fire on subsequent sysfs writes to
 *   scaling_governor — those go directly through cpufreq_set_policy().
 *   This means once all policy leads have been seen the notifier has
 *   nothing left to do and we unregister it, leaving all future governor
 *   changes (userspace, thermal, etc.) completely unobstructed.
 *
 *   Unregistration from inside the notifier callback is deferred to a
 *   work item to avoid deadlocking on the notifier chain's rwsem.
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

/*
 * Bitmask of policy-lead CPUs already enforced.
 * Allocated in __init, never written after all leads are done.
 */
static cpumask_var_t enforced_leads;

/*
 * Number of policy leads on this SoC (e.g. 2 on mt6833: policy0 + policy4).
 * Set at init by counting leads while cpufreq_cpu_get() returns non-NULL.
 * Zero means cpufreq isn't up yet — handle gracefully below.
 */
static unsigned int total_leads __initdata;

/* ------------------------------------------------------------------ */
/* Deferred self-unregistration                                        */
/* ------------------------------------------------------------------ */

static struct notifier_block enforcer_policy_nb;

static void enforcer_unregister_work(struct work_struct *work)
{
	cpufreq_unregister_notifier(&enforcer_policy_nb,
				    CPUFREQ_POLICY_NOTIFIER);
	raco_unregister_rc_override(&raco_governor_trigger);
	free_cpumask_var(enforced_leads);
	pr_info("schedutil_enforcer: Boot enforcement complete. "
		"Notifier unregistered — userspace governor writes unblocked.\n");
}

static DECLARE_WORK(enforcer_unregister, enforcer_unregister_work);

/* ------------------------------------------------------------------ */
/* Policy notifier                                                      */
/* ------------------------------------------------------------------ */

static int enforcer_policy_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	struct cpufreq_governor *gov;
	unsigned int cpu;

	if (event != CPUFREQ_CREATE_POLICY)
		return NOTIFY_DONE;

	if (!policy)
		return NOTIFY_DONE;

	cpu = policy->cpu;

	/* Already handled this lead — pass through untouched */
	if (cpumask_test_cpu(cpu, enforced_leads))
		return NOTIFY_DONE;

	cpumask_set_cpu(cpu, enforced_leads);

	/*
	 * Resolve schedutil each time rather than caching the pointer.
	 * cpufreq_default_governor() is a direct symbol call (built-in only)
	 * and always returns the compiled-in default — schedutil on any
	 * Android GKI kernel with CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y.
	 */
	gov = cpufreq_default_governor();
	if (!gov || strncmp(gov->name, "schedutil", CPUFREQ_NAME_LEN) != 0) {
		pr_warn("schedutil_enforcer: cpufreq_default_governor() returned '%s', "
			"expected schedutil — skipping policy%u\n",
			gov ? gov->name : "<null>", cpu);
		goto check_done;
	}

	if (policy->governor != gov) {
		pr_info("schedutil_enforcer: policy%u '%s' -> schedutil (boot enforcement)\n",
			cpu,
			policy->governor ? policy->governor->name : "<none>");
		policy->governor = gov;
	} else {
		pr_info("schedutil_enforcer: policy%u already on schedutil\n", cpu);
	}

check_done:
	/*
	 * Once every known lead has been seen, schedule self-unregistration.
	 * If total_leads was 0 at init (cpufreq not up yet), we stay
	 * registered until module unload rather than unregistering blindly.
	 */
	if (total_leads > 0 &&
	    cpumask_weight(enforced_leads) >= total_leads) {
		pr_info("schedutil_enforcer: All %u lead(s) enforced, "
			"scheduling notifier removal.\n", total_leads);
		schedule_work(&enforcer_unregister);
	}

	return NOTIFY_OK;
}

static struct notifier_block enforcer_policy_nb = {
	.notifier_call = enforcer_policy_notifier,
	/*
	 * Run before any vendor notifier. Vendor notifiers that try to
	 * restore sugov_ext are typically registered at default priority 0.
	 */
	.priority      = INT_MAX,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init schedutil_enforcer_init(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	int ret;

	if (!zalloc_cpumask_var(&enforced_leads, GFP_KERNEL)) {
		pr_err("schedutil_enforcer: Failed to allocate enforced_leads mask\n");
		return -ENOMEM;
	}

	/* Count policy leads. On mt6833: policy0 (cpu0) + policy4 (cpu4) = 2 */
	total_leads = 0;
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy->cpu == cpu)
			total_leads++;
		cpufreq_cpu_put(policy);
	}

	if (total_leads == 0)
		pr_warn("schedutil_enforcer: No cpufreq policies visible at init — "
			"notifier will stay live until all leads are seen.\n");
	else
		pr_info("schedutil_enforcer: Found %u policy lead(s)\n", total_leads);

	ret = raco_register_rc_override(&raco_governor_trigger, 0,
					"schedutil.governor_lock");
	if (ret == 0)
		pr_info("schedutil_enforcer: Hooked into Raco Core API\n");
	else
		pr_warn("schedutil_enforcer: Raco hook failed (%d), continuing\n", ret);

	ret = cpufreq_register_notifier(&enforcer_policy_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("schedutil_enforcer: Failed to register notifier (%d)\n", ret);
		free_cpumask_var(enforced_leads);
		raco_unregister_rc_override(&raco_governor_trigger);
		return ret;
	}

	pr_info("schedutil_enforcer: Armed — will self-unregister after "
		"%u lead(s) enforced.\n", total_leads);
	return 0;
}

/*
 * __exit is kept for completeness but on obj-y kernels this is only
 * reachable if the kernel is built with MODULE_UNLOAD support and the
 * user explicitly removes the driver — effectively never in production.
 */
static void __exit schedutil_enforcer_exit(void)
{
	cancel_work_sync(&enforcer_unregister);
	cpufreq_unregister_notifier(&enforcer_policy_nb,
				    CPUFREQ_POLICY_NOTIFIER);
	raco_unregister_rc_override(&raco_governor_trigger);
	free_cpumask_var(enforced_leads);
	pr_info("schedutil_enforcer: Unloaded.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("One-shot schedutil boot enforcement via cpufreq policy notifier — obj-y only");