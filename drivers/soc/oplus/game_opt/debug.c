// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <uapi/linux/sched/types.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <trace/hooks/sched.h>

#include "game_ctrl.h"

int g_debug_enable = 0;

static noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

inline void systrace_c_printk(const char *msg, unsigned long val)
{
	if (g_debug_enable == 1) {
		char buf[128];
		snprintf(buf, sizeof(buf), "C|99999|%s|%lu\n", msg, val);
		tracing_mark_write(buf);
	}
}

inline void systrace_c_signed_printk(const char *msg, long val)
{
	if (g_debug_enable == 1) {
		char buf[128];
		snprintf(buf, sizeof(buf), "C|99999|%s|%ld\n", msg, val);
		tracing_mark_write(buf);
	}
}

inline void htb_systrace_c_printk(const char *prefix, int digit, const char *comm, int val)
{
	if (g_debug_enable == 1) {
		char buf[128];
		snprintf(buf, sizeof(buf), "C|99999|%s_%d_%.5s|%d\n", prefix, digit, comm, val);
		tracing_mark_write(buf);
	}
}

static void sched_setaffinity_early_hook(void *unused, struct task_struct *p,
	const struct cpumask *in_mask, bool *skip)
{
	if (p->tgid == game_pid) {
		pr_info("gameopt, %s: c_comm=%s, c_pid=%d, c_tgid=%d, comm=%s, pid=%d, tgid=%d, in_mask=%*pbl\n",
			__func__, current->comm, current->pid, current->tgid, p->comm, p->pid, p->tgid,
			cpumask_pr_args(in_mask));
	}
}

static ssize_t debug_enable_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &g_debug_enable);
	if (ret != 1)
		return -EINVAL;

	if (g_debug_enable == 2) {
		register_trace_android_vh_sched_setaffinity_early(sched_setaffinity_early_hook, NULL);
	}

	return count;
}

static ssize_t debug_enable_proc_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int len;

	len = sprintf(page, "%d\n", g_debug_enable);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops debug_enable_proc_ops = {
	.proc_write		= debug_enable_proc_write,
	.proc_read		= debug_enable_proc_read,
	.proc_lseek		= default_llseek,
};

int debug_init(void)
{
	proc_create_data("debug_enable", 0666, game_opt_dir, &debug_enable_proc_ops, NULL);

	return 0;
}
