// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/proc_fs.h>
#include <linux/kmemleak.h>
#include "frame_info.h"

unsigned int sysctl_frame_boost_enable;
unsigned int sysctl_frame_boost_debug;
EXPORT_SYMBOL_GPL(sysctl_frame_boost_debug);
unsigned int sysctl_slide_boost_enabled;
EXPORT_SYMBOL_GPL(sysctl_slide_boost_enabled);
unsigned int sysctl_input_boost_enabled;
EXPORT_SYMBOL_GPL(sysctl_input_boost_enabled);
unsigned int sysctl_frame_boost_stage_enabled;
EXPORT_SYMBOL_GPL(sysctl_frame_boost_stage_enabled);

#define MAX_FPS_DATA_SIZE (FRAME_FPS_DATA_SIZE * 128)
static char sysctl_frame_boost_fps_data[MAX_FPS_DATA_SIZE];

#define INPUT_BOOST_DURATION 1500000000
static struct hrtimer ibtimer;
static int intput_boost_duration;
static ktime_t ib_last_time;
extern struct frame_fps_data g_frame_fps_data[FRAME_FPS_DATA_SIZE];

void enable_input_boost_timer(void)
{
	ktime_t ktime;

	ib_last_time = ktime_get();
	ktime = ktime_set(0, intput_boost_duration);

	hrtimer_start(&ibtimer, ktime, HRTIMER_MODE_REL);
}

void disable_input_boost_timer(void)
{
	hrtimer_cancel(&ibtimer);
}

enum hrtimer_restart input_boost_timeout(struct hrtimer *timer)
{
	ktime_t now, delta;

	now = ktime_get();
	delta = ktime_sub(now, ib_last_time);

	ib_last_time = now;
	sysctl_input_boost_enabled = 0;

	return HRTIMER_NORESTART;
}

static int input_boost_ctrl_handler(struct ctl_table *table, int write, void __user *buffer,
	size_t *lenp, loff_t *ppos)
{
	int result;

	result = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!write)
		goto out;

	disable_input_boost_timer();
	enable_input_boost_timer();
out:
	return result;
}

static int slide_boost_ctrl_handler(struct ctl_table *table, int write, void __user *buffer,
	size_t *lenp, loff_t *ppos)
{
	int result;

	result = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!write)
		goto out;

	if (sysctl_input_boost_enabled && sysctl_slide_boost_enabled) {
		disable_input_boost_timer();
		sysctl_input_boost_enabled = 0;
	}

out:
	return result;
}

bool is_valid_frame_rate(int frame_rate)
{
	for (int i = 0; i < FRAME_FPS_DATA_SIZE; ++i) {
		if (frame_rate == g_frame_fps_data[i].frame_rate)
			return true;
	}
	return false;
}

/* module parameters */
static int write_frame_fps_data(const char *buf)
{
	/*
	 * format:
	 * echo "60,3,4,4,400,500" > frame_boost_fps_data
	 */
	struct frame_fps_data ff_date;
	struct frame_fps_data *p_ff_data;

	if (sscanf(buf, "%d,%d,%d,%d,%d,%d\n",
		&ff_date.frame_rate,
		&ff_date.input_period,
		&ff_date.animation_period,
		&ff_date.traversal_period,
		&ff_date.step_boost_util,
		&ff_date.half_vsync_util) != 6)
		goto out;

	if (!is_valid_frame_rate(ff_date.frame_rate))
		goto out;

	if (ff_date.step_boost_util < 0 || ff_date.step_boost_util > SCHED_CAPACITY_SCALE
			|| ff_date.half_vsync_util < 0 || ff_date.half_vsync_util > SCHED_CAPACITY_SCALE)
		goto out;

	if (ff_date.input_period < 0 || ff_date.animation_period <0 ||
			ff_date.traversal_period < 0)
		goto out;

	p_ff_data = lookup_frame_fps_data(ff_date.frame_rate);

	/* begin update frame_fps_data */
	p_ff_data->input_period = ff_date.input_period;
	p_ff_data->animation_period = ff_date.animation_period;
	p_ff_data->traversal_period = ff_date.traversal_period;
	p_ff_data->step_boost_util = ff_date.step_boost_util;
	p_ff_data->half_vsync_util = ff_date.half_vsync_util;

	return 0;
out:
	pr_warn("frame_fps_data: invalid:%s\n", buf);
	return -1;
}

static int fps_data_ctrl_handler(struct ctl_table *table, int write, void __user *buffer,
	size_t *lenp, loff_t *ppos)
{
	int ret, cnt = 0;
	char buf[MAX_FPS_DATA_SIZE];
	struct frame_fps_data *p_ff_data;

	#define APPEND_FMT(fmt, ...) do { \
		int _remaining = MAX_FPS_DATA_SIZE - cnt; \
		if (_remaining <= 0) break; \
		cnt += snprintf(buf + cnt, _remaining, fmt, ##__VA_ARGS__); \
	} while (0)

	if (write) {
		ret = write_frame_fps_data(buffer);
		if (ret < 0) {
			return 0;
		}
	} else {
		for (int i = 0; i < FRAME_FPS_DATA_SIZE; ++i) {
			p_ff_data = &g_frame_fps_data[i];

			APPEND_FMT("frame_rate %d, ", p_ff_data->frame_rate);
			APPEND_FMT("input_period: %d, ", p_ff_data->input_period);
			APPEND_FMT("animation_period: %d, ", p_ff_data->animation_period);
			APPEND_FMT("traversal_period: %d, ", p_ff_data->traversal_period);
			APPEND_FMT("step_boost_util: %d, ", p_ff_data->step_boost_util);
			APPEND_FMT("half_vsync_util: %d", p_ff_data->half_vsync_util);
			APPEND_FMT("\n");
		}

		buf[MAX_FPS_DATA_SIZE - 1] = '\0';
		strncpy((char *)table->data, buf, MAX_FPS_DATA_SIZE);
		ret = proc_dostring(table, write, buffer, lenp, ppos);
	}

	#undef APPEND_FMT
	return ret;
}

struct ctl_table frame_boost_table[] = {
	{
		.procname	= "frame_boost_enabled",
		.data		= &sysctl_frame_boost_enable,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "frame_boost_debug",
		.data		= &sysctl_frame_boost_debug,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "slide_boost_enabled",
		.data		= &sysctl_slide_boost_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= slide_boost_ctrl_handler,
	},
	{
		.procname	= "input_boost_enabled",
		.data		= &sysctl_input_boost_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= input_boost_ctrl_handler,
	},
	{
		.procname	= "frame_boost_stage_enabled",
		.data		= &sysctl_frame_boost_stage_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "frame_boost_fps_data",
		.data		= &sysctl_frame_boost_fps_data,
		.maxlen		= sizeof(sysctl_frame_boost_fps_data),
		.mode		= 0666,
		.proc_handler	= fps_data_ctrl_handler,
	},
	{ }
};

void fbg_sysctl_init(void)
{
	struct ctl_table_header *hdr;

	sysctl_frame_boost_enable = 0;
	sysctl_frame_boost_debug = 0;
	sysctl_slide_boost_enabled = 0;
	sysctl_input_boost_enabled = 0;
	sysctl_frame_boost_stage_enabled = 0;

	ib_last_time = ktime_get();
	intput_boost_duration = INPUT_BOOST_DURATION;
	hrtimer_init(&ibtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ibtimer.function = &input_boost_timeout;

	hdr = register_sysctl("fbg", frame_boost_table);
	kmemleak_not_leak(hdr);
}
