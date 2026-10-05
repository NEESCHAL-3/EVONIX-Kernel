// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/sched.h>
#include "frame_boost.h"
#include "frame_debug.h"
#include "frame_group.h"
#include "frame_info.h"
#include "frame_timer.h"

#define DEFAULT_VUTIL_MARGIN    (0)
#define DEFAULT_FRAME_RATE      (60)
#define DEFAULT_FRAME_INTERVAL  (16666667L)
static struct frame_info sf_frame_info;
static struct frame_info game_frame_info;
static struct frame_info multi_frame_info[MULTI_FBG_NUM];
static struct multi_fbg_id_manager g_id_manager = {
	.id_map = {0},
	.offset = 0,
	.lock = __RW_LOCK_UNLOCKED(g_id_manager.lock)
};

struct frame_fps_data g_frame_fps_data[FRAME_FPS_DATA_SIZE] = {
	[0] = {
		.frame_rate = 60,
		.input_period = 3,
		.animation_period = 4,
		.traversal_period = 4,
		.step_boost_util = 400,
		.half_vsync_util = 500,
	},
	[1] = {
		.frame_rate = 90,
		.input_period = 3,
		.animation_period = 4,
		.traversal_period = 4,
		.step_boost_util = 400,
		.half_vsync_util = 500,
	},
	[2] = {
		.frame_rate = 120,
		.input_period = 3,
		.animation_period = 4,
		.traversal_period = 4,
		.step_boost_util = 400,
		.half_vsync_util = 500,
	},
	[3] = {
		.frame_rate = 144,
		.input_period = 3,
		.animation_period = 4,
		.traversal_period = 4,
		.step_boost_util = 400,
		.half_vsync_util = 500,
	},
};

extern struct frame_group *frame_boost_groups[MAX_NUM_FBG_ID];
extern struct frame_timer frame_timer[MAX_NUM_FBG_ID];

bool is_fbg(int grp_id)
{
	return ((grp_id > 0) && (grp_id < MAX_NUM_FBG_ID));
}
EXPORT_SYMBOL_GPL(is_fbg);

bool is_frame_fbg(int grp_id)
{
	return (is_fbg(grp_id) && (grp_id != INPUTMETHOD_FRAME_GROUP_ID));
}
EXPORT_SYMBOL_GPL(is_frame_fbg);

bool is_multi_frame_fbg(int id)
{
	return ((id >= MULTI_FBG_ID) && (id < MULTI_FBG_ID + MULTI_FBG_NUM));
}
EXPORT_SYMBOL_GPL(is_multi_frame_fbg);

bool is_active_multi_frame_fbg(int id)
{
	bool ret = false;

	if ((id < MULTI_FBG_ID) || (id >= MULTI_FBG_ID + MULTI_FBG_NUM))
		return false;

	read_lock(&g_id_manager.lock);
	if (test_bit(id - MULTI_FBG_ID, g_id_manager.id_map))
		ret = true;
	read_unlock(&g_id_manager.lock);

	return ret;
}
EXPORT_SYMBOL_GPL(is_active_multi_frame_fbg);

struct frame_info *fbg_active_multi_frame_info(int id)
{
	struct frame_info *frame_info = NULL;

	if ((id < MULTI_FBG_ID) || (id >= MULTI_FBG_ID + MULTI_FBG_NUM)) {
		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_debug("grp_id[%d] is invalid multi frame group\n", id);
		return NULL;
	}

	read_lock(&g_id_manager.lock);
	if (test_bit(id - MULTI_FBG_ID, g_id_manager.id_map))
		frame_info = &multi_frame_info[id - MULTI_FBG_ID];
	read_unlock(&g_id_manager.lock);

	if (!frame_info) {
		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_debug("grp_id[%d] is inactive multi frame group\n", id);
	}

	return frame_info;
}

static int alloc_fbg_id(void)
{
	unsigned int id_offset;
	int id;
	unsigned long flags;

	write_lock_irqsave(&g_id_manager.lock, flags);
	id_offset = find_next_zero_bit(g_id_manager.id_map, MULTI_FBG_NUM,
				       g_id_manager.offset);
	if (id_offset >= MULTI_FBG_NUM) {
		id_offset = find_first_zero_bit(g_id_manager.id_map,
						MULTI_FBG_NUM);
		if (id_offset >= MULTI_FBG_NUM) {
			write_unlock_irqrestore(&g_id_manager.lock, flags);
			ofb_err("alloc grp_id failed, no free multi frame group\n");
			return -NO_FREE_MULTI_FBG;
		}
	}

	set_bit(id_offset, g_id_manager.id_map);
	g_id_manager.offset = id_offset;
	id = id_offset + MULTI_FBG_ID;
	write_unlock_irqrestore(&g_id_manager.lock, flags);

	if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
		ofb_debug("alloc grp_id[%d] succeed, id_offset=%u\n", id, id_offset);
	return id;
}

void free_fbg_id(int id)
{
	unsigned int id_offset = id - MULTI_FBG_ID;
	unsigned long flags;

	if (id_offset >= MULTI_FBG_NUM) {
		ofb_err("invalid multi grp_id[%d], id_offset[%u]\n", id, id_offset);
		return;
	}

	write_lock_irqsave(&g_id_manager.lock, flags);
	clear_bit(id_offset, g_id_manager.id_map);
	write_unlock_irqrestore(&g_id_manager.lock, flags);
	if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
		ofb_debug("free multi grp_id[%d] succeed, id_offset[%u]\n", id, id_offset);
}

int alloc_multi_fbg(void)
{
	struct frame_info *frame_info = NULL;
	int grp_id;

	grp_id = alloc_fbg_id();
	if (grp_id < 0)
		return grp_id;

	frame_info = fbg_active_multi_frame_info(grp_id);
	if (!frame_info) {
		free_fbg_id(grp_id);
		return -ERR_INFO;
	}

	set_frame_rate(grp_id, DEFAULT_FRAME_RATE);

	return grp_id;
}
EXPORT_SYMBOL_GPL(alloc_multi_fbg);

/*this function is used when system use single RTG*/
int set_static_fbg(int grp_id)
{
	unsigned int id_offset;
	unsigned long flags;

	if ((grp_id < MULTI_FBG_ID) || (grp_id >= MULTI_FBG_ID + MULTI_FBG_NUM)) {
		return -1;
	}
	id_offset = grp_id - MULTI_FBG_ID;
	write_lock_irqsave(&g_id_manager.lock, flags);
	set_bit(grp_id - MULTI_FBG_ID, g_id_manager.id_map);
	g_id_manager.offset = id_offset;
	write_unlock_irqrestore(&g_id_manager.lock, flags);

	set_frame_rate(grp_id, DEFAULT_FRAME_RATE);

	return 0;
}
EXPORT_SYMBOL_GPL(set_static_fbg);

void release_multi_fbg(int id)
{
	if ((id < MULTI_FBG_ID) || (id >= MULTI_FBG_ID + MULTI_FBG_NUM)) {
		return;
	}

	if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
		ofb_debug("release multi grp_id[%d].\n", id);

	read_lock(&g_id_manager.lock);
	if (!test_bit(id - MULTI_FBG_ID, g_id_manager.id_map)) {
		read_unlock(&g_id_manager.lock);
		ofb_err("multi grp_id[%d] is already inactive\n", id);
		return;
	}
	read_unlock(&g_id_manager.lock);

	free_fbg_id(id);
}
EXPORT_SYMBOL_GPL(release_multi_fbg);

void clear_multi_fbg(void)
{
	unsigned long flags;

	write_lock_irqsave(&g_id_manager.lock, flags);
	bitmap_zero(g_id_manager.id_map, MULTI_FBG_NUM);
	g_id_manager.offset = 0;
	write_unlock_irqrestore(&g_id_manager.lock, flags);
}

struct frame_info *fbg_frame_info(int grp_id)
{
	struct frame_info *frame_info = NULL;

	if (!is_frame_fbg(grp_id)) {
		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_err("grp_id[%d] is not a real frame group\n", grp_id);
		return NULL;
	} else if (grp_id == SF_FRAME_GROUP_ID)
		frame_info = &sf_frame_info;
	else if (grp_id == GAME_FRAME_GROUP_ID)
		frame_info = &game_frame_info;
	else
		frame_info = &multi_frame_info[grp_id - MULTI_FBG_ID];

	return frame_info;
}
EXPORT_SYMBOL_GPL(fbg_frame_info);

static void __set_frame_rate(struct frame_info *frame_info, unsigned int frame_rate)
{
	frame_info->frame_rate = frame_rate;
	frame_info->frame_interval = NSEC_PER_SEC / frame_rate;
	frame_info->vutil_margin = 0;
	frame_info->vutil_time2max = frame_info->frame_interval / NSEC_PER_MSEC + frame_info->vutil_margin;
}

/*
 * get_frame_rate - get current frame rate of system
 */
int get_frame_rate(int grp_id)
{
	struct frame_info *frame_info = NULL;
	unsigned long flags = 0;
	unsigned int frame_rate = 0;

	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL)
		return frame_rate;

	raw_spin_lock_irqsave(&frame_info->lock, flags);
	frame_rate = frame_info->frame_rate;
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	return frame_rate;
}
EXPORT_SYMBOL_GPL(get_frame_rate);

/*
 * set_frame_rate - set frame rate by top app
 * @frame_rate: frame rate
 *
 *  Return: true if update frame rate successfully
 */
bool set_frame_rate(int grp_id, unsigned int frame_rate)
{
	unsigned long flags = 0;
	struct frame_info *frame_info = fbg_frame_info(grp_id);

	if (frame_info == NULL)
		return false;

	raw_spin_lock_irqsave(&frame_info->lock, flags);

	if (frame_rate == frame_info->frame_rate) {
		raw_spin_unlock_irqrestore(&frame_info->lock, flags);
		return false;
	}

	if ((frame_rate < MIN_FRAME_RATE) || (frame_rate > MAX_FRAME_RATE)) {
		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_err("invalid frame rate %d, min is %d, max is %d", frame_rate, MIN_FRAME_RATE, MAX_FRAME_RATE);
		raw_spin_unlock_irqrestore(&frame_info->lock, flags);
		return false;
	}

	/* app frame rate should be limited by sf frame rate */
	if ((frame_info != &sf_frame_info) && (frame_rate > sf_frame_info.frame_rate)) {
		raw_spin_unlock_irqrestore(&frame_info->lock, flags);
		return false;
	}

	__set_frame_rate(frame_info, frame_rate);
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
		ofb_debug("set grp_id[%d] frame rate[%d] succeed\n", grp_id, frame_rate);

	if (unlikely(sysctl_frame_boost_debug & DEBUG_FTRACE))
		trace_printk("set grp_id[%d] frame_rate=%u frame_interval=%u vutil_time2max=%u\n",
			grp_id,
			frame_info->frame_rate,
			frame_info->frame_interval,
			frame_info->vutil_time2max);

	return true;
}
EXPORT_SYMBOL_GPL(set_frame_rate);

int set_frame_margin(int grp_id, int margin_ms)
{
	struct frame_info *frame_info = NULL;
	unsigned long flags;
	int interval_ms = -1;
	int max_margin = INT_MAX;
	int min_margin = INT_MIN;

	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL) {
		return -ERR_INFO;
	}

	raw_spin_lock_irqsave(&frame_info->lock, flags);

	interval_ms = div_u64(frame_info->frame_interval, NSEC_PER_MSEC);
	max_margin = interval_ms;
	min_margin = -1 * (interval_ms >> 1);
	if (margin_ms < min_margin || margin_ms > max_margin) {
		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_err("invalid frame margin %d, max is %d, min is %d", margin_ms, min_margin, max_margin);
		raw_spin_unlock_irqrestore(&frame_info->lock, flags);
		return -EINVAL;
	}

	frame_info->vutil_margin = margin_ms;
	frame_info->vutil_time2max = frame_info->frame_interval / NSEC_PER_MSEC + frame_info->vutil_margin;
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(set_frame_margin);

bool is_high_frame_rate(int grp_id)
{
	struct frame_info *frame_info = NULL;

	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL)
		return false;
	return frame_info->frame_rate > DEFAULT_FRAME_RATE;
}
EXPORT_SYMBOL_GPL(is_high_frame_rate);

/*
 * set_frame_util_min - set minimal utility clamp value
 * @min_util: minimal utility
 */
int set_frame_util_min(int grp_id, int min_util, bool clear)
{
	struct frame_info *frame_info = NULL;
	unsigned long flags;

	if (min_util < 0 || min_util > SCHED_CAPACITY_SCALE)
		return -INVALID_ARG;

	frame_info = fbg_active_multi_frame_info(grp_id);
	if (frame_info == NULL)
		return -INACTIVE_MULTI_FBG_ID;
	raw_spin_lock_irqsave(&frame_info->lock, flags);
	frame_info->frame_min_util = min_util;
	frame_info->clear_limit = clear;
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	if (unlikely(sysctl_frame_boost_debug & (DEBUG_SYSTRACE | DEBUG_SYSTRACE_KEY_UTIL))) {
		val_systrace_c(grp_id, min_util, "frame_min_util", frame_min_util);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(set_frame_util_min);

/*
 * set_frame_state - set frame state when switch fg/bg, receive vsync-app or
 *            out of valid frame range (default set to 2 frame length)
 * @delta: frame state.
 *
 * switch fg/bg---------FRAME_END
 * receive vsync-app----FRAME_START
 * extra long frame-----FRAME_END
 */
void set_frame_state(int grp_id, unsigned int state, int buffer_count, int next_vsync)
{
	unsigned long flags = 0;
	struct frame_info *frame_info = fbg_frame_info(grp_id);

	if (frame_info == NULL)
		return;

	raw_spin_lock_irqsave(&frame_info->lock, flags);

	frame_info->frame_state = state;
	if (buffer_count != -1)
		atomic_set(&frame_info->buffer_count, buffer_count);
	/* next_vsync hint only be sent on frame_end state */
	if (next_vsync != -1)
		frame_info->next_vsync = next_vsync;

	if (unlikely(frame_info->clear_limit && state == FRAME_START)) {
		frame_info->frame_max_util = SCHED_CAPACITY_SCALE;
		frame_info->frame_min_util = 0;
		frame_info->clear_limit = false;
		if (unlikely(sysctl_frame_boost_debug & (DEBUG_SYSTRACE | DEBUG_SYSTRACE_KEY_UTIL)))
			val_systrace_c(grp_id, 0, "frame_min_util", frame_min_util);
	}
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	if (unlikely(sysctl_frame_boost_debug & DEBUG_SYSTRACE)) {
		if (state == FRAME_START)
			val_systrace_c(grp_id, 1, "frame_state", frame_state);
		else if (state == FRAME_END) {
			val_systrace_c(grp_id, 0, "frame_state", frame_state);
			if (next_vsync != -1)
				val_systrace_c(grp_id, next_vsync, "next_vsync", next_vsync_id);
		}
		if (buffer_count != -1)
			val_systrace_c(grp_id, buffer_count, "buffer_count", buffer_count_id);
	}
}
EXPORT_SYMBOL_GPL(set_frame_state);

unsigned int get_frame_state(int grp_id)
{
	struct frame_info *frame_info = fbg_frame_info(grp_id);

	if (frame_info == NULL)
		return 0;

	return frame_info->frame_state;
}
EXPORT_SYMBOL_GPL(get_frame_state);
/*
 * get_frame_vutil - calculate frame virtual util using delta
 *             time from frame start
 * @delta: delta time (nano sec).
 *
 * We use parabola to emulate the relationship between delta and virtual load
 * we have 2 know point in the parabola, one is (0,0) and the other is
 * (max_time, max_vutil) or (1.25 frame length, 1024), so it is easy to figure
 * out the function as the following:
 * virtual utility = f(delta)
 *    = delta * delta + (max_vutil/max_time - max_time) * delta
 *    = delta * (delta + max_vutil/max_time - max_time)
 *
 * Return: virtual utility
 */
unsigned long get_frame_vutil(int grp_id, u64 delta, bool handler_busy, int *buffer_count)
{
	unsigned long vutil = 0;
	int delta_ms = -1, max_time = -1;
	int tmp;
	struct frame_info *frame_info;
	unsigned long flags = 0;
	int interval_ms = -1;
	int min_margin = INT_MIN;
	int margin_ms_eff = INT_MIN;
	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL)
		return vutil;

	raw_spin_lock_irqsave(&frame_info->lock, flags);
	delta_ms = div_u64(delta, NSEC_PER_MSEC);
	*buffer_count = atomic_read(&frame_info->buffer_count);
	if (frame_info->frame_state == FRAME_END && !handler_busy)
		goto out;

	interval_ms = div_u64(frame_info->frame_interval, NSEC_PER_MSEC);
	min_margin = -1 * (interval_ms >> 1);

	if (*buffer_count <= 1)
		margin_ms_eff = min(min_margin, frame_info->vutil_margin);
	else if (*buffer_count == 2)
		margin_ms_eff = frame_info->vutil_margin;
	else if (*buffer_count >= 3)
		goto out;

	max_time = interval_ms + margin_ms_eff;

	if (max_time <= 0 || delta_ms > max_time) {
		vutil = SCHED_CAPACITY_SCALE;
		goto out;
	}

	tmp = delta_ms + SCHED_CAPACITY_SCALE / max_time;
	if (tmp <= max_time)
		goto out;

	vutil = delta_ms * (tmp - max_time);
out:
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);
	return vutil;
}

/*
 * get_frame_util - calculate frame physical util using delta
 * @delta: delta time (nano sec).
 *
 * Return: physical utility
 */
unsigned long get_frame_putil(int grp_id, u64 delta, unsigned int frame_zone)
{
	struct frame_info *frame_info;
	unsigned long util = 0;
	unsigned long frame_interval = 0;

	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL)
		return util;

	frame_interval = (frame_zone & FRAME_ZONE) ?
		frame_info->frame_interval : DEFAULT_FRAME_INTERVAL;

	if (frame_interval > 0)
		util = div_u64((delta << SCHED_CAPACITY_SHIFT), frame_interval);

	return util;
}

unsigned long frame_uclamp(int grp_id, unsigned long util)
{
	struct frame_info *frame_info;
	unsigned long clamp_util;

	frame_info = fbg_frame_info(grp_id);
	if (frame_info == NULL)
		return util;

	if (unlikely(frame_info->frame_min_util > frame_info->frame_max_util))
		return util;

	clamp_util = max_t(unsigned long, frame_info->frame_min_util, util);
	return min_t(unsigned long, clamp_util, frame_info->frame_max_util);
}

bool check_last_compose_time(bool composition)
{
	struct frame_info *frame_info = &sf_frame_info;
	unsigned long flags;
	u64 now;

	now = fbg_ktime_get_ns();
	if (composition) {
		raw_spin_lock_irqsave(&frame_info->lock, flags);
		frame_info->last_compose_time = now;
		raw_spin_unlock_irqrestore(&frame_info->lock, flags);
	}

	return (now - frame_info->last_compose_time) <= frame_info->frame_interval;
}
EXPORT_SYMBOL_GPL(check_last_compose_time);

/*
 * Set draw step for one frame.
 * Return: 0 if update frame step success.
 */
int set_frame_step_by_grp(int grp_id, int step)
{
	struct frame_info *info;
	unsigned long flags;

	info = fbg_frame_info(grp_id);
	if (!info)
		return -1;

	raw_spin_lock_irqsave(&info->lock, flags);
	set_frame_step(info, step);
	raw_spin_unlock_irqrestore(&info->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(set_frame_step_by_grp);

int get_frame_step_by_grp(int grp_id)
{
	struct frame_info *info;
	unsigned long flags;
	int step = FRAME_STEP_DEFAULT;

	info = fbg_frame_info(grp_id);
	if (!info)
		return -1;

	raw_spin_lock_irqsave(&info->lock, flags);
	step = get_frame_step(info);
	raw_spin_unlock_irqrestore(&info->lock, flags);

	return step;
}
EXPORT_SYMBOL_GPL(get_frame_step_by_grp);

static inline int mapping_frame_rate_to_index(int frame_rate)
{
	switch (frame_rate) {
	case 60:
		return 0;
	case 90:
		return 1;
	case 120:
		return 2;
	case 144:
		return 3;
	default:
		/* Used config of 60FPS as default */
		return 0;
	}
}

struct frame_fps_data *lookup_frame_fps_data(int frame_rate)
{
	return &g_frame_fps_data[mapping_frame_rate_to_index(frame_rate)];
}

int get_frame_step_boost_util(int frame_rate)
{
	struct frame_fps_data *ff_data;

	ff_data = &g_frame_fps_data[mapping_frame_rate_to_index(frame_rate)];

	return ff_data->step_boost_util;
}

int get_frame_half_vsync_boost_util(int frame_rate)
{
	struct frame_fps_data *ff_data;

	ff_data = &g_frame_fps_data[mapping_frame_rate_to_index(frame_rate)];

	return ff_data->half_vsync_util;
}

void do_frame_step_hint(int grp_id, int step)
{
	int duration;
	int frame_rate;
	struct frame_info *info;
	struct frame_timer *ft;
	struct frame_fps_data *ff_data;

	info = fbg_active_multi_frame_info(grp_id);
	if (!info) {
		ofb_err("Unknown frame info!");
		return;
	}

	ft = info->ft;
	if (!ft) {
		ofb_err("ft is NULL, grp_id %d", grp_id);
		return;
	}

	frame_rate = get_frame_rate(grp_id);
	ff_data = lookup_frame_fps_data(frame_rate);

	switch (step) {
	case FRAME_STEP_VSYNC:
		duration = (MSEC_PER_SEC / frame_rate) >> 1;
		start_frame_half_vsync_timer(info, step, duration);
		break;
	case FRAME_STEP_INPUT:
		stop_frame_step_timer(info, step);
		duration = ff_data->input_period;
		start_frame_step_timer(info, step, duration);
		break;
	case FRAME_STEP_ANIMATION:
		/* exclude pre-animation */
		if (info->prev_draw_step != FRAME_STEP_DO_FRAME_END) {
			stop_frame_step_timer(info, step);
			duration = ff_data->animation_period;
			start_frame_step_timer(info, step, duration);
		}
		break;
	case FRAME_STEP_TRAVERSAL:
		stop_frame_step_timer(info, step);
		duration = ff_data->traversal_period;
		start_frame_step_timer(info, step, duration);
		break;
	case FRAME_STEP_DO_FRAME_END:
		stop_frame_half_vsync_timer(info, step);
		stop_frame_step_timer(info, step);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(do_frame_step_hint);

static int __frame_info_init(struct frame_info *frame_info)
{
	unsigned long flags;

	memset(frame_info, 0, sizeof(struct frame_info));
	raw_spin_lock_init(&frame_info->lock);

	raw_spin_lock_irqsave(&frame_info->lock, flags);
	frame_info->frame_rate = DEFAULT_FRAME_RATE;
	frame_info->frame_interval = DEFAULT_FRAME_INTERVAL;
	frame_info->frame_max_util = SCHED_CAPACITY_SCALE;
	frame_info->frame_min_util = 0;
	frame_info->vutil_margin = DEFAULT_VUTIL_MARGIN;
	frame_info->vutil_time2max = frame_info->frame_interval / NSEC_PER_MSEC + frame_info->vutil_margin;
	frame_info->frame_state = FRAME_END;
	atomic_set(&frame_info->buffer_count, 0);
	frame_info->next_vsync = 0;
	frame_info->clear_limit = false;
	frame_info->last_compose_time = 0;

	frame_info->curr_draw_step = FRAME_STEP_DEFAULT;
	frame_info->prev_draw_step = FRAME_STEP_DEFAULT;
	frame_info->ft = NULL;
	raw_spin_unlock_irqrestore(&frame_info->lock, flags);

	return 0;
}

int frame_info_init(void)
{
	int id;
	int ret = 0;

	ret = __frame_info_init(&sf_frame_info);
	if (ret != 0)
		return ret;

	sf_frame_info.fgrp = frame_boost_groups[SF_FRAME_GROUP_ID];

	ret = __frame_info_init(&game_frame_info);
	if (ret != 0)
		return ret;

	game_frame_info.fgrp = frame_boost_groups[GAME_FRAME_GROUP_ID];

	for (id = 0; id < MULTI_FBG_NUM; id++) {
		if (ret != 0)
			break;
		ret = __frame_info_init(&multi_frame_info[id]);
		multi_frame_info[id].ft = &frame_timer[id];
		multi_frame_info[id].fgrp = frame_boost_groups[id + MULTI_FBG_ID];
	}
	return ret;
}
