// SPDX-License-Identifier: GPL-3.0-only
// drivers/misc/pavolia_reine_resetprop.c
// Pavolia Reine Setprop Engine - Usermodehelper Resetprop API
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pavolia_reine_resetprop.h>

#define POLL_INTERVAL_MS 5000
#define MAX_RETRIES      24 // 120s max wait time

struct prop_entry {
	char prop[128];
	char val[128];
	struct list_head list;
};

static LIST_HEAD(prop_list);
static DEFINE_MUTEX(prop_lock);
static void execute_resetprop_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(prop_work, execute_resetprop_work);
static int retry_count = 0;

static void execute_resetprop_work(struct work_struct *work)
{
	struct prop_entry *entry, *tmp;
	struct file *f;

	// 1. Actively check if ksud exists on the filesystem yet
	f = filp_open("/data/adb/ksud", O_RDONLY, 0);
	if (IS_ERR(f)) {
		if (PTR_ERR(f) == -ENOENT) {
			if (retry_count++ < MAX_RETRIES) {
				// /data is not mounted yet, or ksud is not ready. Try again in 5s.
				schedule_delayed_work(&prop_work, msecs_to_jiffies(POLL_INTERVAL_MS));
				return;
			}
			pr_err("pavolia_reine: /data/adb/ksud never appeared after 120s! Aborting.\n");
			goto cleanup;
		}
		// If it's -EACCES or something else, /data is mounted but SELinux is blocking us. Proceed anyway.
	} else {
		filp_close(f, NULL);
	}

	// 2. /data is mounted and ksud is confirmed ready! Execute instantly.
	mutex_lock(&prop_lock);
	list_for_each_entry_safe(entry, tmp, &prop_list, list) {
		char *argv[] = { "/data/adb/ksud", "resetprop", entry->prop, entry->val, NULL };
		int ret;

		ret = call_usermodehelper(argv[0], argv, NULL, UMH_WAIT_EXEC);
		
		if (ret == 0) {
			pr_info("pavolia_reine: Successfully executed -> ksud resetprop %s %s\n", entry->prop, entry->val);
		} else {
			pr_err("pavolia_reine: Failed to execute ksud resetprop %s %s (ret=%d)\n", entry->prop, entry->val, ret);
		}

		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&prop_lock);

	pr_info("pavolia_reine: All queued properties processed dynamically.\n");
	return;

cleanup:
	mutex_lock(&prop_lock);
	list_for_each_entry_safe(entry, tmp, &prop_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&prop_lock);
}

int pavolia_reine_resetprop(const char *prop, const char *val)
{
	struct prop_entry *new_entry;

	if (!prop || !val)
		return -EINVAL;

	new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;

	strscpy(new_entry->prop, prop, sizeof(new_entry->prop));
	strscpy(new_entry->val, val, sizeof(new_entry->val));

	mutex_lock(&prop_lock);
	list_add_tail(&new_entry->list, &prop_list);
	mutex_unlock(&prop_lock);

	pr_info("pavolia_reine: Queued resetprop -> %s = %s\n", prop, val);

	// Ensure the polling loop is active
	if (!delayed_work_pending(&prop_work)) {
		schedule_delayed_work(&prop_work, msecs_to_jiffies(POLL_INTERVAL_MS));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pavolia_reine_resetprop);

static int __init pavolia_reine_init(void)
{
	// Start the dynamic VFS polling loop 10 seconds after boot
	schedule_delayed_work(&prop_work, msecs_to_jiffies(10000));

	pr_info("pavolia_reine: Usermodehelper API initialized. Dynamic VFS polling started.\n");
	return 0;
}

static void __exit pavolia_reine_exit(void)
{
	struct prop_entry *entry, *tmp;

	cancel_delayed_work_sync(&prop_work);

	mutex_lock(&prop_lock);
	list_for_each_entry_safe(entry, tmp, &prop_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&prop_lock);

	pr_info("pavolia_reine: Unloaded.\n");
}

late_initcall(pavolia_reine_init);
module_exit(pavolia_reine_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Pavolia Reine Usermodehelper Resetprop API");