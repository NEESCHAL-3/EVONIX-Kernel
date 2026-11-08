# SPDX-License-Identifier: GPL-2.0-only
ifndef NDK
$(error Set NDK to the Android NDK directory)
endif
API ?= 35
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CC := $(TOOLCHAIN)/aarch64-linux-android$(API)-clang
CFLAGS := -O2 -Wall -Wextra -Werror
all: rodin_nandswap
rodin_nandswap: rodin_nandswap.c
	$(CC) $(CFLAGS) $< -o $@
.PHONY: all
