#ifndef __KSU_POLICY_ALLOWLIST_H
#define __KSU_POLICY_ALLOWLIST_H

#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/list.h>
#include "uapi/app_profile.h"

#define PER_USER_RANGE 100000

static inline void ksu_prune_allowlist(bool (*is_uid_exist)(uid_t, char *, void *), void *data)
{
	// No-op for minimal YamadaKSUCore
}

#endif
