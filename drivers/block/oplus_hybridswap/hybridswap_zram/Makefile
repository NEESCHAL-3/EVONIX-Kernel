# SPDX-License-Identifier: GPL-2.0-only

GCOV_PROFILE := y
obj-$(CONFIG_HYBRIDSWAP_ZRAM) += oplus_bsp_hybridswap_zram.o

obj-$(CONFIG_CRYPTO_ZSTDN) += zstd/
obj-$(CONFIG_CRYPTO_LZ4K) += lz4k/

oplus_bsp_hybridswap_zram-y	:=	zcomp.o zram_drv.o
oplus_bsp_hybridswap_zram-$(CONFIG_KCOMPRESSD)	+=  kcompressd.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP) += hybridswap/hybridmain.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_SWAPD) += hybridswap/hybridswapd.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_CORE) += hybridswap/hybridswap.o

obj-$(CONFIG_HYBRID_ZSMALLOC)	+= hybrid_zsmalloc.o
