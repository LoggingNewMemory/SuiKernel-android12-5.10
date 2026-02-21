// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/slab.h>

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	char *cmdline_copy;
	char *pos;
	
	cmdline_copy = kstrdup(saved_command_line, GFP_KERNEL);
	if (!cmdline_copy) {
		/* Fallback if allocation fails */
		seq_puts(m, saved_command_line);
		seq_putc(m, '\n');
		return 0;
	}

	while ((pos = strstr(cmdline_copy, "androidboot.verifiedbootstate=orange")) != NULL) {
		strncpy(pos, "androidboot.verifiedbootstate=green ", 36);
	}
	
	while ((pos = strstr(cmdline_copy, "androidboot.vbmeta.device_state=unlocked")) != NULL) {
		strncpy(pos, "androidboot.vbmeta.device_state=locked  ", 40);
	}

	seq_puts(m, cmdline_copy);
	seq_putc(m, '\n');
	
	kfree(cmdline_copy);
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
