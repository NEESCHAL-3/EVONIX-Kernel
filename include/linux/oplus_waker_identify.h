// SPDX-License-Identifier: GPL-2.0-only
#ifndef _LINUX_OPLUS_WAKER_IDENTIFY_H
#define _LINUX_OPLUS_WAKER_IDENTIFY_H

#include <linux/kconfig.h>

struct task_struct;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_WAKER_IDENTIFY)
void evonix_waker_identify_try_to_wake_up(struct task_struct *task);
#else
static inline void evonix_waker_identify_try_to_wake_up(struct task_struct *task)
{
}
#endif

#endif
