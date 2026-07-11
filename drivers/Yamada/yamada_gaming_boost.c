// SPDX-License-Identifier: GPL-3.0-only
// Yamada Gaming Boost — Schedutil Hook Edition
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/sched/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/input.h>

#define BOOST_DURATION_MS   100

static bool yamada_boost_enabled = true;
module_param(yamada_boost_enabled, bool, 0644);

static unsigned int boost_duration_ms = BOOST_DURATION_MS;
module_param(boost_duration_ms, uint, 0644);

extern bool yamada_is_boosted;
static DEFINE_SPINLOCK(boost_lock);

static struct delayed_work boost_off_work;

/* Hook from drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

static void do_boost_off(struct work_struct *work) {
	unsigned long flags;

	spin_lock_irqsave(&boost_lock, flags);
	WRITE_ONCE(yamada_is_boosted, false);
	spin_unlock_irqrestore(&boost_lock, flags);

	pr_info("yamada_gaming_boost: touch boost OFF\n");
}

static void kobo_trigger_boost(void) {
	unsigned long flags;

	if (!yamada_boost_enabled) 
		return;

	spin_lock_irqsave(&boost_lock, flags);
	if (!yamada_is_boosted) {
		WRITE_ONCE(yamada_is_boosted, true);
		pr_info("yamada_gaming_boost: touch boost ON\n");
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	/* Refresh the delayed work timer on every touch event */
	mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(boost_duration_ms));
}

static int __init yamada_gaming_boost_init(void) {
	INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

	yamada_boost_hook = kobo_trigger_boost;

	pr_info("yamada_gaming_boost: Active (Direct Schedutil Mode)\n");
	return 0;
}

static void __exit yamada_gaming_boost_exit(void) {
	yamada_boost_hook = NULL;

	cancel_delayed_work_sync(&boost_off_work);

	pr_info("yamada_gaming_boost: Unloaded\n");
}

module_init(yamada_gaming_boost_init);
module_exit(yamada_gaming_boost_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Gaming Boost — Schedutil Direct Hook Edition");