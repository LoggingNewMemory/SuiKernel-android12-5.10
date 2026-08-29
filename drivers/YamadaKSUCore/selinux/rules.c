#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <trace/hooks/avc.h>

struct selinux_state;
struct av_decision {
	u32 allowed;
	u32 auditallow;
	u32 auditdeny;
	u32 seqno;
	u32 flags;
};
struct policydb {};

struct avc_node {
    struct {
        u32 ssid;
        u32 tsid;
        u16 tclass;
        struct av_decision avd;
    } ae;
};

// Dummy struct for sed replacement
struct {
    struct policydb *policy;
} fake_selinux_state;
#define selinux_state fake_selinux_state
#undef rcu_assign_pointer
#define rcu_assign_pointer(ptr, val) do { (ptr) = (val); } while (0)
static struct policydb fake_pol;
static struct policydb *pol = &fake_pol;

struct ksu_rule {
    const char *s;
    const char *t;
    const char *c;
    const char *p;
};
static struct ksu_rule ksu_rules[128];
static int ksu_rules_count = 0;

void ksu_allow(void *db, const char *s, const char *t, const char *c, const char *p)
{
    if (ksu_rules_count < 128) {
        ksu_rules[ksu_rules_count].s = s;
        ksu_rules[ksu_rules_count].t = t;
        ksu_rules[ksu_rules_count].c = c;
        ksu_rules[ksu_rules_count].p = p;
        ksu_rules_count++;
    }
}
EXPORT_SYMBOL(ksu_allow);

static void my_avc_insert(void *data, const struct avc_node *node)
{
    if (ksu_rules_count > 0 && node->ae.ssid == 1) {
        // SECINITSID_KERNEL is 1. We grant all to kernel.
        struct av_decision *avd = (struct av_decision *)&node->ae.avd;
        avd->allowed = 0xffffffff;
    }
}

static int selinux_hooked = 0;

void yamada_ksu_init_selinux_hooks(void)
{
    void *db = NULL;
    // The sed script will inject ksu_allow calls here
    rcu_assign_pointer(selinux_state.policy, pol);
    
    if (register_trace_android_vh_selinux_avc_insert(my_avc_insert, NULL) == 0) {
        selinux_hooked = 1;
        pr_info("YamadaKSUCore: SELinux hooked via Vendor Hook (Tracepoint).\n");
    } else {
        pr_err("YamadaKSUCore: Failed to hook SELinux!\n");
    }
}

void yamada_ksu_exit_selinux_hooks(void)
{
    if (selinux_hooked) {
        unregister_trace_android_vh_selinux_avc_insert(my_avc_insert, NULL);
    }
}
#undef selinux_state
