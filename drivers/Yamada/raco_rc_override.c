// SPDX-License-Identifier: GPL-3.0-only
/*
 * drivers/misc/raco_rc_override.c
 * Raco Override — Universal .rc Override API
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/raco_override.h>

#define RACO_SCAN_MS    5000   /* How often the sniper polls (ms)       */
#define RACO_MAX_RETRIES  24   /* ~120 s of active guarding after boot   */

struct raco_target {
	raco_enforce_cb_t cb;
	const char     *name;
	unsigned long   expires;
	struct list_head list;
};

static LIST_HEAD(raco_target_list);
static DEFINE_MUTEX(raco_list_lock);         /* guards list + raco_thread */
static struct task_struct *raco_thread;

/* ------------------------------------------------------------------ */

static int raco_sniper_thread(void *data)
{
	struct raco_target *entry;
	int active;

	pr_info("raco_override: Sniper deployed dynamically\n");

	while (!kthread_should_stop()) {
		msleep_interruptible(RACO_SCAN_MS);

		if (kthread_should_stop())
			break;

		active = 0;
		mutex_lock(&raco_list_lock);
		list_for_each_entry(entry, &raco_target_list, list) {
			if (time_before(jiffies, entry->expires)) {
				active++;
				if (entry->cb) {
					/* Punch vendor init.rc! Execute the callback unconditionally */
					entry->cb();
				}
			}
		}

		if (active == 0) {
			raco_thread = NULL;   /* must clear before releasing lock */
			mutex_unlock(&raco_list_lock);
			break;
		}
		mutex_unlock(&raco_list_lock);
	}

	pr_info("raco_override: Sniper mission complete, thread stopped.\n");
	return 0;
}

/* ------------------------------------------------------------------ */

/**
 * raco_register_rc_override - Register an atomic_t to be held at a
 *                              fixed value against init.rc interference.
 */
int raco_register_rc_override(raco_enforce_cb_t enforce_cb, const char *name)
{
	struct raco_target *new_target;

	if (!enforce_cb)
		return -EINVAL;

	new_target = kmalloc(sizeof(*new_target), GFP_KERNEL);
	if (!new_target) {
		pr_err("raco_override: Failed to allocate memory for '%s'\n", name);
		return -ENOMEM;
	}

	new_target->cb             = enforce_cb;
	new_target->name           = name;
	new_target->expires        = jiffies + msecs_to_jiffies(RACO_SCAN_MS * RACO_MAX_RETRIES);

	mutex_lock(&raco_list_lock);
	list_add_tail(&new_target->list, &raco_target_list);
	pr_info("raco_override: Registered '%s' for active enforcement\n", name);

	if (!raco_thread) {
		raco_thread = kthread_run(raco_sniper_thread, NULL, "raco_sniper");
		if (IS_ERR(raco_thread)) {
			pr_err("raco_override: Failed to deploy Raco Sniper\n");
			raco_thread = NULL;
		} else {
			pr_info("raco_override: Sniper deployed dynamically\n");
		}
	}
	mutex_unlock(&raco_list_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(raco_register_rc_override);

/* ------------------------------------------------------------------ */

/**
 * raco_unregister_rc_override - Remove a target from the Sniper's watch list.
 *
 * Safe to call even after the Sniper thread has self-terminated (the list
 * will simply be empty and -ENOENT is returned, which callers can ignore).
 */
int raco_unregister_rc_override(raco_enforce_cb_t enforce_cb)
{
	struct raco_target *entry, *tmp;

	mutex_lock(&raco_list_lock);
	list_for_each_entry_safe(entry, tmp, &raco_target_list, list) {
		if (entry->cb == enforce_cb) {
			list_del(&entry->list);
			kfree(entry);
			pr_info("raco_override: Unregistered target for cb %px\n",
				enforce_cb);
			mutex_unlock(&raco_list_lock);
			return 0;
		}
	}
	mutex_unlock(&raco_list_lock);

	pr_warn("raco_override: Unregister called but target cb %px not found\n",
		enforce_cb);
	return -ENOENT;
}
EXPORT_SYMBOL_GPL(raco_unregister_rc_override);

/* ------------------------------------------------------------------ */

static int __init raco_override_init(void)
{
	pr_info("raco_override: Framework initialized, waiting for registrations.\n");
	return 0;
}
late_initcall(raco_override_init);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Raco Universal RC Override");