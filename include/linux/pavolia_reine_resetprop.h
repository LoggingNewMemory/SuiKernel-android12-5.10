#ifndef _PAVOLIA_REINE_SETPROP_H
#define _PAVOLIA_REINE_SETPROP_H

/**
 * pavolia_reine_resetprop - Queue an Android property to be set via usermodehelper
 * @prop: The property name (e.g. "debug.graphics.game_default_frame_rate.disabled")
 * @val: The property value (e.g. "true")
 *
 * Returns 0 on success, or negative error code.
 */
int pavolia_reine_resetprop(const char *prop, const char *val);

#endif /* _PAVOLIA_REINE_SETPROP_H */
