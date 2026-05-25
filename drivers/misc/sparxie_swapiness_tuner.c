// sparxie_swapiness_tuner.c
// SPDX-License-Identifier: GPL-2.0-only
// Sparxie Swappiness Tuner — Change the value of the swappiness
// Author: Kanagawa Yamada

// Note: This is so stupid but I must say this one should work no matter when init replace swap val

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>

extern int vm_swappiness;
static struct task_struct *sparxie_thread;

static int sparxie_sniper(void *data)
{
    int retries = 24; 
    
    pr_info("sparxie: Sniper is in position, waiting for init.rc ambush...\n");
    
    while (!kthread_should_stop() && retries > 0) {
        if (vm_swappiness != 30) {
            pr_info("sparxie: HAHA! Caught init.rc changing swappiness to %d!\n", vm_swappiness);
            
            vm_swappiness = 30;
            
            pr_info("sparxie: Forced back to 30. Target eliminated. Sniper exiting!\n");
            break;
        }
        
        msleep_interruptible(5000);
        retries--;
    }
    
    if (retries == 0) {
        pr_info("sparxie: No ambush from init.rc after 2 minutes. Sniper exiting cleanly!\n");
    }
    
    return 0;
}

static int __init sparxie_swap_init(void)
{
    vm_swappiness = 30;
    
    sparxie_thread = kthread_run(sparxie_sniper, NULL, "sparxie_sniper");
    if (IS_ERR(sparxie_thread)) {
        pr_err("sparxie: Failed to deploy sniper\n");
        return PTR_ERR(sparxie_thread);
    }
    
    return 0;
}

static void __exit sparxie_swap_exit(void)
{
    pr_info("sparxie: Module unloaded.\n");
}

module_init(sparxie_swap_init);
module_exit(sparxie_swap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sparxie Swappiness Tuner - Change the value of the swappiness");