// SPDX-License-Identifier: GPL-3.0-only
// Yamada Touch Boost — Schedutil Hook Edition
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/sched/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/input.h>

#define BOOST_DURATION_MS   100

bool yamada_touch_boost_enabled = true;

int yamada_touch_boost_duration = BOOST_DURATION_MS;

extern bool yamada_is_boosted;
static DEFINE_SPINLOCK(boost_lock);

static struct delayed_work boost_off_work;

/* Hook from drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

static void do_boost_off(struct work_struct *work) {
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&boost_lock, flags);
	WRITE_ONCE(yamada_is_boosted, false);
	spin_unlock_irqrestore(&boost_lock, flags);

	for_each_online_cpu(cpu) {
		cpufreq_update_policy(cpu);
	}

	pr_info("yamada_touch_boost: touch boost OFF\n");
}

static void kobo_trigger_boost(void) {
	unsigned long flags;

	if (!yamada_touch_boost_enabled) 
		return;

	spin_lock_irqsave(&boost_lock, flags);
	if (!yamada_is_boosted) {
		WRITE_ONCE(yamada_is_boosted, true);
		pr_info("yamada_touch_boost: touch boost ON\n");
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	/* Refresh the delayed work timer on every touch event */
	mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(yamada_touch_boost_duration));
}

static int __init yamada_touch_boost_init(void) {
	INIT_DELAYED_WORK(&boost_off_work, do_boost_off);

	yamada_boost_hook = kobo_trigger_boost;

	pr_info("yamada_touch_boost: Active (Direct Schedutil Mode)\n");
	return 0;
}

static void __exit yamada_touch_boost_exit(void) {
	yamada_boost_hook = NULL;

	cancel_delayed_work_sync(&boost_off_work);

	pr_info("yamada_touch_boost: Unloaded\n");
}

module_init(yamada_touch_boost_init);
module_exit(yamada_touch_boost_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Touch Boost — Schedutil Direct Hook Edition");