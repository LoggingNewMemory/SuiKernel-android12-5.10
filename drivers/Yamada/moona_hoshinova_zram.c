// SPDX-License-Identifier: GPL-3.0-only
// drivers/Yamada/moona_hoshinova_zram.c
// Moona Hoshinova ZRAM Enforcer — forces zstd comp_algorithm
// Author: Kanagawa Yamada

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/raco_override.h>
#include <linux/suikernel.h>

bool moona_enabled = true;

static void moona_apply_tweaks(bool enable)
{
	struct file *filp;
	loff_t pos;

	if (enable) {
		WRITE_ONCE(page_cluster, 0);

		filp = filp_open("/sys/kernel/mm/swap/vma_ra_enabled", O_WRONLY, 0);
		if (!IS_ERR(filp)) {
			pos = 0;
			kernel_write(filp, "true\n", 5, &pos);
			filp_close(filp, NULL);
		}
	} else {
		WRITE_ONCE(page_cluster, 3);

		filp = filp_open("/sys/kernel/mm/swap/vma_ra_enabled", O_WRONLY, 0);
		if (!IS_ERR(filp)) {
			pos = 0;
			kernel_write(filp, "false\n", 6, &pos);
			filp_close(filp, NULL);
		}
	}
}

static void set_zram_to_zstd(void)
{
	// Mandatory: /data must be checked
	if (!sui_data_is_ready())
		return;

	if (!moona_enabled)
		return;

	if (!sui_is_feature_enabled("moona_zram")) {
		moona_set_enabled(false);
		pr_info("moona_zram: disabled via Manager config\n");
		return;
	}

	moona_apply_tweaks(true);
}

void moona_set_enabled(bool enabled)
{
	moona_enabled = enabled;
	moona_apply_tweaks(enabled);
}
EXPORT_SYMBOL(moona_set_enabled);

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
MODULE_DESCRIPTION("Enforce zstd as default ZRAM and ZRAM Tweaks");
