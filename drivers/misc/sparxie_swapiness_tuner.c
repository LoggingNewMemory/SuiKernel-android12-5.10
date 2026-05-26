// SPDX-License-Identifier: GPL-2.0-only
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
 * Shadow atomic — Raco guards this value.
 * When a vendor service clobbers vm_swappiness, Raco resets the shadow
 * back to SPARXIE_SWAPPINESS; the apply_work then writes it to the real
 * vm_swappiness.
 *
 * NOTE: Raco's sniper polls every 5 s and resets the shadow if it
 * drifts. That reset is our "pulse". The kthread in Raco will set it
 * back; we don't need a separate monitoring thread here — the delayed
 * work fires once at boot, and Raco keeps the shadow in check.
 * If you want continuous re-application on vendor rollback, add a
 * periodic work (see inaho for that pattern).
 */
static atomic_t sparxie_swappiness = ATOMIC_INIT(SPARXIE_SWAPPINESS);

static void apply_swappiness_work_fn(struct work_struct *w)
{
	int val = atomic_read(&sparxie_swappiness);

	vm_swappiness = val;
	pr_info("sparxie: vm_swappiness applied -> %d\n", val);
}
static DECLARE_DELAYED_WORK(apply_swappiness_work, apply_swappiness_work_fn);

static int __init sparxie_swap_init(void)
{
	/* 1. Apply immediately at boot */
	vm_swappiness = SPARXIE_SWAPPINESS;
	pr_info("sparxie: vm_swappiness set to %d\n", SPARXIE_SWAPPINESS);

	/* 2. Hand long-term defence to Raco Sniper via the shadow atomic */
	if (raco_register_rc_override(&sparxie_swappiness, SPARXIE_SWAPPINESS,
				      "vm.swappiness") == 0) {
		pr_info("sparxie: Swappiness defence delegated to Raco Engine\n");
	} else {
		pr_err("sparxie: Failed to hook into Raco Framework\n");
	}

	/*
	 * Queue a delayed apply so the shadow's initial value is written to
	 * vm_swappiness after any early-boot vendor services have run.
	 * 30 s is enough for init.rc to finish on mt6833.
	 */
	schedule_delayed_work(&apply_swappiness_work, msecs_to_jiffies(30000));

	return 0;
}

static void __exit sparxie_swap_exit(void)
{
	/*
	 * Cancel pending work BEFORE unregistering from Raco, so the work
	 * fn cannot run after the module text is unmapped.
	 */
	cancel_delayed_work_sync(&apply_swappiness_work);

	/*
	 * Unregister the shadow from Raco so the Sniper thread does not
	 * dereference sparxie_swappiness after this module is freed.
	 */
	raco_unregister_rc_override(&sparxie_swappiness);

	pr_info("sparxie: Swappiness tuner module detached.\n");
}

module_init(sparxie_swap_init);
module_exit(sparxie_swap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sparxie Swappiness Tuner — Hooked to Raco Override Engine");