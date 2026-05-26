/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/linux/raco_override.h
 * Raco Universal RC Override API — public header
 * Author: Kanagawa Yamada
 */

#ifndef _RACO_RC_OVERRIDE_H
#define _RACO_RC_OVERRIDE_H

#include <linux/atomic.h>

int raco_register_rc_override(atomic_t *target_ptr, int desired_val,
			      const char *name);

int raco_unregister_rc_override(atomic_t *target_ptr);

#endif /* _RACO_RC_OVERRIDE_H */