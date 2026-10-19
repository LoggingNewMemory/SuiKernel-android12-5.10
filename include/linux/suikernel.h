/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * include/linux/suikernel.h
 *
 * SuiKernel Global Manager & Storage Helpers
 * Author: Kanagawa Yamada
 */

#ifndef _LINUX_SUIKERNEL_H
#define _LINUX_SUIKERNEL_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/kernel.h>

#define SUI_MANAGER_PKG "kanagawa.yamada.suikernel.manager"

/**
 * sui_data_is_ready - Check if /data filesystem is mounted and accessible
 */
static inline bool sui_data_is_ready(void)
{
	struct file *f;

	f = filp_open("/data/system", O_RDONLY, 0);
	if (!IS_ERR(f)) {
		filp_close(f, NULL);
		return true;
	}
	f = filp_open("/data/data", O_RDONLY, 0);
	if (!IS_ERR(f)) {
		filp_close(f, NULL);
		return true;
	}
	return false;
}

/**
 * sui_manager_is_installed - Check if SuiKernel Manager app is installed
 */
static inline bool sui_manager_is_installed(void)
{
	struct file *f;

	f = filp_open("/data/data/" SUI_MANAGER_PKG, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		filp_close(f, NULL);
		return true;
	}
	f = filp_open("/data/user_de/0/" SUI_MANAGER_PKG, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		filp_close(f, NULL);
		return true;
	}
	return false;
}

/**
 * sui_read_raw_file - Read any file into a buffer and strip trailing newline
 */
static inline int sui_read_raw_file(const char *path, char *buf, size_t size)
{
	struct file *f;
	loff_t pos = 0;
	int ret;

	if (!path || !buf || size == 0)
		return -1;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -1;

	ret = kernel_read(f, buf, size - 1, &pos);
	filp_close(f, NULL);

	if (ret > 0) {
		if (buf[ret - 1] == '\n')
			buf[ret - 1] = '\0';
		else
			buf[ret] = '\0';
	} else {
		ret = -1;
	}

	return ret;
}

/**
 * sui_read_config - Read a config file from /data/SuiKernel/<feature>
 */
static inline int sui_read_config(const char *feature, char *buf, size_t size)
{
	char path[64];

	if (!feature)
		return -1;

	snprintf(path, sizeof(path), "/data/SuiKernel/%s", feature);
	return sui_read_raw_file(path, buf, size);
}

/**
 * sui_is_feature_enabled - Check if a SuiKernel feature is enabled
 * @feature: Config filename (e.g. "tenebrion", "inaho", "sandevistan")
 *
 * Rules:
 * - If Manager is NOT installed (or was uninstalled): returns true (default enabled).
 * - If Manager IS installed: checks /data/SuiKernel/<feature>.
 *   If '0' -> disabled (returns false).
 *   Otherwise -> enabled (returns true).
 */
static inline bool sui_is_feature_enabled(const char *feature)
{
	char cfg[16];

	if (!sui_manager_is_installed())
		return true;

	if (sui_read_config(feature, cfg, sizeof(cfg)) > 0) {
		if (cfg[0] == '0')
			return false;
	}

	return true;
}

/**
 * sui_read_int_config - Read an integer config with defaults and clamping
 */
static inline int sui_read_int_config(const char *feature, int default_val, int min_val, int max_val)
{
	char cfg[16];
	int val = default_val;

	if (!sui_manager_is_installed())
		return default_val;

	if (sui_read_config(feature, cfg, sizeof(cfg)) > 0) {
		if (kstrtoint(cfg, 10, &val) == 0) {
			if (val < min_val)
				val = min_val;
			else if (val > max_val)
				val = max_val;
			return val;
		}
	}

	return default_val;
}

/**
 * sui_get_touch_boost_mode - Get Yamada Touch Boost mode (0=off, 1=balanced, 2=gaming)
 */
static inline int sui_get_touch_boost_mode(void)
{
	char cfg[16];

	if (!sui_manager_is_installed())
		return 1; /* Default: Balanced */

	if (sui_read_config("touch_boost", cfg, sizeof(cfg)) > 0) {
		if (cfg[0] == '0')
			return 0; /* Disabled */
		if (cfg[0] == '2')
			return 2; /* Gaming */
	}

	return 1; /* Balanced */
}

#endif /* _LINUX_SUIKERNEL_H */
