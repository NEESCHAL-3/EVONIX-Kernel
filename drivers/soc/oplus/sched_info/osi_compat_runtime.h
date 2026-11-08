/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OPLUS_OSI_COMPAT_RUNTIME_H
#define OPLUS_OSI_COMPAT_RUNTIME_H
#include <linux/cpumask.h>
#include <linux/sched.h>
void osi_compat_runtime_init(void);
void osi_compat_account_runtime(struct task_struct *task, u64 runtime);
u32 osi_compat_cpu_load(u32 windows, const struct cpumask *mask);
#endif
