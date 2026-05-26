// drivers/misc/raco_rc_override.c
// SPDX-License-Identifier: GPL-2.0-only
// Raco Override — Universal .rc Override API
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>       
#include <linux/list.h>
#include <linux/raco_override.h>

/*
I use linked list to handle multiple files, I know that
One day I might ran out of spaces and I probably will forgot about this XD

To use: Simply you can do raco_register_rc_override. See the
drivers/misc/sparxie_swapiness_tuner.c for example (Simplest usage)
*/

struct raco_target {
    int *kernel_val_ptr;
    int target_val;
    const char *name;
    struct list_head list;   
};

static LIST_HEAD(raco_target_list);
static struct task_struct *raco_thread;

static int raco_sniper_thread(void *data)
{
    int retries = 24; 
    struct raco_target *entry;
    struct raco_target *tmp;

    pr_info("raco_override: Global Sniper deployed in shadow lines...\n");

    while (!kthread_should_stop() && retries > 0) {
        list_for_each_entry(entry, &raco_target_list, list) {
            if (*(entry->kernel_val_ptr) != entry->target_val) {
                pr_info("raco_override: HAHA! Caught init.rc altering %s! Forcing back to %d\n", 
                        entry->name, entry->target_val);
                
                *(entry->kernel_val_ptr) = entry->target_val;
            }
        }
        msleep_interruptible(5000);
        retries--;
    }

    /* Clean up loop uses the cleanly initialized tmp pointer up top */
    list_for_each_entry_safe(entry, tmp, &raco_target_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    pr_info("raco_override: Global Sniper mission complete. List cleaned up.\n");
    raco_thread = NULL;
    return 0;
}

int raco_register_rc_override(int *target_ptr, int desired_val, const char *name)
{
    struct raco_target *new_target;

    new_target = kmalloc(sizeof(struct raco_target), GFP_KERNEL);
    if (!new_target) {
        pr_err("raco_override: Failed to allocate memory for %s\n", name);
        return -ENOMEM;
    }

    new_target->kernel_val_ptr = target_ptr;
    new_target->target_val = desired_val;
    new_target->name = name;

    list_add_tail(&new_target->list, &raco_target_list);
    pr_info("raco_override: Registered [%s] -> %d\n", name, desired_val);

    if (!raco_thread) {
        raco_thread = kthread_run(raco_sniper_thread, NULL, "raco_sniper");
        if (IS_ERR(raco_thread)) {
            pr_err("raco_override: Failed to deploy Raco Sniper on demand\n");
            raco_thread = NULL;
        } else {
            pr_info("raco_override: Global Sniper deployed dynamically via registration!\n");
        }
    }

    return 0;
}
EXPORT_SYMBOL(raco_register_rc_override);

static int __init raco_override_init(void)
{
    pr_info("raco_override: Framework core initialized. Waiting for registrations...\n");
    return 0;
}
late_initcall(raco_override_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Raco Universal RC Override");