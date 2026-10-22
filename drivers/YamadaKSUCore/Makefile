obj-$(CONFIG_YAMADA_KSU_CORE) += yamada_ksu_core.o

yamada_ksu_core-y := core.o selinux/rules.o manager/apk_sign.o manager/throne_tracker.o manager/pkg_observer.o

ccflags-y += -I$(srctree)/drivers/YamadaKSUCore -I$(srctree)/drivers/YamadaKSUCore/include
ccflags-y += -DKSU_PER_USER_RANGE=100000 -DKSU_INVALID_APPID=-1
ccflags-y += -Wno-strict-prototypes -Wno-int-conversion -Wno-gcc-compat -Wno-missing-prototypes -Wno-missing-declarations
ccflags-y += -Wno-declaration-after-statement -Wno-unused-function -Wno-unused-variable
