// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/bootconfig - Extra boot configuration
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bootconfig.h>
#include <linux/slab.h>
#include <linux/string.h>

static char *saved_boot_config;

#ifdef CONFIG_VESTIA_ZETA_SPOOF
#include <linux/sched.h>
static void safe_replace(char *str, const char *old_str, const char *new_str)
{
	char *pos;
	while ((pos = strstr(str, old_str))) {
		size_t old_len = strlen(old_str);
		size_t new_len = strlen(new_str);
		size_t tail_len = strlen(pos + old_len);
		memmove(pos + new_len, pos + old_len, tail_len + 1);
		memcpy(pos, new_str, new_len);
	}
}
#endif

static int boot_config_proc_show(struct seq_file *m, void *v)
{
	if (saved_boot_config) {
#ifdef CONFIG_VESTIA_ZETA_SPOOF
		char *b;
		bool is_init = (current->pid == 1 || strstr(current->comm, "init") || strstr(current->comm, "ueventd") || strstr(current->comm, "vold"));
		bool is_recovery = (strstr(current->comm, "recovery") || strstr(current->comm, "twrp") || strstr(current->comm, "orangefox") || strstr(current->comm, "pitchblack") || strstr(current->comm, "shrp"));

		if (is_recovery) {
			seq_puts(m, saved_boot_config);
			return 0;
		}

		b = kmalloc(strlen(saved_boot_config) + 256, GFP_KERNEL);
		if (!b) {
			seq_puts(m, saved_boot_config);
			return 0;
		}
		strcpy(b, saved_boot_config);

		safe_replace(b, "verifiedbootstate = \"orange\"", "verifiedbootstate = \"green\"");
		safe_replace(b, "device_state = \"unlocked\"", "device_state = \"locked\"");
		safe_replace(b, "flash.locked = \"0\"", "flash.locked = \"1\"");

		if (!is_init) {
			safe_replace(b, "veritymode = \"logging\"", "veritymode = \"enforcing\"");
			safe_replace(b, "veritymode = \"disabled\"", "veritymode = \"enforcing\"");
		}

		seq_puts(m, b);
		kfree(b);
#else
		seq_puts(m, saved_boot_config);
#endif
	}
	return 0;
}

/* Rest size of buffer */
#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

/* Return the needed total length if @size is 0 */
static int __init copy_xbc_key_value_list(char *dst, size_t size)
{
	struct xbc_node *leaf, *vnode;
	char *key, *end = dst + size;
	const char *val;
	char q;
	int ret = 0;

	key = kzalloc(XBC_KEYLEN_MAX, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	xbc_for_each_key_value(leaf, val) {
		ret = xbc_node_compose_key(leaf, key, XBC_KEYLEN_MAX);
		if (ret < 0)
			break;
		ret = snprintf(dst, rest(dst, end), "%s = ", key);
		if (ret < 0)
			break;
		dst += ret;
		vnode = xbc_node_get_child(leaf);
		if (vnode) {
			xbc_array_for_each_value(vnode, val) {
				if (strchr(val, '"'))
					q = '\'';
				else
					q = '"';
				ret = snprintf(dst, rest(dst, end), "%c%s%c%s",
					q, val, q, xbc_node_is_array(vnode) ? ", " : "\n");
				if (ret < 0)
					goto out;
				dst += ret;
			}
		} else {
			ret = snprintf(dst, rest(dst, end), "\"\"\n");
			if (ret < 0)
				break;
			dst += ret;
		}
	}
out:
	kfree(key);

	return ret < 0 ? ret : dst - (end - size);
}

static int __init proc_boot_config_init(void)
{
	int len;

	len = copy_xbc_key_value_list(NULL, 0);
	if (len < 0)
		return len;

	if (len > 0) {
		saved_boot_config = kzalloc(len + 1, GFP_KERNEL);
		if (!saved_boot_config)
			return -ENOMEM;

		len = copy_xbc_key_value_list(saved_boot_config, len + 1);
		if (len < 0) {
			kfree(saved_boot_config);
			return len;
		}
	}

	proc_create_single("bootconfig", 0, NULL, boot_config_proc_show);

	return 0;
}
fs_initcall(proc_boot_config_init);
