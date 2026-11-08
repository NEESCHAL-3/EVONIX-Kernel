# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_HYBRID_ZSMALLOC) += zsmalloc.o
zsmalloc-y := thp_zsmalloc/zsmalloc.o
obj-$(CONFIG_EVONIX_OPLUS_HYBRIDSWAP_BUILTIN) += mtk_disp_notify.o
obj-$(CONFIG_HYBRIDSWAP_ZRAM) += zram.o
zram-y := hybridswap_zram/zcomp.o hybridswap_zram/zram_drv.o
zram-$(CONFIG_HYBRIDSWAP) += hybridswap_zram/hybridswap/hybridmain.o hybridswap_zram/hybridswap/display.o
zram-$(CONFIG_HYBRIDSWAP_SWAPD) += hybridswap_zram/hybridswap/hybridswapd.o
zram-$(CONFIG_HYBRIDSWAP_CORE) += hybridswap_zram/hybridswap/hybridswap.o
obj-$(CONFIG_CRYPTO_ZSTDN) += hybridswap_zram/zstd/
obj-$(CONFIG_CRYPTO_LZ4K) += hybridswap_zram/lz4k/
ccflags-y += -I$(srctree)/$(src) -I$(srctree)/$(src)/rodin-headers -DCONFIG_OPLUS_MTK_DRM_GKI_NOTIFY
