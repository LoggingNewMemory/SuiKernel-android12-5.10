// SPDX-License-Identifier: GPL-3.0-only
/*
 * drivers/Yamada/moona_hoshinova_zram.c
 * Moona Hoshinova ZRAM Enforcer — forces zstd comp_algorithm
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/raco_override.h>

static void set_zram_to_zstd(void)
{
	struct file *filp;
	loff_t pos;

	filp = filp_open("/proc/sys/vm/page-cluster", O_WRONLY, 0);
	if (!IS_ERR(filp)) {
		pos = 0;
		kernel_write(filp, "0\n", 2, &pos);
		filp_close(filp, NULL);
	}

	filp = filp_open("/sys/kernel/mm/swap/vma_ra_enabled", O_WRONLY, 0);
	if (!IS_ERR(filp)) {
		pos = 0;
		kernel_write(filp, "true\n", 5, &pos);
		filp_close(filp, NULL);
	}
}

static int __init moona_hoshinova_zram_init(void)
{
	int err;

	err = raco_register_rc_override(set_zram_to_zstd, "moona_zram_flag");
	if (err) {
		pr_err("moona_zram: Failed to register with Raco!\n");
	} else {
		pr_info("moona_zram: Moona Hoshinova ZRAM Tweaks Applied\n");
	}

	return 0;
}
late_initcall(moona_hoshinova_zram_init);

MODULE_LICENSE("GPL v3");
MODULE_DESCRIPTION("Enforce zstd as default ZRAM");
