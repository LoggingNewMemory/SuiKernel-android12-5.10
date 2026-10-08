// SPDX-License-Identifier: GPL-3.0-only
// drivers/misc/pavolia_reine_setprop.c
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
#include <linux/pavolia_reine_setprop.h>

#define POLL_INTERVAL_MS 5000
#define MAX_RETRIES      24 // 120s max wait time

struct prop_entry {
	char prop[128];
	char val[128];
	struct list_head list;
};

static LIST_HEAD(prop_list);
static DEFINE_MUTEX(prop_lock);
static void execute_setprop_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(prop_work, execute_setprop_work);
static int retry_count = 0;

static void execute_setprop_work(struct work_struct *work)
{
	struct prop_entry *entry, *tmp;
	struct file *f;

	// 1. Actively check if the property service socket exists yet
	f = filp_open("/dev/socket/property_service", O_RDONLY, 0);
	if (IS_ERR(f)) {
		if (PTR_ERR(f) == -ENOENT) {
			if (retry_count++ < MAX_RETRIES) {
				// property_service is not ready. Try again in 5s.
				schedule_delayed_work(&prop_work, msecs_to_jiffies(POLL_INTERVAL_MS));
				return;
			}
			pr_err("pavolia_reine: /dev/socket/property_service never appeared after 120s! Aborting.\n");
			goto cleanup;
		}
		// If it's -EACCES or something else, it exists but SELinux is blocking us. Proceed anyway.
	} else {
		filp_close(f, NULL);
	}

	// 2. Property service is confirmed ready! Execute instantly.
	mutex_lock(&prop_lock);
	list_for_each_entry_safe(entry, tmp, &prop_list, list) {
		char *argv[4];
		char *envp[4];
		int ret;

		argv[0] = "/system/bin/setprop";
		argv[1] = entry->prop;
		argv[2] = entry->val;
		argv[3] = NULL;

		envp[0] = "HOME=/";
		envp[1] = "TERM=linux";
		envp[2] = "PATH=/sbin:/system/sbin:/system/bin:/system/xbin";
		envp[3] = NULL;

		ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
		
		if (ret == 0) {
			pr_info("pavolia_reine: Successfully executed -> setprop %s %s\n", entry->prop, entry->val);
		} else {
			pr_err("pavolia_reine: Failed to execute setprop %s %s (ret=%d)\n", entry->prop, entry->val, ret);
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

int pavolia_reine_setprop(const char *prop, const char *val)
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

	pr_info("pavolia_reine: Queued setprop -> %s = %s\n", prop, val);

	// Ensure the polling loop is active
	if (!delayed_work_pending(&prop_work)) {
		schedule_delayed_work(&prop_work, msecs_to_jiffies(POLL_INTERVAL_MS));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pavolia_reine_setprop);

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