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

static struct task_struct *airani_thread;
static char dynamic_cpu_mask[32];

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

static void airani_execute_cpuset_override(void)
{
	int total_cores = num_possible_cpus();
	char write_buf[32];

	/*
	 * Keep dynamic_cpu_mask without \n for strstr checking,
	 * but use write_buf with \n for the actual kernel_write.
	 */
	snprintf(dynamic_cpu_mask, sizeof(dynamic_cpu_mask),
		 "0-%d", total_cores - 1);
	snprintf(write_buf, sizeof(write_buf), "%s\n", dynamic_cpu_mask);

	pr_info("airani: Dynamic core range: %s\n", dynamic_cpu_mask);

	if (airani_write_file("/dev/cpuset/foreground/cpus", write_buf) == 0)
		pr_info("airani: foreground cpuset -> %s\n", dynamic_cpu_mask);

	if (airani_write_file("/dev/cpuset/top-app/cpus", write_buf) == 0)
		pr_info("airani: top-app cpuset -> %s\n", dynamic_cpu_mask);

	if (airani_write_file("/dev/cpuset/boost-app/cpus", write_buf) == 0)
		pr_info("airani: boost-app cpuset -> %s\n", dynamic_cpu_mask);
}

static int airani_worker(void *data)
{
	pr_info("airani: standing by — engaging in %d ms\n", ENGAGE_DELAY_MS);
	msleep(ENGAGE_DELAY_MS);

	airani_execute_cpuset_override();

	while (!kthread_should_stop()) {
		/*
		 * Watchdog: read the live cpuset value and re-enforce
		 * if a vendor service rolled it back without triggering
		 * Raco (e.g. direct sysfs write after Raco's window).
		 */
		{
			struct file *f = filp_open("/dev/cpuset/foreground/cpus",
						   O_RDONLY, 0);
			if (!IS_ERR(f)) {
				char current_mask[32] = {0};
				char *cleaned;
				loff_t pos = 0;

				kernel_read(f, current_mask,
					    sizeof(current_mask) - 1, &pos);
				filp_close(f, NULL);

				cleaned = strim(current_mask);
				if (strstr(cleaned, dynamic_cpu_mask) == NULL) {
					pr_info("airani: Watchdog caught vendor rollback ('%s')! Re-enforcing.\n",
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
		pr_info("airani: disabled via module param\n");
		return 0;
	}

	if (raco_register_rc_override(airani_execute_cpuset_override, "airani.cpuset_lock") == 0)
		pr_info("airani: CPUSet guard hooked to Raco Global Sniper\n");
	else
		pr_warn("airani: Raco hook failed, continuing without it\n");

	airani_thread = kthread_run(airani_worker, NULL, "airani_cpuset");
	if (IS_ERR(airani_thread)) {
		pr_err("airani: failed to start thread: %ld\n",
		       PTR_ERR(airani_thread));
		raco_unregister_rc_override(airani_execute_cpuset_override);
		return PTR_ERR(airani_thread);
	}

	pr_info("airani: active\n");
	return 0;
}

static void __exit airani_cpuset_exit(void)
{
	if (airani_thread)
		kthread_stop(airani_thread);

	raco_unregister_rc_override(airani_execute_cpuset_override);

	pr_info("airani: unloaded\n");
}

module_init(airani_cpuset_init);
module_exit(airani_cpuset_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Airani Iofifteen — Maximum CPUSet Tweaks");
