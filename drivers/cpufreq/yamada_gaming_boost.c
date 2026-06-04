// SPDX-License-Identifier: GPL-2.0-only
// Yamada Gaming Boost — Schedutil Vendor Hook Edition
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/input.h>
#include <trace/hooks/sched.h> // Required for Android Vendor Hooks

#define BOOST_DURATION_MS   1000

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);

static bool is_boosted = false;
static DEFINE_SPINLOCK(boost_lock);

static struct delayed_work boost_off_work;

/* Hook from drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

/**
 * yamada_vh_map_util_freq_probe - Intercepts schedutil's frequency mapping
 * 
 * This callback matches the prototype defined in trace/hooks/sched.h
 */
static void yamada_vh_map_util_freq_probe(void *data, unsigned long util,
					  unsigned long freq, unsigned long cap,
					  unsigned long *next_freq,
					  struct cpufreq_policy *policy,
					  bool *need_freq_update)
{
	unsigned long flags;
	bool active_boost;

	if (!yamada_boost_enabled)
		return;

	spin_lock_irqsave(&boost_lock, flags);
	active_boost = is_boosted;
	spin_unlock_irqrestore(&boost_lock, flags);

	/* If the screen is being touched, bypass normal math and force maxfreq instantly! */
	if (active_boost && policy && next_freq && need_freq_update) {
		*next_freq = policy->cpuinfo.max_freq;
		*need_freq_update = true;
	}
}

static void do_boost_off(struct work_struct *work) {
	unsigned long flags;

	spin_lock_irqsave(&boost_lock, flags);
	is_boosted = false;
	spin_unlock_irqrestore(&boost_lock, flags);

	pr_info("yamada_gaming_boost: touch boost OFF (Letting schedutil handle math again)\n");
}

static void kobo_trigger_boost(void) {
	unsigned long flags;

	if (!yamada_boost_enabled) 
		return;

	spin_lock_irqsave(&boost_lock, flags);
	if (!is_boosted) {
		is_boosted = true;
		pr_info("yamada_gaming_boost: touch boost ON (Vendor Hook Intercept Active)\n");
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	/* Refresh the delayed work timer on every touch event */
	mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(boost_duration_ms));
}

static int __init yamada_gaming_boost_init(void) {
	int ret;

	INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

	/* Register our probe to the Android Schedutil Vendor Hook */
	ret = register_trace_android_vh_map_util_freq(yamada_vh_map_util_freq_probe, NULL);
	if (ret) {
		pr_err("yamada_gaming_boost: Failed to register schedutil vendor hook probe!\n");
		return ret;
	}

	yamada_boost_hook = kobo_trigger_boost;

	pr_info("yamada_gaming_boost: Active (Vendor Hook Mode)\n");
	return 0;
}

static void __exit yamada_gaming_boost_exit(void) {
	yamada_boost_hook = NULL;

	/* Unregister the probe so we don't leave wild pointers in the scheduler */
	unregister_trace_android_vh_map_util_freq(yamada_vh_map_util_freq_probe, NULL);

	cancel_delayed_work_sync(&boost_off_work);

	pr_info("yamada_gaming_boost: Unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — Schedutil Vendor Hook Edition");