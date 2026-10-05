// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/string.h>
#include <linux/kernel.h>
#include "frame_group.h"
#include "frame_debug.h"
#define MSG_COMM_LEN      50

struct verbose_msg {
	char msg[MSG_COMM_LEN];
	int val;
};

static struct verbose_msg  verbose_msgs[MAX_NUM_FBG_ID][max_msg_id] = {
	{{.msg = "01_frame_zone", .val = -1},
	{.msg = "frame_min_util", .val = -1},
	{.msg = "prev_window_exec", .val = -1},
	{.msg = "prev_window_util", .val = -1},
	{.msg = "curr_window_exec", .val = -1},
	{.msg = "curr_window_util", .val = -1},
	{.msg = "framerate", .val = -1},
	{.msg = "vutil", .val = -1},
	{.msg = "use_vutil", .val = -1},
	{.msg = "rt_boost_freq/migr", .val = -1},
	{.msg = "buffer_count_id", .val = -1},
	{.msg = "next_vsync_id", .val = -1},
	{.msg = "handler_busy ", .val = -1},
	{.msg = "frame_state", .val = -1},
	{.msg = "00_ui", .val = -1},
	{.msg = "delta_end_exec", .val = -1},
	{.msg = "delta_end_time", .val = -1},
	{.msg = "vutil_id", .val = -1},
	{.msg = "use_vutil_id", .val = -1},
	{.msg = "cfs_boost_freq", .val = -1},
	{.msg = "cfs_boost_migr", .val = -1},
	{.msg = "IMS_policy_util", .val = -1},
	{.msg = "cfs_policy/curr_util", .val = -1},
	{.msg = "rt_policy/curr_util", .val = -1},
	{.msg = "inputmethod/raw_util", .val = -1},
	{.msg = "ed_task_boost_type", .val = -1},
	},
};

DEFINE_PER_CPU(struct verbose_msg[MAX_NUM_FBG_ID][max_cpu_grp_msg_id], cpu_grp_state_msgs);

DEFINE_PER_CPU(struct verbose_msg[max_cpu_msg_id], cpu_state_msgs);


static noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

void pref_cpus_systrace_c(int grp_id, unsigned int cpu)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "C|10000|grp[%d]_02_cls_pref|%d\n", grp_id, cpu);
	tracing_mark_write(buf);
}

void avai_cpus_systrace_c(int grp_id, unsigned int cpu)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "C|10000|grp[%d]_02_cls_avai|%d\n", grp_id, cpu);
	tracing_mark_write(buf);
}

void val_systrace_c(int grp_id, unsigned long val, char *msg, int msg_id)
{
	char buf[256];
	struct verbose_msg *verbose_msg;

	if ((grp_id < 0) || (grp_id >= MAX_NUM_FBG_ID))
		return;
	if (msg_id < frame_zone || msg_id >= max_msg_id)
		return;

	verbose_msg = &verbose_msgs[grp_id][msg_id];
	if (strncmp(verbose_msg->msg, msg, strlen(msg)))
		return;
	if (verbose_msg->val != val) {
		snprintf(buf, sizeof(buf), "C|10000|grp[%d]%s|%lu\n", grp_id, msg, val);
		tracing_mark_write(buf);
		verbose_msg->val = val;
	}
}
EXPORT_SYMBOL_GPL(val_systrace_c);

void cpu_val_systrace_c(unsigned long val, unsigned int cpu, char *msg, int msg_id)
{
	char buf[256];

	struct verbose_msg *ptr;

	if (msg_id < fbg_state || msg_id >= max_cpu_msg_id)
		return;

	ptr = &per_cpu(cpu_state_msgs[msg_id], cpu);
	if (strncmp(ptr->msg, msg, strlen(msg)))
		return;
	if (ptr->val != val) {
		snprintf(buf, sizeof(buf), "C|10000|%s_cpu%d|%lu\n", msg, cpu, val);
		tracing_mark_write(buf);
		ptr->val = val;
	}
}
EXPORT_SYMBOL_GPL(cpu_val_systrace_c);


void cpu_grp_systrace_c(int grp_id, unsigned long val, unsigned int cpu, char *msg, int msg_id)
{
	char buf[256];
	struct verbose_msg *ptr;

	if (msg_id < cfs_policy_curr_util || msg_id >= max_cpu_grp_msg_id)
		return;
	if ((grp_id < 0) || (grp_id >= MAX_NUM_FBG_ID))
		return;
	ptr = &per_cpu(cpu_grp_state_msgs[grp_id][msg_id], cpu);
	if (strncmp(ptr->msg, msg, strlen(msg)))
		return;
	if (ptr->val != val) {
		snprintf(buf, sizeof(buf), "C|10000|grp[%d]_cpu[%d]_%s|%lu\n", grp_id, cpu, msg, val);
		tracing_mark_write(buf);
		ptr->val = val;
	}
}
EXPORT_SYMBOL_GPL(cpu_grp_systrace_c);

void max_grp_systrace_c(unsigned int cpu, int grp_id)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "C|10000|cls[%d]_max_grp_id|%d\n", cpu, grp_id);
	tracing_mark_write(buf);
}

void fbg_dbg_reset(void)
{
	int i, j;
	int cpu, msg_id;
	struct verbose_msg *ptr;

	for (i = 1; i < MAX_NUM_FBG_ID; i++) {
		for (j = 0; j < max_msg_id; j++) {
			verbose_msgs[i][j].val = -1;
		}
	}

	for_each_possible_cpu(cpu)
		for (msg_id = fbg_state; msg_id < max_cpu_msg_id; msg_id++) {
			ptr = &per_cpu(cpu_state_msgs[msg_id], cpu);
			ptr->val = -1;
		}
	for_each_possible_cpu(cpu)
		for (i = 1; i < MAX_NUM_FBG_ID; i++) {
			for (msg_id = cfs_policy_curr_util; msg_id < max_cpu_grp_msg_id; msg_id++) {
				ptr = &per_cpu(cpu_grp_state_msgs[i][msg_id], cpu);
				ptr->val = -1;
			}
		}
}

void fbg_dbg_init(void)
{
	int i, j;
	int cpu, msg_id;
	struct verbose_msg *ptr;

	for (i = 1; i < MAX_NUM_FBG_ID; i++) {
		for (j = 0; j < max_msg_id; j++) {
			verbose_msgs[i][j].val = -1;
			strncpy(verbose_msgs[i][j].msg, verbose_msgs[0][j].msg, MSG_COMM_LEN);
		}
	}

	for_each_possible_cpu(cpu)
		for (msg_id = fbg_state; msg_id < max_cpu_msg_id; msg_id++) {
			ptr = &per_cpu(cpu_state_msgs[msg_id], cpu);
			ptr->val = -1;
			switch (msg_id) {
			case fbg_state:
				snprintf(ptr->msg, sizeof(ptr->msg), "%s", "fbg_state");
			break;
			case fbg_active:
				snprintf(ptr->msg,  sizeof(ptr->msg), "%s", "fbg_active");
			break;
			case frameboost_limit:
				snprintf(ptr->msg,  sizeof(ptr->msg), "%s", "frameboost_limit");
			break;
			case raw_util:
				snprintf(ptr->msg,  sizeof(ptr->msg), "%s", "raw_util");
			break;
			default:
			break;
			}
		}
	for_each_possible_cpu(cpu) {
		for (i = 1; i < MAX_NUM_FBG_ID; i++) {
			for (msg_id = cfs_policy_curr_util; msg_id < max_cpu_grp_msg_id; msg_id++) {
				ptr = &per_cpu(cpu_grp_state_msgs[i][msg_id], cpu);
				ptr->val = -1;
				switch (msg_id) {
				case cfs_policy_curr_util:
					snprintf(ptr->msg, sizeof(ptr->msg), "%s", "cfs_policy/curr_util");
				break;
				case rt_policy_curr_util:
					snprintf(ptr->msg,  sizeof(ptr->msg), "%s", "rt_policy/curr_util");
				break;
				case inputmethod_raw_util:
					snprintf(ptr->msg,  sizeof(ptr->msg), "%s", "inputmethod/raw_util");
				break;
				default:
				break;
				}
			}
		}
	}
}


