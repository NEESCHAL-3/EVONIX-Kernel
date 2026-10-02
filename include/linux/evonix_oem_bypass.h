/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EVONIX_OEM_BYPASS_H
#define _LINUX_EVONIX_OEM_BYPASS_H
#include <linux/types.h>

/* Built-in only: no new vendor-module ABI or userspace entry point. */
int evonix_oem_supply_read(const char *supply, const char *name, int *value);
int evonix_oem_navigation_set(bool enabled);
#endif
