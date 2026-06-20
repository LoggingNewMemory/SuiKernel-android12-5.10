// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/linux/ayunda_risu_native_root_exec.h
 * Ayunda Risu Native Root Execution Engine API
 * Author: Kanagawa Yamada
 */

#ifndef _LINUX_AYUNDA_RISU_NATIVE_ROOT_EXEC_H
#define _LINUX_AYUNDA_RISU_NATIVE_ROOT_EXEC_H

#include <linux/types.h>

/**
 * ayunda_risu_exec - Execute a shell command natively as root, bypassing SELinux
 * @cmd: The shell command to execute
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int ayunda_risu_exec(const char *cmd);

#endif /* _LINUX_AYUNDA_RISU_NATIVE_ROOT_EXEC_H */
