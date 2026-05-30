// SPDX-License-Identifier: GPL-2.0-only
/*
 * schedutil_enforcer.c
 * Enforce schedutil as the CPUFreq governor against vendor init.rc overrides.
 * Author: Kanagawa Yamada
 *
 * Design — why a policy notifier, not a kthread writing sysfs:
 *
 *   The exported cpufreq API available to out-of-tree modules on this kernel
 *   (per cpufreq.h + EXPORT_SYMBOL grep of drivers/cpufreq/cpufreq.c) is:
 *
 *     cpufreq_cpu_get / cpufreq_cpu_put        -- refcount, no rwsem
 *     cpufreq_cpu_acquire / cpufreq_cpu_release -- refcount + write rwsem
 *     cpufreq_get_policy                        -- read-only snapshot
 *     cpufreq_update_policy                     -- re-validates freq limits,
 *                                                  does NOT switch governors
 *     cpufreq_register_notifier /
 *     cpufreq_unregister_notifier               -- policy + transition hooks
 *
 *   cpufreq_set_policy() and cpufreq_start_governor() are NOT exported;
 *   they cannot be called from a module.  Writing sysfs via filp_open +
 *   kernel_write hits the LSM/SELinux path and fails with -EACCES on
 *   vendor kernels (the AOSP neverallow for kernel writing sysfs_devices_
 *   system_cpu survives ksu_allow injection).
 *
 *   The correct hook is CPUFREQ_POLICY_NOTIFIER / CPUFREQ_CREATE_POLICY:
 *   the core calls notifiers with write rwsem already held and the policy
 *   fully initialised but not yet started.  At that point we can legally
 *   overwrite policy->governor because the governor has not been started
 *   yet — no stop/start dance required.  cpufreq_start_governor() is then
 *   called by the core immediately after the notifier chain returns.
 *
 *   For the re-enforcement case (init.rc overrides an already-running
 *   policy), we register a CPUFREQ_POLICY_NOTIFIER as well: any time the
 *   vendor writes a new governor name through sysfs the core fires
 *   CPUFREQ_CREATE_POLICY again for that policy, which re-triggers our
 *   notifier before the new governor is started.
 *
 *   schedutil is guaranteed present — it is selected at compile time and
 *   the kernel will not build without it.  We obtain its struct pointer
 *   once at module init via cpufreq_default_governor() (which returns
 *   schedutil when CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y) or via
 *   cpufreq_fallback_governor() as a second option; both are declared in
 *   cpufreq.h.  Note that neither is EXPORT_SYMBOL — they are used at
 *   module init time from __init context where the symbol resolution is
 *   done by the module loader before the module's init function runs, so
 *   they are accessible as long as the kernel is built with
 *   CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y (which it must be).
 *
 *   If for any reason the governor pointer lookup fails at init we fall
 *   back to cpufreq_register_governor() probing — but on a kernel where
 *   schedutil is mandatory this path is never reached.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/raco_override.h>

static atomic_t raco_governor_trigger = ATOMIC_INIT(0);

/* Cached pointer to the schedutil governor struct.  Set once at init,
 * read-only afterwards — no locking needed.                           */
static struct cpufreq_governor *schedutil_gov;

/* ------------------------------------------------------------------ */
/* Policy notifier                                                      */
/* ------------------------------------------------------------------ */

/*
 * enforcer_policy_notifier - Called by the cpufreq core on every policy
 * lifecycle event with the policy's write rwsem already held.
 *
 * We act on CPUFREQ_CREATE_POLICY only.  At this point the policy is fully
 * initialised (governor field populated from the driver default or from
 * the last sysfs write) but cpufreq_start_governor() has NOT been called
 * yet.  Overwriting policy->governor here is therefore safe and correct:
 * the core will start whichever governor is in that field when control
 * returns from the notifier chain.
 *
 * This fires both at boot (initial policy creation) and any time a new
 * governor is written to scaling_governor — which is exactly the init.rc
 * override window we need to guard.
 */
static int enforcer_policy_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_CREATE_POLICY)
		return NOTIFY_DONE;

	if (!policy || !schedutil_gov)
		return NOTIFY_DONE;

	/* Already on schedutil — nothing to do */
	if (policy->governor == schedutil_gov)
		return NOTIFY_DONE;

	pr_info("schedutil_enforcer: policy%u governor '%s' -> schedutil (notifier)\n",
		policy->cpu,
		policy->governor ? policy->governor->name : "<none>");

	policy->governor = schedutil_gov;

	return NOTIFY_OK;
}

static struct notifier_block enforcer_policy_nb = {
	.notifier_call = enforcer_policy_notifier,
	/*
	 * Use a high priority so we run before any vendor notifier that might
	 * try to restore sugov_ext or another override governor.
	 */
	.priority = INT_MAX,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init schedutil_enforcer_init(void)
{
	int ret;

	/*
	 * Obtain the schedutil governor pointer.
	 *
	 * cpufreq_default_governor() returns the governor selected by
	 * CONFIG_CPU_FREQ_DEFAULT_GOV_* — schedutil on any sane Android
	 * GKI kernel.  cpufreq_fallback_governor() is the secondary option.
	 * Both are declared in cpufreq.h; schedutil is built-in so the
	 * symbol is always present.
	 */
	schedutil_gov = cpufreq_default_governor();
	if (!schedutil_gov ||
	    strncmp(schedutil_gov->name, "schedutil", CPUFREQ_NAME_LEN) != 0) {
		pr_warn("schedutil_enforcer: default governor is not schedutil, trying fallback\n");
		schedutil_gov = cpufreq_fallback_governor();
	}

	if (!schedutil_gov ||
	    strncmp(schedutil_gov->name, "schedutil", CPUFREQ_NAME_LEN) != 0) {
		pr_err("schedutil_enforcer: Cannot locate schedutil governor struct — aborting\n");
		return -ENODEV;
	}

	pr_info("schedutil_enforcer: Located schedutil governor @ %px\n",
		schedutil_gov);

	/* Register with Raco for symmetry; failure is non-fatal */
	ret = raco_register_rc_override(&raco_governor_trigger, 0,
					"schedutil.governor_lock");
	if (ret == 0)
		pr_info("schedutil_enforcer: Hooked into Raco Core API\n");
	else
		pr_warn("schedutil_enforcer: Raco hook failed (%d), continuing without it\n",
			ret);

	/*
	 * Register the policy notifier.  CPUFREQ_POLICY_NOTIFIER fires on
	 * CPUFREQ_CREATE_POLICY for every existing policy at registration
	 * time as well as for future events — so this single call both
	 * enforces schedutil on currently active policies and arms the guard
	 * for all future init.rc overrides.
	 */
	ret = cpufreq_register_notifier(&enforcer_policy_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("schedutil_enforcer: Failed to register policy notifier (%d)\n",
		       ret);
		raco_unregister_rc_override(&raco_governor_trigger);
		return ret;
	}

	pr_info("schedutil_enforcer: Policy notifier armed — schedutil enforced on all future governor changes.\n");
	return 0;
}

static void __exit schedutil_enforcer_exit(void)
{
	cpufreq_unregister_notifier(&enforcer_policy_nb,
				    CPUFREQ_POLICY_NOTIFIER);
	raco_unregister_rc_override(&raco_governor_trigger);
	pr_info("schedutil_enforcer: Module unloaded cleanly.\n");
}

module_init(schedutil_enforcer_init);
module_exit(schedutil_enforcer_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Enforce schedutil as default CPUFreq governor via policy notifier");