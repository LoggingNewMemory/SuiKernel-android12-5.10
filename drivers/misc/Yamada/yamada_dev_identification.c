// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/Yamada/yamada_dev_identification.c
 * SuiKernel Developer Identification
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pavolia_reine_setprop.h>

static int __init yamada_dev_ident_init(void)
{
	pavolia_reine_setprop("suikernel.developer.is", "Kanagawa Yamada");
	pavolia_reine_setprop("hoshimachi.suisei", "Stellar Stellar");

	return 0;
}

static void __exit yamada_dev_ident_exit(void)
{
}

module_init(yamada_dev_ident_init);
module_exit(yamada_dev_ident_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("SuiKernel Developer Identification");