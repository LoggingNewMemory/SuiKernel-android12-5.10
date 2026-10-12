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
#include <linux/fs.h>

#define BOOST_DURATION_MS   100

bool yamada_touch_boost_enabled = true;

int yamada_touch_boost_duration = BOOST_DURATION_MS;

extern bool yamada_is_boosted;
static DEFINE_SPINLOCK(boost_lock);

static struct delayed_work boost_off_work;

static int yamada_touch_boost_read_file(const char *path, char *buf, size_t size)
{
	struct file *f;
	loff_t pos = 0;
	int ret;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -1;

	ret = kernel_read(f, buf, size - 1, &pos);
	filp_close(f, NULL);

	if (ret > 0) {
		if (buf[ret - 1] == '\n')
			buf[ret - 1] = '\0';
		else
			buf[ret] = '\0';
	} else {
		ret = -1;
	}

	return ret;
}

void yamada_touch_boost_set_mode(int mode)
{
	if (mode == 0) {
		yamada_touch_boost_enabled = false;
		yamada_touch_boost_off();
	} else if (mode == 2) {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = 300;
	} else {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = 100;
	}
}
EXPORT_SYMBOL(yamada_touch_boost_set_mode);

static struct delayed_work touch_boost_init_work;

static void touch_boost_init_worker(struct work_struct *work)
{
	char cfg[16];

	if (yamada_touch_boost_read_file("/data/SuiKernel/touch_boost", cfg, sizeof(cfg)) > 0) {
		if (cfg[0] == '0') {
			yamada_touch_boost_set_mode(0);
		} else if (cfg[0] == '2') {
			yamada_touch_boost_set_mode(2);
		} else if (cfg[0] == '1') {
			yamada_touch_boost_set_mode(1);
		}
	}
}

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

}

void yamada_touch_boost_off(void) {
	cancel_delayed_work_sync(&boost_off_work);
	do_boost_off(NULL);
}
EXPORT_SYMBOL(yamada_touch_boost_off);


static void kobo_trigger_boost(void) {
	unsigned long flags;

	if (!yamada_touch_boost_enabled) 
		return;

	spin_lock_irqsave(&boost_lock, flags);
	if (!yamada_is_boosted) {
		WRITE_ONCE(yamada_is_boosted, true);
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	/* Refresh the delayed work timer on every touch event */
	mod_delayed_work(system_wq, &boost_off_work, msecs_to_jiffies(yamada_touch_boost_duration));
}

static int __init yamada_touch_boost_init(void) {
	INIT_DELAYED_WORK(&boost_off_work, do_boost_off);
	INIT_DELAYED_WORK(&touch_boost_init_work, touch_boost_init_worker);
	schedule_delayed_work(&touch_boost_init_work, msecs_to_jiffies(15000));

	yamada_boost_hook = kobo_trigger_boost;

	pr_info("yamada_touch_boost: Active (Direct Schedutil Mode)\n");
	return 0;
}

static void __exit yamada_touch_boost_exit(void) {
	yamada_boost_hook = NULL;

	cancel_delayed_work_sync(&touch_boost_init_work);
	cancel_delayed_work_sync(&boost_off_work);

	pr_info("yamada_touch_boost: Unloaded\n");
}

module_init(yamada_touch_boost_init);
module_exit(yamada_touch_boost_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Touch Boost — Schedutil Direct Hook Edition");