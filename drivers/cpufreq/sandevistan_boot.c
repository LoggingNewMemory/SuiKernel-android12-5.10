#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define BOOST_DELAY_SEC   30
#define GOV_BOOST         "performance"
#define GOV_NORMAL        "schedutil"

static bool sandevistan_enabled = true;
module_param(sandevistan_enabled, bool, 0644);
MODULE_PARM_DESC(sandevistan_enabled, "Enable Sandevistan boot governor boost (default: true)");

static struct delayed_work revert_work;

static int write_governor(unsigned int cpu, const char *gov)
{
    struct file *f;
    char path[64];
    loff_t pos = 0;
    int ret;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", cpu);

    f = filp_open(path, O_WRONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    ret = kernel_write(f, gov, strlen(gov), &pos);
    filp_close(f, NULL);

    return ret < 0 ? ret : 0;
}

static void set_governor_all(const char *gov)
{
    int cpu;
    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
        if (!policy)
            continue;
        /* Only write once per shared policy cluster */
        if (policy->cpu == cpu) {
            int ret = write_governor(cpu, gov);
            if (ret)
                pr_warn("sandevistan_boot: cpu%d → %s failed (%d)\n",
                        cpu, gov, ret);
        }
        cpufreq_cpu_put(policy);
    }
}

static void revert_governor_work(struct work_struct *work)
{
    pr_info("sandevistan_boot: flatline — reverting to %s\n", GOV_NORMAL);
    set_governor_all(GOV_NORMAL);
}

static int __init sandevistan_boot_init(void)
{
    if (!sandevistan_enabled) {
        pr_info("sandevistan_boot: disabled\n");
        return 0;
    }

    pr_info("sandevistan_boot: jacking in — %s for %ds\n",
            GOV_BOOST, BOOST_DELAY_SEC);

    INIT_DELAYED_WORK(&revert_work, revert_governor_work);
    set_governor_all(GOV_BOOST);
    schedule_delayed_work(&revert_work,
                          msecs_to_jiffies(BOOST_DELAY_SEC * 1000));
    return 0;
}

static void __exit sandevistan_boot_exit(void)
{
    cancel_delayed_work_sync(&revert_work);
    set_governor_all(GOV_NORMAL);
    pr_info("sandevistan_boot: unloaded\n");
}

module_init(sandevistan_boot_init);
module_exit(sandevistan_boot_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Sandevistan Boot — temporary performance governor boost for faster boot");