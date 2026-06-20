// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/linux/airani_iofifteen_susfs.h
 * Airani Iofifteen SUSFS Modifier API
 * Author: Kanagawa Yamada
 */

#ifndef _LINUX_AIRANI_IOFIFTEEN_SUSFS_H
#define _LINUX_AIRANI_IOFIFTEEN_SUSFS_H

#include <linux/types.h>

/**
 * airani_iofifteen_susfs_hide_path - Dynamically hides a specific path using SUSFS
 * @path: The absolute path to hide from userspace
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int airani_iofifteen_susfs_hide_path(const char *path);

/**
 * airani_iofifteen_susfs_hide_mount - Dynamically hides a specific mount using SUSFS
 * @path: The absolute mount path to hide from userspace
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int airani_iofifteen_susfs_hide_mount(const char *path);

#endif /* _LINUX_AIRANI_IOFIFTEEN_SUSFS_H */
