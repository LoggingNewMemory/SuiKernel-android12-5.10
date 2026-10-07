// SPDX-License-Identifier: GPL-3.0-only
// drivers/misc/raco_rc_override.c
// Raco Override — Universal .rc Override API
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kprobes.h>
#include <linux/raco_override.h>

#define RACO_HEAL_DELAY_MS 250 // Time to wait after init writes before healing

struct raco_target {
	raco_enforce_cb_t cb;
	const char     *name;
	struct list_head list;
};

static LIST_HEAD(raco_target_list);
static DEFINE_MUTEX(raco_list_lock);
static struct delayed_work raco_work;

static void raco_sniper_work(struct work_struct *work)
{
	struct raco_target *entry;

	mutex_lock(&raco_list_lock);
	list_for_each_entry(entry, &raco_target_list, list) {
		if (entry->cb) {
			// Punch vendor init.rc! Execute the callback unconditionally
			entry->cb();
		}
	}
	mutex_unlock(&raco_list_lock);
}

static unsigned long raco_expiry_jiffies;

// Kprobe on vfs_write to intercept init.rc activities
static int raco_vfs_write_pre(struct kprobe *p, struct pt_regs *regs)
{
	// Expire after 120 seconds. Mission accomplished, stop fighting the system.
	if (time_after(jiffies, raco_expiry_jiffies))
		return 0;

	// Fast path: Only intercept if the writer is 'init' or 'vendor_init'
	if (strncmp(current->comm, "init", 4) == 0 || strncmp(current->comm, "vendor_init", 11) == 0) {
		// If targets exist, debounce the heal operation
		if (!list_empty(&raco_target_list)) {
			mod_delayed_work(system_wq, &raco_work, msecs_to_jiffies(RACO_HEAL_DELAY_MS));
		}
	}
	return 0;
}

static struct kprobe raco_kprobe = {
	.symbol_name = "vfs_write",
	.pre_handler = raco_vfs_write_pre,
};

// raco_register_rc_override - Register a callback to fight init.rc interference.
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

	new_target->cb   = enforce_cb;
	new_target->name = name;

	mutex_lock(&raco_list_lock);
	list_add_tail(&new_target->list, &raco_target_list);
	pr_info("raco_override: Registered '%s' for event-driven enforcement\n", name);
	mutex_unlock(&raco_list_lock);

	// Force an initial heal right now in case init already wrote before we registered
	mod_delayed_work(system_wq, &raco_work, msecs_to_jiffies(RACO_HEAL_DELAY_MS));

	return 0;
}
EXPORT_SYMBOL_GPL(raco_register_rc_override);

// raco_unregister_rc_override - Remove a target from the watch list.
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

static int __init raco_override_init(void)
{
	int ret;
	
	// Set expiration to exactly 120 seconds after this module initializes
	raco_expiry_jiffies = jiffies + msecs_to_jiffies(120000);

	INIT_DELAYED_WORK(&raco_work, raco_sniper_work);
	
	ret = register_kprobe(&raco_kprobe);
	if (ret < 0) {
		pr_err("raco_override: Failed to register kprobe, error %d\n", ret);
		return ret;
	}

	pr_info("raco_override: Elite event-driven framework initialized (vfs_write hooked).\n");
	return 0;
}

static void __exit raco_override_exit(void)
{
	unregister_kprobe(&raco_kprobe);
	cancel_delayed_work_sync(&raco_work);
	pr_info("raco_override: Unloaded.\n");
}

late_initcall(raco_override_init);
module_exit(raco_override_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Raco Universal RC Override");