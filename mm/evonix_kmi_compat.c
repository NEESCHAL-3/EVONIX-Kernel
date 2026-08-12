// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX GKI KMI compatibility.
 *
 * Android GKI vendor KMI lists expose kasan_flag_enabled.
 * When HW-tag KASAN is disabled, retain the ABI symbol as a
 * permanently-false static key without enabling any KASAN runtime.
 */

#include <linux/export.h>
#include <linux/static_key.h>

#ifndef CONFIG_KASAN_HW_TAGS
DEFINE_STATIC_KEY_FALSE(kasan_flag_enabled);
EXPORT_SYMBOL(kasan_flag_enabled);
#endif
