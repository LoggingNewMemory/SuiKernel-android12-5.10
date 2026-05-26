// include/linux/raco_override.h
// SPDX-License-Identifier: GPL-2.0-only
// Author: Kanagawa Yamada
// This to make a better implementation since in the future I will need a lot of init override method

#ifndef _RACO_RC_OVERRIDE_H
#define _RACO_RC_OVERRIDE_H

int raco_register_rc_override(int *target_ptr, int desired_val, const char *name);

#endif