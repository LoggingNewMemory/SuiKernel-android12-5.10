// SPDX-License-Identifier: GPL-3.0-only
/*
 * airani_iofifteen_cpuset.c
 * Airani Iofifteen — Maximum CPUSet Tweaks (Raco API)
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/raco_override.h>

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

#define ENGAGE_DELAY_MS     20000
#define CPUSET_SCAN_MS       5000

static bool airani_enabled = true;
module_param(airani_enabled, bool, 0644);
MODULE_PARM_DESC(airani_enabled, "Enable Airani Iofifteen CPUSet Tweaks (default: true)");

static char all_cores_str[64];
static char no_prime_cores_str[64];
static char little_cores_str[64];
static bool masks_calculated = false;

static int airani_write_file(const char *path, const char *buf)
{
	struct file *f;
	loff_t pos = 0;
	int ret;

	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	ret = kernel_write(f, buf, strlen(buf), &pos);
	filp_close(f, NULL);

	return ret < 0 ? ret : 0;
}

static void airani_calculate_dynamic_masks(void)
{
	int cpu;
	int max_cap = 0, min_cap = 1024;
	struct cpumask all_mask, no_prime_mask, little_mask;
	
	cpumask_clear(&all_mask);
	cpumask_clear(&no_prime_mask);
	cpumask_clear(&little_mask);

	/* 1. Find min and max capacities */
	for_each_possible_cpu(cpu) {
		int cap = arch_scale_cpu_capacity(cpu);
		if (cap > max_cap)
			max_cap = cap;
		if (cap < min_cap)
			min_cap = cap;
		cpumask_set_cpu(cpu, &all_mask);
	}

	/* 2. Group CPUs into masks based on capacity */
	for_each_possible_cpu(cpu) {
		int cap = arch_scale_cpu_capacity(cpu);
		
		/* If it's a little core, add to little_mask */
		if (cap == min_cap)
			cpumask_set_cpu(cpu, &little_mask);
			
		/* If it's NOT the prime/super core, add to no_prime_mask */
		if (cap < max_cap || max_cap == min_cap)
			cpumask_set_cpu(cpu, &no_prime_mask);
	}

	/* 3. Convert cpumasks to string (e.g. "0-3" or "4-7") using kernel's bitmap list printer */
	snprintf(all_cores_str, sizeof(all_cores_str), "%*pbl\n", cpumask_pr_args(&all_mask));
	snprintf(no_prime_cores_str, sizeof(no_prime_cores_str), "%*pbl\n", cpumask_pr_args(&no_prime_mask));
	snprintf(little_cores_str, sizeof(little_cores_str), "%*pbl\n", cpumask_pr_args(&little_mask));

	pr_info("iofi: Topologies -> All: %s NoPrime: %s Little: %s", all_cores_str, no_prime_cores_str, little_cores_str);
	masks_calculated = true;
}

/*
Yamada Note: 
Well this thing is actually pretty stupid since I don't know every devices
cpuset dirs, but I don't have any other way. I added oiface, etc it's because
I use ColorOS Port ROM (Which probably of course won't exist on other ROMs)
*/

static void airani_execute_cpuset_override(void)
{
	if (!masks_calculated)
		airani_calculate_dynamic_masks();

	/* TIER 1: MAXIMUM PERFORMANCE (Gaming, UI Rendering, Camera) */
	airani_write_file("/dev/cpuset/top-app/cpus", all_cores_str);
	airani_write_file("/dev/cpuset/oiface_fg/cpus", all_cores_str);
	airani_write_file("/dev/cpuset/sf/cpus", all_cores_str);
	airani_write_file("/dev/cpuset/audio-app/cpus", all_cores_str);
	airani_write_file("/dev/cpuset/camera-daemon/cpus", all_cores_str);

	/* TIER 2: BALANCED PERFORMANCE (Regular Apps, Saves Battery) */
	airani_write_file("/dev/cpuset/foreground/cpus", no_prime_cores_str);
	airani_write_file("/dev/cpuset/foreground_window/cpus", no_prime_cores_str);
	airani_write_file("/dev/cpuset/display/cpus", no_prime_cores_str);

	/* TIER 3: STRICT BATTERY SAVING (Background Tasks) */
	airani_write_file("/dev/cpuset/background/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/system-background/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/camera-background/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/restricted/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/oiface_bg/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/l-background/cpus", little_cores_str);
	airani_write_file("/dev/cpuset/h-background/cpus", little_cores_str);
}

static int airani_worker(void *data)
{
	pr_info("iofi: standing by — engaging in %d ms\n", ENGAGE_DELAY_MS);
	msleep(ENGAGE_DELAY_MS);

	airani_execute_cpuset_override();

	while (!kthread_should_stop()) {
		/*
		 * Watchdog: check top-app to ensure the vendor hasn't rolled it back
		 */
		{
			struct file *f = filp_open("/dev/cpuset/top-app/cpus",
						   O_RDONLY, 0);
			if (!IS_ERR(f)) {
				char current_mask[32] = {0};
				char *cleaned;
				loff_t pos = 0;

				kernel_read(f, current_mask,
					    sizeof(current_mask) - 1, &pos);
				filp_close(f, NULL);

				cleaned = strim(current_mask);
				if (strstr(cleaned, all_cores_str) == NULL) {
					pr_info("iofi: Watchdog caught vendor rollback ('%s')! Re-enforcing.\n",
						cleaned);
					airani_execute_cpuset_override();
				}
			}
		}

		msleep_interruptible(CPUSET_SCAN_MS);
	}

	return 0;
}

static int __init airani_cpuset_init(void)
{
	if (!airani_enabled) {
		pr_info("iofi: disabled via module param\n");
		return 0;
	}

	if (raco_register_rc_override(airani_execute_cpuset_override, "airani.cpuset_lock") == 0)
		pr_info("iofi: CPUSet guard hooked to Raco Global Sniper\n");
	else
		pr_warn("iofi: Raco hook failed, continuing without it\n");

	airani_thread = kthread_run(airani_worker, NULL, "airani_cpuset");
	if (IS_ERR(airani_thread)) {
		pr_err("iofi: failed to start thread: %ld\n",
		       PTR_ERR(airani_thread));
		raco_unregister_rc_override(airani_execute_cpuset_override);
		return PTR_ERR(airani_thread);
	}

	pr_info("iofi: active\n");
	return 0;
}

static void __exit airani_cpuset_exit(void)
{
	if (airani_thread)
		kthread_stop(airani_thread);

	raco_unregister_rc_override(airani_execute_cpuset_override);

	pr_info("iofi: unloaded\n");
}

module_init(airani_cpuset_init);
module_exit(airani_cpuset_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Airani Iofifteen — Maximum CPUSet Tweaks");
