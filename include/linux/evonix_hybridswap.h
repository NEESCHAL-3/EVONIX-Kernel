/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_EVONIX_HYBRIDSWAP_H
#define _LINUX_EVONIX_HYBRIDSWAP_H

#include <linux/types.h>

#ifdef CONFIG_EVONIX_OPLUS_HYBRIDSWAP_BUILTIN
bool evonix_hybridswap_allocator_ready(void);
bool evonix_hybridswap_zram_ready(void);
bool evonix_hybridswap_notifier_ready(void);
#endif

#endif
