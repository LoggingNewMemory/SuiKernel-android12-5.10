#include <linux/kprobes.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dcache.h>
#include <linux/string.h>
#include <linux/workqueue.h>

char pavolia_rc_buf[4096] = {0};
size_t pavolia_rc_len = 0;
ssize_t pavolia_rc_pos = 0;

void ksu_pavolia_add_prop(const char *prop, const char *val) {
    char buf[256];
    snprintf(buf, sizeof(buf), "\non property:sys.boot_completed=1\n    exec u:r:su:s0 root root -- /resetprop -n %s \"%s\"\n", prop, val);
    strlcat(pavolia_rc_buf, buf, sizeof(pavolia_rc_buf));
    pavolia_rc_len = strlen(pavolia_rc_buf);
}
EXPORT_SYMBOL(ksu_pavolia_add_prop);

static ssize_t (*orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*orig_read_iter)(struct kiocb *, struct iov_iter *);
static struct file_operations fops_proxy;

static ssize_t read_proxy(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret = orig_read(file, buf, count, pos);
    if (ret != 0) return ret;

    if (pavolia_rc_pos < pavolia_rc_len) {
        size_t append_count = pavolia_rc_len - pavolia_rc_pos;
        if (append_count > count) append_count = count;
        if (copy_to_user(buf, pavolia_rc_buf + pavolia_rc_pos, append_count)) return ret;
        pavolia_rc_pos += append_count;
        ret += append_count;
    }
    return ret;
}

static ssize_t read_iter_proxy(struct kiocb *iocb, struct iov_iter *to)
{
    ssize_t ret = orig_read_iter(iocb, to);
    if (ret != 0) return ret;

    if (pavolia_rc_pos < pavolia_rc_len) {
        size_t append_count = copy_to_iter(pavolia_rc_buf + pavolia_rc_pos, pavolia_rc_len - pavolia_rc_pos, to);
        if (!append_count) return ret;
        pavolia_rc_pos += append_count;
        ret += append_count;
    }
    return ret;
}

static struct kprobe vfs_read_kp;
static struct kretprobe vfs_getattr_kp;
extern void yamada_ksu_exit_runtime_hooks(void);
extern void yamada_ksu_exit_stat_hooks(void);

static void do_unhook_work(struct work_struct *work)
{
    yamada_ksu_exit_runtime_hooks();
    yamada_ksu_exit_stat_hooks();
}
static DECLARE_WORK(unhook_work, do_unhook_work);

static int vfs_read_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    static bool hooked = false;
    struct file *file;
    if (hooked) return 0;
    
    if (strcmp(current->comm, "init") != 0) return 0;
    
#ifdef CONFIG_ARM64
    file = (struct file *)regs->regs[0];
#else
    file = (struct file *)regs_get_kernel_argument(regs, 0);
#endif

    if (!file || !file->f_path.dentry) return 0;
    
    if (strcmp(file->f_path.dentry->d_name.name, "init.rc") == 0) {
        hooked = true;
        memcpy(&fops_proxy, file->f_op, sizeof(struct file_operations));
        orig_read = file->f_op->read;
        if (orig_read) fops_proxy.read = read_proxy;
        orig_read_iter = file->f_op->read_iter;
        if (orig_read_iter) fops_proxy.read_iter = read_iter_proxy;
        
        file->f_op = &fops_proxy;
        pr_info("YamadaKSUCore: init.rc read hooked! Pavolia Reine integration active.\n");
        
        // Auto unregister kprobes because we only need to hook init.rc during early boot!
        // This leaves ZERO kprobes in memory when Android starts up, ensuring maximum stealth.
        schedule_work(&unhook_work);
    }
    return 0;
}

static struct kprobe vfs_read_kp = {
    .symbol_name = "vfs_read",
    .pre_handler = vfs_read_pre_handler,
};

static int runtime_hooked = 0;

void yamada_ksu_init_runtime_hooks(void)
{
    if (register_kprobe(&vfs_read_kp) == 0) {
        runtime_hooked = 1;
    }
}

void yamada_ksu_exit_runtime_hooks(void)
{
    if (runtime_hooked) {
        unregister_kprobe(&vfs_read_kp);
        runtime_hooked = 0;
    }
}

struct statx_kp_data {
    struct kstat *stat;
    struct path *path;
};

static int vfs_getattr_entry(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct statx_kp_data *data = (struct statx_kp_data *)ri->data;
#ifdef CONFIG_ARM64
    data->path = (struct path *)regs->regs[0];
    data->stat = (struct kstat *)regs->regs[1];
#else
    data->path = (struct path *)regs_get_kernel_argument(regs, 0);
    data->stat = (struct kstat *)regs_get_kernel_argument(regs, 1);
#endif
    return 0;
}

static int vfs_getattr_ret(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct statx_kp_data *data = (struct statx_kp_data *)ri->data;
    if (regs_return_value(regs) != 0) return 0;
    if (data->path && data->path->dentry && data->path->dentry->d_name.name) {
        if (strcmp(data->path->dentry->d_name.name, "init.rc") == 0) {
            if (strcmp(current->comm, "init") == 0 && data->stat) {
                data->stat->size += pavolia_rc_len;
            }
        }
    }
    return 0;
}

static struct kretprobe vfs_getattr_kp = {
    .handler = vfs_getattr_ret,
    .entry_handler = vfs_getattr_entry,
    .data_size = sizeof(struct statx_kp_data),
    .maxactive = 64,
    .kp = {
        .symbol_name = "vfs_getattr",
    },
};

static int stat_hooked = 0;

void yamada_ksu_init_stat_hooks(void)
{
    if (register_kretprobe(&vfs_getattr_kp) == 0) {
        stat_hooked = 1;
    }
}

void yamada_ksu_exit_stat_hooks(void)
{
    if (stat_hooked) {
        unregister_kretprobe(&vfs_getattr_kp);
        stat_hooked = 0;
    }
}
