// SPDX-License-Identifier: GPL-3.0-only
// See the codes for the props used by spoofing

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pavolia_reine_setprop.h>

static int __init ayunda_risu_setprop(void)
{
    pavolia_reine_setprop("ro.boot.verifiedbootstate", "green");
    pavolia_reine_setprop("ro.boot.veritymode", "enforcing");
    pavolia_reine_setprop("vendor.boot.vbmeta.device_state", "locked");
    pavolia_reine_setprop("ro.crypto.state", "encrypted");
    pavolia_reine_setprop("ro.secureboot.lockstate", "locked");
    pavolia_reine_setprop("ro.boot.flash.locked", "1");
    pavolia_reine_setprop("ro.boot.vbmeta.device_state", "locked");
    pavolia_reine_setprop("ro.boot.selinux", "enforcing");
    pavolia_reine_setprop("sys.oem_unlock_allowed", "0");
    pavolia_reine_setprop("ro.boot.veritymode.managed", "yes");
    pavolia_reine_setprop("ro.boot.realmebootstate", "green");
    pavolia_reine_setprop("ro.boot.warranty_bit", "0");
    pavolia_reine_setprop("ro.vendor.boot.warranty_bit", "0");
    pavolia_reine_setprop("ro.vendor.warranty_bit", "0");
    pavolia_reine_setprop("ro.warranty_bit", "0");
    pavolia_reine_setprop("ro.boot.realme.lockstate", "1");
    pavolia_reine_setprop("vendor.boot.verifiedbootstate", "green");

    return 0;
}

module_init(ayunda_risu_setprop);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Spoof some props for reduced root detection");