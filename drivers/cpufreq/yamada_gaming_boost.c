// yamada_gaming_boost.c
// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — CPU Input Boost + Schedutil Rate Limit Tuning
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#define BOOST_DURATION_MS   500

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);
MODULE_PARM_DESC(yamada_boost_enabled, "Enable Yamada Gaming Boost (default: true)");

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);
MODULE_PARM_DESC(boost_duration_ms, "Boost duration in ms after last input (default: 500)");

/* ------------------------------------------------------------------ */
/* Copied from kernel/sched/cpufreq_schedutil.c — not exported         */
/*                                                                      */
/* WARNING: must stay in sync with the compiled kernel's layout.       */
/* A field inserted before rate_limit_us or tunables in the upstream   */
/* structs will cause silent memory corruption at runtime.             */
/* Verified against android12-5.10 cpufreq_schedutil.c                */
/* ------------------------------------------------------------------ */

struct sugov_tunables {
	struct gov_attr_set     attr_set;       /* MUST remain first member */
	unsigned int            rate_limit_us;
};

struct sugov_policy {
	struct cpufreq_policy   *policy;        /* MUST remain first member */
	struct sugov_tunables   *tunables;
	struct list_head         tunables_hook;

	raw_spinlock_t           update_lock;
	u64                      last_freq_update_time;
	s64                      freq_update_delay_ns;  /* derived: rate_limit_us * NSEC_PER_USEC */
	/* remaining fields intentionally omitted */
};

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool boost_active = false;

/*
 * Use a spinlock, not a mutex.  yamada_input_event() is called from
 * the input core which can run in softirq context on some drivers.
 * Mutexes may sleep; spinlocks are safe in all contexts.
 */
static DEFINE_SPINLOCK(boost_lock);

/*
 * boost_generation is incremented every time a new boost cycle begins.
 * do_boost_off() captures the current generation when it is scheduled
 * and aborts if the generation has advanced by the time it executes,
 * meaning a new boost_on raced ahead of it.  This prevents the
 * "CPUFreq locked at max" bug described in the header comment above.
 */
static atomic_t boost_generation = ATOMIC_INIT(0);

/*
 * Wrapper for the delayed off-work so it can carry the generation that
 * was current when the work was last (re-)scheduled.
 */
struct yamada_boost_off_work {
	struct delayed_work     dwork;
	unsigned int            generation;
};
static struct yamada_boost_off_work boost_off_work;

/*
 * Dedicated work item for boost-on.  do_boost_on() calls cpufreq_update_policy()
 * which acquires a mutex internally — it must never run from the input event
 * path (which can be softirq context).  schedule_work() is safe from any context.
 */
static struct work_struct boost_on_work;

static struct input_handler yamada_input_handler;

/*
 * Per-policy saved values, keyed by policy->cpu (the representative CPU of
 * each cluster).  We save both scaling_min_freq and rate_limit_us so we
 * restore exactly what was set before the boost, not just the hardware floor.
 */
static unsigned int saved_scaling_min_freq[NR_CPUS];
static unsigned int saved_rate_limit_us[NR_CPUS];

/* ------------------------------------------------------------------ */
/* Schedutil rate_limit_us helper                                       */
/* ------------------------------------------------------------------ */

static void set_schedutil_rate_limit(struct cpufreq_policy *policy,
				     unsigned int rate_limit_us)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	if (!sg_policy || !sg_policy->tunables)
		return;

	sg_policy->tunables->rate_limit_us = rate_limit_us;
	WRITE_ONCE(sg_policy->freq_update_delay_ns,
		   (s64)rate_limit_us * NSEC_PER_USEC);
}

/* ------------------------------------------------------------------ */
/* scaling_min_freq writer                                              */
/*                                                                      */
/* cpufreq_update_policy() re-reads policy->user_policy and calls the  */
/* driver's ->setpolicy / notifies the governor.  We write directly to */
/* policy->user_policy.min before calling it so the governor picks up  */
/* the new floor on the very next call.                                 */
/*                                                                      */
/* This is the same path used by the cpufreq sysfs                     */
/* /sys/devices/.../cpufreq/scaling_min_freq write handler.            */
/* ------------------------------------------------------------------ */

static void set_policy_min_freq(struct cpufreq_policy *policy,
				unsigned int min_freq)
{
	policy->user_policy.min = min_freq;
	cpufreq_update_policy(policy->cpu);
}

/* ------------------------------------------------------------------ */
/* Boost ON — runs from boost_on_work (process context).               */
/* Never call directly from the input event handler.                   */
/* ------------------------------------------------------------------ */

static void do_boost_on(struct work_struct *work)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			struct sugov_policy *sg_policy = policy->governor_data;

			/*
			 * Save the *current* scaling_min_freq (userspace value),
			 * not cpuinfo.min_freq, so we restore exactly what was
			 * configured before the boost — important if the user or
			 * another driver had set a custom floor.
			 *
			 * Guard against overwriting with 0: if for any reason
			 * policy->min is already 0 (shouldn't happen, but be
			 * defensive), keep the previously saved value.
			 */
			if (policy->min != 0)
				saved_scaling_min_freq[cpu] = policy->min;

			/*
			 * Save rate_limit_us only when non-zero to avoid
			 * clobbering the saved value if a previous boost cycle
			 * already set it to 0 and we haven't restored yet.
			 */
			if (sg_policy && sg_policy->tunables &&
			    sg_policy->tunables->rate_limit_us != 0)
				saved_rate_limit_us[cpu] =
					sg_policy->tunables->rate_limit_us;

			/*
			 * Raise the minimum frequency floor to max_freq.
			 * The governor (schedutil) is NOT bypassed — it
			 * remains in full control and can still scale above
			 * this value.  We are only setting the floor, not
			 * capping or forcing a fixed frequency.
			 */
			set_policy_min_freq(policy, policy->cpuinfo.max_freq);

			/* Set schedutil rate_limit_us to 0 for instant response */
			set_schedutil_rate_limit(policy, 0);
		}

		cpufreq_cpu_put(policy);
	}

	pr_debug("yamada_gaming_boost: boost on\n");
}

/* ------------------------------------------------------------------ */
/* Boost OFF — runs from system_wq (process context), safe to sleep.  */
/*                                                                      */
/* Checks boost_generation before doing anything.  If the generation   */
/* has advanced since this work was queued it means a new boost cycle  */
/* started after us — bail out and let that cycle's off-work handle    */
/* the eventual restore.  This is the key fix for the lock-at-max bug. */
/* ------------------------------------------------------------------ */

static void do_boost_off(struct work_struct *work)
{
	struct yamada_boost_off_work *bwork =
		container_of(work, struct yamada_boost_off_work, dwork.work);
	unsigned int cpu;
	unsigned long flags;

	/*
	 * Stale generation: a new boost cycle began after this work was
	 * queued.  do_boost_on() has already raised scaling_min_freq back
	 * to max and will schedule its own off-work.  If we ran the restore
	 * here we would lower the floor prematurely, defeating the new
	 * boost cycle entirely.  Abort.
	 */
	if (bwork->generation != (unsigned int)atomic_read(&boost_generation)) {
		pr_debug("yamada_gaming_boost: stale off-work (gen %u vs %u), skipping\n",
			 bwork->generation,
			 (unsigned int)atomic_read(&boost_generation));
		return;
	}

	spin_lock_irqsave(&boost_lock, flags);
	boost_active = false;
	spin_unlock_irqrestore(&boost_lock, flags);

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			struct sugov_policy *sg_policy = policy->governor_data;

			/*
			 * Restore scaling_min_freq to what it was before the
			 * boost.  If saved_scaling_min_freq[cpu] was never
			 * written (module loaded with boost already pending),
			 * fall back to cpuinfo.min_freq as a safe default.
			 */
			set_policy_min_freq(policy,
					    saved_scaling_min_freq[cpu]
					    ? saved_scaling_min_freq[cpu]
					    : policy->cpuinfo.min_freq);

			/* Restore schedutil rate_limit_us AND freq_update_delay_ns */
			if (sg_policy && sg_policy->tunables) {
				sg_policy->tunables->rate_limit_us =
					saved_rate_limit_us[cpu];
				WRITE_ONCE(sg_policy->freq_update_delay_ns,
					   (s64)saved_rate_limit_us[cpu] *
					   NSEC_PER_USEC);
			}
		}

		cpufreq_cpu_put(policy);
	}

	pr_debug("yamada_gaming_boost: boost off\n");
}

/* ------------------------------------------------------------------ */
/* Shared restore helper — used by yamada_gaming_boost_exit() when     */
/* boost is still active at unload time.  Mirrors do_boost_off() but   */
/* without the generation check (irrelevant at exit).                  */
/* ------------------------------------------------------------------ */

static void yamada_restore_all(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			struct sugov_policy *sg_policy = policy->governor_data;

			set_policy_min_freq(policy,
					    saved_scaling_min_freq[cpu]
					    ? saved_scaling_min_freq[cpu]
					    : policy->cpuinfo.min_freq);

			if (sg_policy && sg_policy->tunables) {
				sg_policy->tunables->rate_limit_us =
					saved_rate_limit_us[cpu];
				WRITE_ONCE(sg_policy->freq_update_delay_ns,
					   (s64)saved_rate_limit_us[cpu] *
					   NSEC_PER_USEC);
			}
		}

		cpufreq_cpu_put(policy);
	}
}

/* ------------------------------------------------------------------ */
/* Input handler                                                        */
/* ------------------------------------------------------------------ */

static void yamada_input_event(struct input_handle *handle,
			       unsigned int type,
			       unsigned int code,
			       int value)
{
	unsigned long flags;
	bool need_boost_on = false;
	unsigned int gen;

	if (type != EV_ABS && type != EV_KEY)
		return;

	if (!yamada_boost_enabled)
		return;

	spin_lock_irqsave(&boost_lock, flags);

	if (!boost_active) {
		/*
		 * Starting a new boost cycle.  Increment the generation
		 * counter INSIDE the spinlock so do_boost_off() — which
		 * also reads it — always sees a consistent value relative
		 * to boost_active.
		 */
		boost_active = true;
		gen = atomic_inc_return(&boost_generation);
		need_boost_on = true;
	} else {
		gen = (unsigned int)atomic_read(&boost_generation);
	}

	spin_unlock_irqrestore(&boost_lock, flags);

	if (need_boost_on) {
		/*
		 * Schedule do_boost_on() into process context.
		 * cpufreq_update_policy() acquires a mutex — must NOT be
		 * called directly here (potential softirq context).
		 * schedule_work() is safe from any context.
		 */
		schedule_work(&boost_on_work);
	}

	/*
	 * Reschedule the off-timer on every event to extend the boost
	 * window from the last touch.  Store the current generation in the
	 * work wrapper so do_boost_off() can detect if it has been lapped.
	 *
	 * mod_delayed_work() cancels any pending instance and re-queues,
	 * so this is safe to call on every EV_ABS/EV_KEY event during a
	 * gaming session without cascading timer storms.
	 */
	boost_off_work.generation = gen;
	mod_delayed_work(system_wq, &boost_off_work.dwork,
			 msecs_to_jiffies(boost_duration_ms));
}

static int yamada_input_connect(struct input_handler *handler,
				struct input_dev *dev,
				const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev     = dev;
	handle->handler = handler;
	handle->name    = "yamada_gaming_boost";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;

	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void yamada_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id yamada_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};
MODULE_DEVICE_TABLE(input, yamada_input_ids);

/* ------------------------------------------------------------------ */
/* Init / Exit                                                          */
/* ------------------------------------------------------------------ */

static int __init yamada_gaming_boost_init(void)
{
	int ret;

	if (!yamada_boost_enabled) {
		pr_info("yamada_gaming_boost: disabled\n");
		return 0;
	}

	memset(saved_scaling_min_freq, 0, sizeof(saved_scaling_min_freq));
	memset(saved_rate_limit_us, 0, sizeof(saved_rate_limit_us));

	INIT_WORK(&boost_on_work, do_boost_on);
	INIT_DELAYED_WORK(&boost_off_work.dwork, do_boost_off);
	boost_off_work.generation = 0;

	yamada_input_handler.event      = yamada_input_event;
	yamada_input_handler.connect    = yamada_input_connect;
	yamada_input_handler.disconnect = yamada_input_disconnect;
	yamada_input_handler.name       = "yamada_gaming_boost";
	yamada_input_handler.id_table   = yamada_input_ids;

	ret = input_register_handler(&yamada_input_handler);
	if (ret) {
		pr_err("yamada_gaming_boost: failed to register input handler: %d\n",
		       ret);
		return ret;
	}

	pr_info("yamada_gaming_boost: active — boost duration %dms\n",
		boost_duration_ms);
	return 0;
}

static void __exit yamada_gaming_boost_exit(void)
{
	input_unregister_handler(&yamada_input_handler);

	/*
	 * Drain both work items before touching any shared state.
	 * cancel_work_sync() ensures boost_on_work has finished if it was
	 * in flight at the moment unregister_handler ran.
	 * cancel_delayed_work_sync() ensures the off-work is not pending
	 * or currently executing.
	 */
	cancel_work_sync(&boost_on_work);
	cancel_delayed_work_sync(&boost_off_work.dwork);

	/*
	 * If boost is still active after draining, restore scaling_min_freq
	 * and rate_limit_us manually before unloading.
	 */
	if (boost_active) {
		unsigned long flags;

		spin_lock_irqsave(&boost_lock, flags);
		boost_active = false;
		spin_unlock_irqrestore(&boost_lock, flags);

		yamada_restore_all();
	}

	pr_info("yamada_gaming_boost: unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — CPU input boost via scaling_min_freq + schedutil rate limit tuning");