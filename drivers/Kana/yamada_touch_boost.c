// SPDX-License-Identifier: GPL-3.0-only
// Kana Touch Boost — Universal Governor Edition (PM QoS)
// Author: Kana Wesley Yamada

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/pm_qos.h>
#include <linux/suikernel.h>
#include "IOCTL_Library.h"

/* Hook from drivers/input/input.c */
extern void (*yamada_boost_hook)(void);

#define BOOST_DURATION_BALANCED_MS 100
#define BOOST_DURATION_GAMING_MS   300

bool yamada_touch_boost_enabled = true;
int yamada_touch_boost_duration = BOOST_DURATION_BALANCED_MS;

static unsigned long boost_active;
static struct work_struct boost_on_work;
static struct delayed_work boost_off_work;
static struct delayed_work touch_boost_init_work;

/* PM QoS requests per policy CPU */
static struct freq_qos_request touch_min_req[NR_CPUS];
static bool qos_initialized[NR_CPUS];

static void yamada_boost_on_work(struct work_struct *work)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;

	if (!test_bit(0, &boost_active))
		return;

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu == cpu) {
			if (!qos_initialized[cpu]) {
				freq_qos_add_request(&policy->constraints,
						     &touch_min_req[cpu],
						     FREQ_QOS_MIN,
						     policy->cpuinfo.max_freq);
				qos_initialized[cpu] = true;
			} else {
				freq_qos_update_request(&touch_min_req[cpu],
							policy->cpuinfo.max_freq);
			}
		}
		cpufreq_cpu_put(policy);
	}
}

static void yamada_boost_off_work(struct work_struct *work)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;

	clear_bit(0, &boost_active);
	smp_mb__after_atomic();

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu == cpu && qos_initialized[cpu]) {
			if (!test_bit(0, &boost_active)) {
				freq_qos_update_request(&touch_min_req[cpu],
							policy->cpuinfo.min_freq);
			}
		}
		cpufreq_cpu_put(policy);
	}
}

void yamada_touch_boost_off(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;

	cancel_work_sync(&boost_on_work);
	cancel_delayed_work_sync(&boost_off_work);
	clear_bit(0, &boost_active);
	smp_mb__after_atomic();

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		if (policy->cpu == cpu && qos_initialized[cpu]) {
			freq_qos_update_request(&touch_min_req[cpu],
						policy->cpuinfo.min_freq);
		}
		cpufreq_cpu_put(policy);
	}
}
EXPORT_SYMBOL(yamada_touch_boost_off);

static void yamada_touch_boost_trigger(void)
{
	if (!test_and_set_bit(0, &boost_active)) {
		queue_work(system_highpri_wq, &boost_on_work);
	}

	mod_delayed_work(system_highpri_wq, &boost_off_work, msecs_to_jiffies(yamada_touch_boost_duration));
}

void yamada_touch_boost_set_mode(int mode)
{
	if (mode == 0) {
		yamada_touch_boost_enabled = false;
		WRITE_ONCE(yamada_boost_hook, NULL);
		yamada_touch_boost_off();
		pr_info("yamada_touch_boost: disabled\n");
	} else if (mode == 2) {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = BOOST_DURATION_GAMING_MS;
		WRITE_ONCE(yamada_boost_hook, yamada_touch_boost_trigger);
		pr_info("yamada_touch_boost: gaming mode (300ms)\n");
	} else {
		yamada_touch_boost_enabled = true;
		yamada_touch_boost_duration = BOOST_DURATION_BALANCED_MS;
		WRITE_ONCE(yamada_boost_hook, yamada_touch_boost_trigger);
		pr_info("yamada_touch_boost: balanced mode (100ms)\n");
	}
}
EXPORT_SYMBOL(yamada_touch_boost_set_mode);

static void yamada_qos_cleanup(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (qos_initialized[cpu]) {
			freq_qos_remove_request(&touch_min_req[cpu]);
			qos_initialized[cpu] = false;
		}
	}
}

static void touch_boost_init_worker(struct work_struct *work)
{
	int mode;

	// Mandatory: /data must be checked
	mode = sui_get_touch_boost_mode();
	yamada_touch_boost_set_mode(mode);
}

static int __init yamada_touch_boost_init(void)
{
	memset(qos_initialized, 0, sizeof(qos_initialized));

	INIT_WORK(&boost_on_work, yamada_boost_on_work);
	INIT_DELAYED_WORK(&boost_off_work, yamada_boost_off_work);
	INIT_DELAYED_WORK(&touch_boost_init_work, touch_boost_init_worker);

	// Default to balanced mode until /data config is evaluated
	yamada_touch_boost_set_mode(1);

	schedule_delayed_work(&touch_boost_init_work, msecs_to_jiffies(15000));
	pr_info("yamada_touch_boost: universal governor initialized (awaiting /data config)\n");
	return 0;
}

static void __exit yamada_touch_boost_exit(void)
{
	WRITE_ONCE(yamada_boost_hook, NULL);
	cancel_delayed_work_sync(&touch_boost_init_work);
	yamada_touch_boost_off();
	yamada_qos_cleanup();
	pr_info("yamada_touch_boost: unloaded\n");
}

module_init(yamada_touch_boost_init);
module_exit(yamada_touch_boost_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kana Wesley Yamada");
MODULE_DESCRIPTION("Kana Touch Boost — Universal Governor Edition (PM QoS)");
