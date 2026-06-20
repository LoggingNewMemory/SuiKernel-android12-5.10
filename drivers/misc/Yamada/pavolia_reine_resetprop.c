// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/pavolia_reine_resetprop.c
 * Pavolia Reine Setprop Engine - Inject Android properties from kernel
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/pavolia_reine_resetprop.h>

#include <linux/list.h>
#include <linux/spinlock.h>

struct pavolia_prop_job {
	char prop[92]; /* Android property name maximum length is 92 */
	char val[92];  /* Android property value maximum length is 92 */
	struct list_head list;
};

static LIST_HEAD(pavolia_prop_list);
static DEFINE_SPINLOCK(pavolia_lock);
static void pavolia_reine_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(pavolia_dwork, pavolia_reine_work_fn);

static void pavolia_reine_work_fn(struct work_struct *work)
{
	struct pavolia_prop_job *job;
	unsigned long flags;
	struct file *f;
	int ret;

	/* Kobo's Magic Environment Variables! Wajib buat Android Binaries 🪄 */
	char *envp[] = { 
		"HOME=/", 
		"PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin", 
		"ANDROID_ROOT=/system", 
		"ANDROID_DATA=/data", 
		NULL 
	};

	f = filp_open("/data/adb/ksud", O_RDONLY, 0);
	if (IS_ERR(f)) {
		schedule_delayed_work(&pavolia_dwork, msecs_to_jiffies(2000));
		return;
	}
	filp_close(f, NULL);

	spin_lock_irqsave(&pavolia_lock, flags);
	if (list_empty(&pavolia_prop_list)) {
		spin_unlock_irqrestore(&pavolia_lock, flags);
		return;
	}

	job = list_first_entry(&pavolia_prop_list, struct pavolia_prop_job, list);
	spin_unlock_irqrestore(&pavolia_lock, flags);

	/* Tembak ksud langsung pake Array, bebas bug kutip/spasi! 🔫 */
	char *test_argv[] = { "/data/adb/ksud", "resetprop", "-n", job->prop, job->val, NULL };
	ret = call_usermodehelper(test_argv[0], test_argv, envp, UMH_WAIT_PROC);

	if (ret != 0) {
		schedule_delayed_work(&pavolia_dwork, msecs_to_jiffies(2000));
		return;
	}

	pr_info("pavolia_reine: Android property service online! Processing queue...\n");

	while (1) {
		spin_lock_irqsave(&pavolia_lock, flags);
		if (list_empty(&pavolia_prop_list)) {
			spin_unlock_irqrestore(&pavolia_lock, flags);
			break;
		}
		job = list_first_entry(&pavolia_prop_list, struct pavolia_prop_job, list);
		list_del(&job->list);
		spin_unlock_irqrestore(&pavolia_lock, flags);

		/* Bikin argv baru buat tiap iterasi */
		char *loop_argv[] = { "/data/adb/ksud", "resetprop", "-n", job->prop, job->val, NULL };
		call_usermodehelper(loop_argv[0], loop_argv, envp, UMH_WAIT_PROC);
		
		pr_info("pavolia_reine: Injected -> %s = %s\n", job->prop, job->val);
		kfree(job);
	}
	
	pr_info("pavolia_reine: All queued properties successfully injected!\n");
}

/**
 * pavolia_reine_resetprop - Queues an Android property injection
 * @prop: Property name
 * @val: Property value
 * 
 * Injects it after a 30 second delay to ensure /system is mounted
 * and Android's init property service is running.
 */
int pavolia_reine_resetprop(const char *prop, const char *val)
{
	struct pavolia_prop_job *job;

	unsigned long flags;

	if (!prop || !val)
		return -EINVAL;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;

	strscpy(job->prop, prop, sizeof(job->prop));
	strscpy(job->val, val, sizeof(job->val));

	/* Add to our global queue */
	spin_lock_irqsave(&pavolia_lock, flags);
	list_add_tail(&job->list, &pavolia_prop_list);
	spin_unlock_irqrestore(&pavolia_lock, flags);

	/* 
	 * Kick the single worker thread. It will poll every 2s until success,
	 * then blast through the entire queue of 100+ items instantly.
	 */
	schedule_delayed_work(&pavolia_dwork, msecs_to_jiffies(5000));

	return 0;
}
EXPORT_SYMBOL_GPL(pavolia_reine_resetprop);

static int __init pavolia_reine_init(void)
{
	pr_info("pavolia_reine: Android Property Injection API initialized.\n");
	return 0;
}
late_initcall(pavolia_reine_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Pavolia Reine Android Property Injector");
