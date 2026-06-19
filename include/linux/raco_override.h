/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/linux/raco_override.h
 * Raco Universal RC Override API — public header
 * Author: Kanagawa Yamada
 */

#ifndef _RACO_RC_OVERRIDE_H
#define _RACO_RC_OVERRIDE_H

typedef void (*raco_enforce_cb_t)(void);

int raco_register_rc_override(raco_enforce_cb_t enforce_cb, const char *name);

int raco_unregister_rc_override(raco_enforce_cb_t enforce_cb);

#endif /* _RACO_RC_OVERRIDE_H */