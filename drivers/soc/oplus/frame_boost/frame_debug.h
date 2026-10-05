/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _FRAME_DEBUG_H
#define _FRAME_DEBUG_H

/*debug level for fbg*/
#define DEBUG_SYSTRACE (1 << 0)
#define DEBUG_FTRACE   (1 << 1)
#define DEBUG_KMSG     (1 << 2)
#define DEBUG_SYSTRACE_KEY_UTIL     (1 << 3)
#define DEBUG_VERBOSE  (1 << 10)

enum {
	frame_zone = 0,
	frame_min_util,
	prev_window_exec,
	prev_window_util,
	curr_window_exec,
	curr_window_util,
	framerate,
	vutil,
	use_vutil,
	rt_boost_freq_migr,
	buffer_count_id,
	next_vsync_id,
	handler_busy,
	frame_state,
	ui_id,
	delta_end_exec,
	delta_end_time,
	vutil_id,
	use_vutil_id,
	cfs_boost_freq,
	cfs_boost_migr,
	IMS_policy_util,
	cfs_policy_curr_util,
	rt_policy_curr_util,
	inputmethod_raw_util,
	ed_task_type,

	max_msg_id,
};

enum {
	max_cpu_grp_msg_id,
};

enum {
	fbg_state,
	fbg_active,
	frameboost_limit,
	raw_util,

	max_cpu_msg_id,
};


void val_systrace_c(int grp_id, unsigned long val, char *msg, int msg_id);
void cpu_val_systrace_c(unsigned long val, unsigned int cpu, char *msg, int msg_id);
void cpu_grp_systrace_c(int grp_id, unsigned long val, unsigned int cpu, char *msg, int msg_id);
void max_grp_systrace_c(unsigned int cpu, int grp_id);
void pref_cpus_systrace_c(int grp_pid, unsigned int cpu);
void avai_cpus_systrace_c(int grp_pid, unsigned int cpu);
void fbg_dbg_reset(void);
void fbg_dbg_init(void);

#endif /* _FRAME_DEBUG_H */
