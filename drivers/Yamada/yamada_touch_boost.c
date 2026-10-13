// SPDX-License-Identifier: GPL-3.0-only
// Yamada Touch Boost — Schedutil Hook Edition
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/suikernel.h>
#include "IOCTL_Library.h"

/* Hook from drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

/* Direct schedutil boost flag from kernel/sched/cpufreq_schedutil.c */
extern bool yamada_is_boosted;

#define BOOST_DURATION_BALANCED_MS 100
#define BOOST_DURATION_GAMING_MS   300

bool yamada_touch_boost_enabled = true;
int yamada_touch_boost_duration = BOOST_DURATION_BALANCED_MS;

static struct delayed_work boost_off_work;
static struct delayed_work touch_boost_init_work;

static void yamada_boost_off_work(struct work_struct *work)
{
	WRITE_ONCE(yamada_is_boosted, false);
}

void yamada_touch_boost_off(void)
{
	cancel_delayed_work_sync(&boost_off_work);
	WRITE_ONCE(yamada_is_boosted, false);
}
EXPORT_SYMBOL(yamada_touch_boost_off);

static void yamada_touch_boost_trigger(void)
{
	WRITE_ONCE(yamada_is_boosted, true);
	mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(yamada_touch_boost_duration));
}

void yamada_touch_boost_set_mode(int mode)
{
	if (mode == 0) {
		yamada_touch_boost_enabled = false;
		WRITE_ONCE(yamada_boost_hook, NULL);
		yamada_touch_boost_off();
		pr_info("yamada_touch_boost: disabled\n");
	} else if (mode == 2) {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = BOOST_DURATION_GAMING_MS;
		WRITE_ONCE(yamada_boost_hook, yamada_touch_boost_trigger);
		pr_info("yamada_touch_boost: gaming mode (300ms)\n");
	} else {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = BOOST_DURATION_BALANCED_MS;
		WRITE_ONCE(yamada_boost_hook, yamada_touch_boost_trigger);
		pr_info("yamada_touch_boost: balanced mode (100ms)\n");
	}
}
EXPORT_SYMBOL(yamada_touch_boost_set_mode);

static void touch_boost_init_worker(struct work_struct *work)
{
	int mode;

	// Mandatory: /data must be checked
	mode = sui_get_touch_boost_mode();
	yamada_touch_boost_set_mode(mode);
}

static int __init yamada_touch_boost_init(void)
{
	INIT_DELAYED_WORK(&boost_off_work, yamada_boost_off_work);
	INIT_DELAYED_WORK(&touch_boost_init_work, touch_boost_init_worker);

	// Default to balanced mode until /data config is evaluated
	yamada_touch_boost_set_mode(1);

	schedule_delayed_work(&touch_boost_init_work, msecs_to_jiffies(15000));
	pr_info("yamada_touch_boost: initialized (awaiting /data config)\n");
	return 0;
}

static void __exit yamada_touch_boost_exit(void)
{
	WRITE_ONCE(yamada_boost_hook, NULL);
	cancel_delayed_work_sync(&touch_boost_init_work);
	yamada_touch_boost_off();
	pr_info("yamada_touch_boost: unloaded\n");
}

module_init(yamada_touch_boost_init);
module_exit(yamada_touch_boost_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Touch Boost — Schedutil Direct Hook Edition");