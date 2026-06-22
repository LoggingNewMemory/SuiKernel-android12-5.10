// SPDX-License-Identifier: GPL-3.0-only
/*
See the codes for the props used by spoofing
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pavolia_reine_resetprop.h>

static int __init ayunda_risu_setprop(void)
{
    pavolia_reine_resetprop("ro.boot.verifiedbootstate", "green");
    pavolia_reine_resetprop("ro.boot.veritymode", "enforcing");
    pavolia_reine_resetprop("vendor.boot.vbmeta.device_state", "locked");
    pavolia_reine_resetprop("ro.crypto.state", "encrypted");
    pavolia_reine_resetprop("ro.secureboot.lockstate", "locked");
    pavolia_reine_resetprop("ro.boot.flash.locked", "1");
    pavolia_reine_resetprop("ro.boot.vbmeta.device_state", "locked");
    pavolia_reine_resetprop("ro.boot.selinux", "enforcing");
    pavolia_reine_resetprop("sys.oem_unlock_allowed", "0");
    pavolia_reine_resetprop("ro.boot.veritymode.managed", "yes");
    pavolia_reine_resetprop("ro.boot.realmebootstate", "green");
    pavolia_reine_resetprop("ro.boot.warranty_bit", "0");
    pavolia_reine_resetprop("ro.vendor.boot.warranty_bit", "0");
    pavolia_reine_resetprop("ro.vendor.warranty_bit", "0");
    pavolia_reine_resetprop("ro.warranty_bit", "0");
    pavolia_reine_resetprop("ro.boot.realme.lockstate", "1");
    pavolia_reine_resetprop("vendor.boot.verifiedbootstate", "green");

    return 0;
}

module_init(ayunda_risu_setprop);

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Kanagawa Yamada");
MODULE_DESCRIPTION("Spoof some props for reduced root detection")