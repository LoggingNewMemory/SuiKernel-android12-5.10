// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/misc/Yamada/ayunda_risu_native_root_exec.c
 * Ayunda Risu Native Root Execution Engine
 *
 * Executes shell commands with full root privileges by injecting them into
 * KernelSU's init.rc RC stream as:
 *
 *   exec u:r:su:s0 root -- /system/bin/sh -c "<cmd>"
 *
 * This is the architecturally correct approach.  call_usermodehelper() runs in
 * the kernel's own SELinux domain (u:r:kernel:s0) which cannot transition into
 * a shell or SU domain — so the log would show "exit code 0" for the helper
 * itself but the child sh process would be denied by AVC and produce no effect.
 *
 * Commands queued via ayunda_risu_exec() before boot_completed use
 * ksu_ayunda_exec_once(), which appends to the RC buffer.  Commands that
 * arrive post-boot fall back to call_usermodehelper() with the KernelSU cred
 * override (effective only if called from a context where that is valid).
 *
 * Author: Kanagawa Yamada
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/atomic.h>

/*
 * ksu_ayunda_exec_once() is injected into KernelSU's ksud_integration.c by
 * PavoliaReinePatch.sh.  It appends a boot_completed init.rc stanza so the
 * command runs via  exec u:r:su:s0 root -- /system/bin/sh -c "<cmd>".
 *
 * This symbol is weak so the driver still compiles when KernelSU is absent
 * (e.g. in a standalone build or unit test).  Without KernelSU the RC-inject
 * path is unavailable and we fall through to the UMH fallback.
 */
extern void ksu_ayunda_exec_once(const char *cmd) __attribute__((weak));

/*
 * Set to 1 once the kernel detects that init.rc has been fully consumed.
 * At that point new commands can no longer be injected via the RC stream and
 * must go through a different path (UMH fallback / ksud IPC in userspace).
 *
 * Currently this flag is set by ayunda_risu_mark_rc_done(), which should be
 * called from the KernelSU boot_completed hook when present.
 */
static atomic_t rc_stream_closed = ATOMIC_INIT(0);

/**
 * ayunda_risu_mark_rc_done - Signal that the init.rc stream is no longer live.
 *
 * Call this from the KernelSU boot_completed handler so that subsequent
 * ayunda_risu_exec() calls know to use the UMH fallback path instead of
 * trying to inject into a stream that init has already finished reading.
 */
void ayunda_risu_mark_rc_done(void)
{
	atomic_set(&rc_stream_closed, 1);
	pr_info("ayunda_risu: init.rc stream closed, switching to UMH fallback\n");
}
EXPORT_SYMBOL_GPL(ayunda_risu_mark_rc_done);

/**
 * ayunda_risu_exec - Execute a shell command as root.
 * @cmd: The shell command string to execute.
 *
 * Pre-boot (init.rc stream still open):
 *   Calls ksu_ayunda_exec_once(cmd) to inject an init.rc stanza executed
 *   as:  exec u:r:su:s0 root -- /system/bin/sh -c "<cmd>"
 *   The return value is 0 on successful queueing, -ENOSYS if KernelSU is
 *   absent, or -EINVAL for bad arguments.
 *
 * Post-boot (init.rc stream closed):
 *   Falls back to call_usermodehelper().  Note that the UMH path runs in the
 *   kernel SELinux domain.  The SELinux rules in selinux.sh grant
 *   kernel → shell_exec execute + execute_no_trans so the helper binary can
 *   launch, but complex shell transitions may still be denied depending on the
 *   device policy.  For privileged post-boot operations prefer triggering ksud
 *   from userspace instead.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ayunda_risu_exec(const char *cmd)
{
	if (!cmd || !*cmd)
		return -EINVAL;

	pr_info("ayunda_risu: exec request: %s\n", cmd);

	/* ------------------------------------------------------------------ *
	 * Path 1 — inject into KernelSU init.rc stream (pre-boot)             *
	 * ------------------------------------------------------------------ */
	if (!atomic_read(&rc_stream_closed)) {
		if (ksu_ayunda_exec_once) {
			ksu_ayunda_exec_once(cmd);
			pr_info("ayunda_risu: queued via KernelSU RC stream\n");
			return 0;
		}
		pr_warn("ayunda_risu: ksu_ayunda_exec_once not available "
			"(KernelSU not installed?), falling back to UMH\n");
	}

	/* ------------------------------------------------------------------ *
	 * Path 2 — call_usermodehelper() fallback (post-boot or no KernelSU) *
	 *                                                                      *
	 * Runs UID 0 in kernel domain.  SELinux execute/execute_no_trans      *
	 * rules for kernel→shell_exec are injected by selinux.sh.             *
	 * ------------------------------------------------------------------ */
	{
		int ret;
		char *argv[] = { "/system/bin/sh", "-c", (char *)cmd, NULL };
		char *envp[] = {
			"HOME=/",
			"PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin",
			"ANDROID_ROOT=/system",
			"ANDROID_DATA=/data",
			NULL
		};

		pr_info("ayunda_risu: UMH fallback path for: %s\n", cmd);
		ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
		if (ret)
			pr_warn("ayunda_risu: UMH finished with exit code %d "
				"(SELinux denial likely if non-zero)\n", ret);
		else
			pr_info("ayunda_risu: UMH finished successfully\n");
		return ret;
	}
}
EXPORT_SYMBOL_GPL(ayunda_risu_exec);

static int __init ayunda_risu_init(void)
{
	pr_info("ayunda_risu: Native Root Execution Engine initialized.\n");
	pr_info("ayunda_risu: Using KernelSU RC-stream injection as primary path.\n");
	return 0;
}
late_initcall(ayunda_risu_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Ayunda Risu Native Root Exec — KernelSU RC injection engine");
