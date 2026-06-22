// SPDX-License-Identifier: GPL-3.0-only
/*
 * sparxie_swapiness_tuner.c
 * Sparxie Swappiness Tuner — Delegated to Raco Engine API
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/mm.h>
#include <linux/raco_override.h>

#define SPARXIE_SWAPPINESS 30

extern int vm_swappiness;

/*
 * We simply define a callback function that Raco will execute every 5s.
 * This actively pushes our value back into vm_swappiness, fighting vendor init!
 */
static void apply_swappiness_cb(void)
{
	vm_swappiness = SPARXIE_SWAPPINESS;
	pr_info("sparxie: vm_swappiness forcefully applied -> %d\n", SPARXIE_SWAPPINESS);
}

static int __init sparxie_swap_init(void)
{
	/* 1. Apply immediately at boot */
	vm_swappiness = SPARXIE_SWAPPINESS;
	pr_info("sparxie: vm_swappiness set to %d\n", SPARXIE_SWAPPINESS);

	/* 2. Hand long-term defence to Raco Sniper via callback */
	if (raco_register_rc_override(apply_swappiness_cb, "vm.swappiness") == 0) {
		pr_info("sparxie: Swappiness defence delegated to Raco Engine\n");
	} else {
		pr_err("sparxie: Failed to hook into Raco Framework\n");
	}

	return 0;
}

static void __exit sparxie_swap_exit(void)
{
	/*
	 * Unregister the callback from Raco so the Sniper thread does not
	 * execute it after this module is freed.
	 */
	raco_unregister_rc_override(apply_swappiness_cb);

	pr_info("sparxie: Swappiness tuner module detached.\n");
}

module_init(sparxie_swap_init);
module_exit(sparxie_swap_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sparxie Swappiness Tuner — Hooked to Raco Override Engine");