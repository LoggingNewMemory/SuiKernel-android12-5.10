// SPDX-License-Identifier: GPL-3.0-only
/*
 * drivers/Yamada/yamada_is_angry.c
 * Blocklist some devices from using SuiKernel
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/string.h>

/* 
 * List of devices that are not allowed to boot SuiKernel.
 * Matches substrings in /firmware/android/serialno from the device tree.
 */
static const char *const blocklisted_devices[] = {
	"X6882", /* Infinix Hot 50 4G */
	NULL
};

/* Helper for case-insensitive substring search */
static const char *yamada_strcasestr(const char *s1, const char *s2)
{
	size_t l1, l2;

	if (!s1 || !s2)
		return NULL;

	l2 = strlen(s2);
	if (!l2)
		return s1;
	l1 = strlen(s1);
	while (l1 >= l2) {
		if (!strncasecmp(s1, s2, l2))
			return s1;
		s1++;
		l1--;
	}
	return NULL;
}

static int __init yamada_is_angry_init(void)
{
	const char *serialno = NULL;
	struct device_node *np;
	int i = 0;

	np = of_find_node_by_path("/firmware/android");
	if (np) {
		of_property_read_string(np, "serialno", &serialno);
		of_node_put(np);
	}

	if (!serialno)
		return 0;

	while (blocklisted_devices[i] != NULL) {
		if (yamada_strcasestr(serialno, blocklisted_devices[i])) {
			pr_emerg("========================================================\n");
			pr_emerg(" SuiKernel is explicitly BLOCKED on this device (%s)!\n",
				 blocklisted_devices[i]);
			pr_emerg(" Device is in the Yamada's blocklist.\n");
			pr_emerg("========================================================\n");
			panic("SuiKernel: Blocklisted device detected! Boot aborted.");
		}
		i++;
	}

	return 0;
}

module_init(yamada_is_angry_init);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("SuiKernel Device Blocklist Driver");
