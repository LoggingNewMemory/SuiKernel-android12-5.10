#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>

extern void yamada_ksu_init_selinux_hooks(void);
extern void yamada_ksu_exit_selinux_hooks(void);
extern void ksu_throne_tracker_init(void);
extern void ksu_throne_tracker_exit(void);

char pavolia_rc_buf[4096] = {0};
size_t pavolia_rc_len = 0;

void ksu_pavolia_add_prop(const char *prop, const char *val)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "\non property:sys.boot_completed=1\n    setprop %s \"%s\"\n", prop, val);
	strlcat(pavolia_rc_buf, buf, sizeof(pavolia_rc_buf));
	pavolia_rc_len = strlen(pavolia_rc_buf);
}
EXPORT_SYMBOL(ksu_pavolia_add_prop);

static int __init yamada_ksu_core_init(void)
{
	pr_info("YamadaKSUCore: Initializing minimal KernelSU features...\n");
	yamada_ksu_init_selinux_hooks();
	ksu_throne_tracker_init();
	return 0;
}

static void __exit yamada_ksu_core_exit(void)
{
	ksu_throne_tracker_exit();
	yamada_ksu_exit_selinux_hooks();
	pr_info("YamadaKSUCore: Exited.\n");
}

module_init(yamada_ksu_core_init);
module_exit(yamada_ksu_core_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Minimal KernelSU Core");
