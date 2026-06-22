// SPDX-License-Identifier: GPL-3.0-only
/*
 * drivers/misc/Yamada/yamada_dev_identification.c
 * SuiKernel Developer Identification
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pavolia_reine_resetprop.h>

static int __init yamada_dev_ident_init(void)
{
	pavolia_reine_resetprop("suikernel.developer.is", "Kanagawa Yamada");
	pavolia_reine_resetprop("hoshimachi.suisei", "Stellar Stellar");

	return 0;
}

module_init(yamada_dev_ident_init);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("SuiKernel Developer Identification");