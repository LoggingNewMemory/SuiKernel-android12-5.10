// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/schedutil_enforcer.c
 * Schedutil Enforcer, shielded by Raco Universal RC Override
 * Author: Kobo Kanaeru (and Bang!)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/cpumask.h> /* Kobo's secret for dynamic CPU scaling! */
#include <linux/raco_override.h>

/* Kobo's master switch! Raco will make sure init.rc doesn't touch this. */
static atomic_t schedutil_enforcer_active = ATOMIC_INIT(1);
static struct task_struct *schedutil_thread;

/* Function to splash the schedutil governor onto ALL CPU policies dynamically */
static void set_governor_to_schedutil(void)
{
	struct file *filp;
	loff_t pos;
	char *gov = "schedutil\n"; /* Exactly 10 bytes with the newline */
	int cpu;
	char path[128];

	/* * Dynamic magic! Loops through every possible CPU core on the device.
	 * No more hardcoded "8"! 
	 */
	for_each_possible_cpu(cpu) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", cpu);
		
		filp = filp_open(path, O_WRONLY, 0);
		if (!IS_ERR(filp)) {
			pos = 0;
			kernel_write(filp, gov, 10, &pos);
			filp_close(filp, NULL);
		}
	}
}

static int schedutil_enforcer_thread(void *data)
{
	/* Loop 30 times (approx 150 seconds) to outlast vendor init.rc spam */
	int retries = 30;

	pr_info("schedutil_enforcer: Kobo's dynamic water magic activated! Forcing schedutil on ALL cores...\n");

	while (!kthread_should_stop() && retries > 0) {
		/* If Raco says we are good to go, enforce the governor! */
		if (atomic_read(&schedutil_enforcer_active) == 1) {
			set_governor_to_schedutil();
		}

		msleep_interruptible(5000); /* Wait 5 seconds between strikes */
		retries--;
	}

	pr_info("schedutil_enforcer: Boot phase complete, going to sleep~ zzz...\n");
	return 0;
}

static int __init schedutil_enforcer_init(void)
{
	int err;

	/* 1. Register our atomic flag with Kanagawa's Raco API */
	err = raco_register_rc_override(&schedutil_enforcer_active, 1, "schedutil_enforcer_flag");
	if (err) {
		pr_err("schedutil_enforcer: Anjir! Failed to register with Raco!\n");
	}

	/* 2. Deploy our own enforcer thread */
	schedutil_thread = kthread_run(schedutil_enforcer_thread, NULL, "schedutil_enforcer");
	if (IS_ERR(schedutil_thread)) {
		pr_err("schedutil_enforcer: Failed to spawn thread\n");
		return PTR_ERR(schedutil_thread);
	}

	return 0;
}
late_initcall(schedutil_enforcer_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Dynamic Schedutil Enforcer using Raco API");