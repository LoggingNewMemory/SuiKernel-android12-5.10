#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/security.h>
#include <linux/slab.h>

extern void yamada_ksu_init_selinux_hooks(void);
extern void yamada_ksu_init_runtime_hooks(void);
extern void yamada_ksu_exit_selinux_hooks(void);
extern void yamada_ksu_exit_runtime_hooks(void);
extern void yamada_ksu_init_stat_hooks(void);
extern void yamada_ksu_exit_stat_hooks(void);

static int __init yamada_ksu_core_init(void)
{
    pr_info("YamadaKSUCore: Initializing minimal KernelSU features...\n");
    yamada_ksu_init_selinux_hooks();
    yamada_ksu_init_runtime_hooks();
    yamada_ksu_init_stat_hooks();
    return 0;
}

static void __exit yamada_ksu_core_exit(void)
{
    yamada_ksu_exit_stat_hooks();
    yamada_ksu_exit_runtime_hooks();
    yamada_ksu_exit_selinux_hooks();
    pr_info("YamadaKSUCore: Exited.\n");
}

module_init(yamada_ksu_core_init);
module_exit(yamada_ksu_core_exit);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Yamada Minimal KernelSU Core");
