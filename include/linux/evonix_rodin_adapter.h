/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_EVONIX_RODIN_ADAPTER_H
#define _LINUX_EVONIX_RODIN_ADAPTER_H
#include <linux/kconfig.h>
#include <linux/types.h>
#if IS_ENABLED(CONFIG_EVONIX_RODIN_ADAPTER_CLASS)
bool evonix_rodin_adapter_class_ready(void);
#else
static inline bool evonix_rodin_adapter_class_ready(void) { return false; }
#endif
#endif
