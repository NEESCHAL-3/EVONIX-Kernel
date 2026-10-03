# SPDX-License-Identifier: GPL-2.0
# Upstream a54e0dc6cf0aff4dd87fec49644a02d2eb612905, EVONIX 6.6 integration.
obj-$(CONFIG_BBG) += bbg.o
bbg-y := baseband_guard.o tracing/tracing.o blkdev_helper.o
ccflags-y += -I$(srctree)/security/selinux -I$(srctree)/security/selinux/include
ccflags-y += -I$(srctree)/block -I$(objtree)/security/selinux
ccflags-y += -I$(srctree)/security/baseband-guard
ccflags-y += -DBBG_USE_DEFINE_LSM -DBB_HAS_IOCTL_COMPAT
ccflags-y += -DBBG_VERSION=\"a54e0dc6-evonix1\"
ccflags-y += -DBBG_REPO=\"https://github.com/vc-teahouse/Baseband-guard\"
