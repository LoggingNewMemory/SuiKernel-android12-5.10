// SPDX-License-Identifier: GPL-3.0-only
// drivers/Kana/gov_enforcer.c
// Governor Enforcer — enforces user predefined CPU governor against vendor init.rc overrides
// Author: Kana Wesley Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cpufreq.h>
#include <linux/raco_override.h>
#include <linux/suikernel.h>
#include "IOCTL_Library.h"

char gov_enforcer_target[32] = "schedutil";
bool gov_enforcer_enabled = true;

static void enforce_user_governor(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	struct file *filp;
	loff_t pos;
	char path[128];
	char target_gov[32] = "schedutil";
	char write_buf[48];

	// Mandatory: check if /data is ready
	if (!sui_data_is_ready())
		return;

	if (!gov_enforcer_enabled)
		return;

	if (!sui_is_feature_enabled("gov_enforcer")) {
		gov_enforcer_enabled = false;
		return;
	}

	if (sui_read_config("governor", target_gov, sizeof(target_gov)) > 0) {
		char *nl = strchr(target_gov, '\n');
		if (nl) *nl = '\0';
		nl = strchr(target_gov, '\r');
		if (nl) *nl = '\0';
	}

	if (!target_gov[0])
		strlcpy(target_gov, "schedutil", sizeof(target_gov));

	strlcpy(gov_enforcer_target, target_gov, sizeof(gov_enforcer_target));
	snprintf(write_buf, sizeof(write_buf), "%s\n", target_gov);

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			if (policy->governor && strcmp(policy->governor->name, target_gov) != 0) {
				snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor", cpu);
				filp = filp_open(path, O_WRONLY, 0);
				if (!IS_ERR(filp)) {
					pos = 0;
					kernel_write(filp, write_buf, strlen(write_buf), &pos);
					filp_close(filp, NULL);
					pr_info("gov_enforcer: policy%u governor enforced to %s\n", cpu, target_gov);
				}
			}
		}
		cpufreq_cpu_put(policy);
	}
}

void gov_enforcer_set_target(const char *gov)
{
	if (gov && gov[0]) {
		strlcpy(gov_enforcer_target, gov, sizeof(gov_enforcer_target));
		enforce_user_governor();
	}
}
EXPORT_SYMBOL(gov_enforcer_set_target);

void gov_enforcer_set_enabled(bool enabled)
{
	gov_enforcer_enabled = enabled;
	if (enabled)
		enforce_user_governor();
}
EXPORT_SYMBOL(gov_enforcer_set_enabled);

static int __init gov_enforcer_init(void)
{
	int err;

	err = raco_register_rc_override(enforce_user_governor, "gov_enforcer_flag");
	if (err) {
		pr_err("gov_enforcer: Failed to register with Raco!\n");
	} else {
		pr_info("gov_enforcer: Governor Enforcer registered with Raco!\n");
	}

	return 0;
}
late_initcall(gov_enforcer_init);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kana Wesley Yamada");
MODULE_DESCRIPTION("Enforce user predefined CPU governor against vendor init.rc overrides");
