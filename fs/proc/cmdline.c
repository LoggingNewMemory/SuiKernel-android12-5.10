// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

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

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_VESTIA_ZETA_SPOOF
	char *c;
	bool is_init = (current->pid == 1 || strstr(current->comm, "init") || strstr(current->comm, "ueventd") || strstr(current->comm, "vold"));
	bool is_recovery = (strstr(current->comm, "recovery") || strstr(current->comm, "twrp"));

	if (is_recovery) {
		seq_puts(m, saved_command_line);
		seq_putc(m, '\n');
		return 0;
	}

	c = kmalloc(strlen(saved_command_line) + 256, GFP_KERNEL);
	if (!c) {
		seq_puts(m, saved_command_line);
		seq_putc(m, '\n');
		return 0;
	}
	strcpy(c, saved_command_line);

	safe_replace(c, "verifiedbootstate=orange", "verifiedbootstate=green");
	safe_replace(c, "verifiedbootstate=red", "verifiedbootstate=green");
	safe_replace(c, "flash.locked=0", "flash.locked=1");

	if (!is_init) {
		safe_replace(c, "veritymode=logging", "veritymode=enforcing");
		safe_replace(c, "veritymode=disabled", "veritymode=enforcing");
	}

	seq_puts(m, c);
	seq_putc(m, '\n');
	kfree(c);
#else
	seq_puts(m, saved_command_line);
	seq_putc(m, '\n');
#endif
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
