// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#ifndef __CPUFREQ_SCX_MAIN_H__
#define __CPUFREQ_SCX_MAIN_H__

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/irq_work.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <../kernel/time/tick-sched.h>
#include <../kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>
#include <trace/hooks/sched.h>
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
#include <../kernel/sched/walt/walt.h>
#include <../kernel/sched/walt/trace.h>
#endif /* CONFIG_OPLUS_SYSTEM_KERNEL_QCOM */
#endif /*__CPUFREQ_SCX_MAIN_H__*/

