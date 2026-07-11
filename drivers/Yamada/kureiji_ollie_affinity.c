// SPDX-License-Identifier: GPL-3.0-only
// Kureiji Ollie Affinity — Kswapd Binder
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/sched/topology.h>
#include <linux/raco_override.h>

static int target_big_core = -1;

/* Helper function to find a Big Core dynamically based on capacity */
static int find_big_core(void)
{
	int cpu;
	int max_capacity = 0;
	int best_cpu = 0;

	for_each_possible_cpu(cpu) {
		int cap = arch_scale_cpu_capacity(cpu);
		if (cap > max_capacity) {
			max_capacity = cap;
			best_cpu = cpu;
		}
	}
	
	return best_cpu;
}

static void enforce_kswapd_affinity(void)
{
	struct task_struct *p;
	struct cpumask kswapd_mask;
	bool found = false;

	if (target_big_core < 0) {
		target_big_core = find_big_core();
		pr_info("Kureiji Ollie: Auto-detected Big Core %d for Kswapd\n", target_big_core);
	}

	cpumask_clear(&kswapd_mask);
	cpumask_set_cpu(target_big_core, &kswapd_mask);

	rcu_read_lock();
	for_each_process(p) {
		if (strncmp(p->comm, "kswapd0", 7) == 0) {
			if (!cpumask_equal(&p->cpus_mask, &kswapd_mask)) {
				set_cpus_allowed_ptr(p, &kswapd_mask);
				pr_info("Kureiji Ollie: Enforced Kswapd0 affinity to Core %d\n", target_big_core);
			}
			found = true;
			break;
		}
	}
	rcu_read_unlock();

	if (!found) {
		pr_warn("Kureiji Ollie: Failed to find kswapd0 process!\n");
	}
}

static int __init kureiji_ollie_init(void)
{
	int ret;

	pr_info("Kureiji Ollie: Initializing Kswapd Affinity Binder\n");

	ret = raco_register_rc_override(enforce_kswapd_affinity, "kureiji_ollie_kswapd");
	if (ret) {
		pr_err("Kureiji Ollie: Failed to register RACO override\n");
		return ret;
	}

	return 0;
}

static void __exit kureiji_ollie_exit(void)
{
	raco_unregister_rc_override(enforce_kswapd_affinity);
	pr_info("Kureiji Ollie: Unloaded\n");
}

module_init(kureiji_ollie_init);
module_exit(kureiji_ollie_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Kureiji Ollie - Modifies kswapd to highest CPU cores");
