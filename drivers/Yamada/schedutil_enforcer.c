// SPDX-License-Identifier: GPL-3.0-only
// drivers/misc/schedutil_enforcer.c
// Schedutil Enforcer, enforce schedutil as default CPU GOV because vendor init.rc overrides it
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cpufreq.h>
#include <linux/raco_override.h>

static void set_governor_to_schedutil(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	struct file *filp;
	loff_t pos;
	char path[128];

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			if (policy->governor && strcmp(policy->governor->name, "schedutil") != 0) {
				snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor", cpu);
				filp = filp_open(path, O_WRONLY, 0);
				if (!IS_ERR(filp)) {
					pos = 0;
					kernel_write(filp, "schedutil\n", 10, &pos);
					filp_close(filp, NULL);
					pr_info("schedutil_enforcer: policy%u governor restored to schedutil\n", cpu);
				}
			}
		}
		cpufreq_cpu_put(policy);
	}
}

static int __init schedutil_enforcer_init(void)
{
	int err;

	err = raco_register_rc_override(set_governor_to_schedutil, "schedutil_enforcer_flag");
	if (err) {
		pr_err("schedutil_enforcer: Failed to register with Raco!\n");
	} else {
		pr_info("schedutil_enforcer: Schedutil enforcer activated via Raco!\n");
	}

	return 0;
}
late_initcall(schedutil_enforcer_init);

MODULE_LICENSE("GPL v3");
MODULE_DESCRIPTION("Enforce schedutil as default CPU GOV because vendor init.rc overrides it");