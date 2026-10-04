// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */
#define pr_fmt(fmt) "blockio_ux_opt: " fmt

#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/ww_mutex.h>
#include <linux/sched/signal.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/debug.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/osq_lock.h>
#include <linux/sched_clock.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/sched/cputime.h>
#include <../kernel/sched/sched.h>
#include <trace/hooks/vendor_hooks.h>
#include <trace/hooks/sched.h>
#include <trace/hooks/mm.h>
#include "sa_common.h"

#define TRUE_ADDR (0xFFC000000000)

static void dm_bufio_shrink_scan_bypass_handler(void *unused, unsigned long para_task, bool *highprio)
{
	struct task_struct *task = (struct task_struct *)para_task;

	/*Since we reused this hook, if para_task is not the task_addr, just return.*/
	if (para_task < TRUE_ADDR)
		return;

	if (*highprio) {
		struct oplus_task_struct *ots = get_oplus_task_struct(task);

		if (IS_ERR_OR_NULL(ots))
			return;

		ots->ux_state = SA_TYPE_LIGHT;
		return;
	}

	if (test_task_ux(task) || test_task_is_rt(task)) {
		*highprio = true;
	}
}

static int register_blockio_vendor_hooks(void)
{
	int ret = 0;

	ret = register_trace_android_vh_dm_bufio_shrink_scan_bypass(
			dm_bufio_shrink_scan_bypass_handler, NULL);
	return ret;
}

static void unregister_blockio_vendor_hooks(void)
{
	unregister_trace_android_vh_dm_bufio_shrink_scan_bypass(
			dm_bufio_shrink_scan_bypass_handler, NULL);
}

int sa_blockio_init(void)
{
	int ret = 0;
	printk(KERN_EMERG"%s:%d\n", __func__, __LINE__);
	ret = register_blockio_vendor_hooks();
	if (ret != 0)
		return ret;

	pr_info("%s succeed!\n", __func__);
	return 0;
}

void sa_blockio_exit(void)
{
	unregister_blockio_vendor_hooks();
}
