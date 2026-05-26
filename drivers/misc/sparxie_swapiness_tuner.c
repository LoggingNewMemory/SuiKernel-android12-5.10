// sparxie_swapiness_tuner.c
// SPDX-License-Identifier: GPL-2.0-only
// Sparxie Swappiness Tuner — Delegated to Raco Engine API
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/init.h>
#include <linux/raco_override.h>  /* Raco API header! */

extern int vm_swappiness;

static int __init sparxie_swap_init(void)
{
    /* 1. Set the initial ideal value at early boot */
    vm_swappiness = 30;
    
    /* 2. Hand over the long-term defense to the Raco Sniper Framework */
    if (raco_register_rc_override(&vm_swappiness, 30, "vm.swappiness") == 0) {
        pr_info("sparxie: Swappiness defense successfully delegated to Raco Engine!\n");
    } else {
        pr_err("sparxie: Failed to hook into Raco Framework\n");
    }
    
    return 0;
}

static void __exit sparxie_swap_exit(void)
{
    /* Since the registration uses dynamic kmalloc memory, the cleanup 
     * is handled inside the core framework's thread termination loop.
     */
    pr_info("sparxie: Swappiness tuner module detached.\n");
}

module_init(sparxie_swap_init);
module_exit(sparxie_swap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sparxie Swappiness Tuner - Hooked to Raco Override Engine");