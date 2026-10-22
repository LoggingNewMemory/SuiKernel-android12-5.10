#ifndef __KSU_POLICY_ALLOWLIST_H
#define __KSU_POLICY_ALLOWLIST_H

#include <linux/list.h>

static inline void ksu_prune_allowlist(struct list_head *uid_list)
{
	// No-op for minimal YamadaKSUCore
}

#endif
