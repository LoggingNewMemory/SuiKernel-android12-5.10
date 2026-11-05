// SPDX-License-Identifier: GPL-3.0-only
// drivers/Kana/yamada_dev_identification.c
// SuiKernel Developer Identification
// Author: Kana Wesley Yamada

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pavolia_reine_setprop.h>

static int __init yamada_dev_ident_init(void)
{
	pavolia_reine_setprop("suikernel.developer.is", "Kana Wesley Yamada");
	pavolia_reine_setprop("hoshimachi.suisei", "Stellar Stellar");

	return 0;
}

module_init(yamada_dev_ident_init);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kana Wesley Yamada");
MODULE_DESCRIPTION("SuiKernel Developer Identification");