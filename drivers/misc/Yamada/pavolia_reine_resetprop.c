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

extern void ksu_pavolia_add_prop(const char *prop, const char *val);

/**
 * pavolia_reine_resetprop - Queues an Android property injection
 * @prop: Property name
 * @val: Property value
 * * Injects it securely by passing it to KernelSU's init.rc hook,
 * which instructs Android init to naturally set the property during boot.
 */
int pavolia_reine_resetprop(const char *prop, const char *val)
{
	if (!prop || !val)
		return -EINVAL;

	/* Hook directly into KernelSU's internal init.rc stream proxy */
	ksu_pavolia_add_prop(prop, val);
	
	pr_info("pavolia_reine: Property hooked into KernelSU -> %s = %s\n", prop, val);

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