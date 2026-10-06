/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_FRAME_INFO_H
#define _OPLUS_FRAME_INFO_H

#define FRAME_START	(1 << 0)
#define FRAME_END	(1 << 1)
#define MIN_FRAME_RATE 1
#define MAX_FRAME_RATE 144
#define FRAME_FPS_DATA_SIZE 4

enum frame_step {
	FRAME_STEP_DEFAULT = 0,
	FRAME_STEP_VSYNC,
	FRAME_STEP_DO_FRAME_START,
	FRAME_STEP_INPUT,
	FRAME_STEP_ANIMATION,
	FRAME_STEP_TRAVERSAL,
	FRAME_STEP_DO_FRAME_END,
	FRAME_STEP_END,
	FRAME_STEP_MAX,
};

struct frame_fps_data {
	/* rate */
	int frame_rate;

	/* step time */
	int input_period;
	int animation_period;
	int traversal_period;

	/* step boost util */
	int step_boost_util;

	/* half vsync util */
	int half_vsync_util;
};

/**
 * struct frame_info - all information related to frame draw.
 * @frame_rate: frame rate, 60, 90 or 120Hz.
 * @frame_rate_max: set by sf and app can not draw frame more than this rate.
 * @frame_interval: frame length, for example: 16.67ms for 60Hz frame rate.
 * @frame_vutil: virtual utility of this frame, nothing to do with exec
 *         time and only related to delta time from frame start.
 * @need_for_probe: If needs_suppliers is on a list, this indicates if the
 *         suppliers are needed for probe or not.
 * @frame_max_util: max util set from userspace.
 * @frame_min_util: min util set from userspace.
 * @vutil_time2max: time when virtual util set to 1024.
 * @frame_state: indicate the state of frame.
 * @last_compose_time: last client composition time.
 * @vutil_margin: adjust vutil_time2max.
 * @clear_limit: true if max/min should be set to default value in next frame
 *         state changed.
 * @draw_step: record step of draw frame, refer to enum frame_step.
 * @ft: related timer for frame rescue.
 * @fgrp: indicate frame group.
 */
struct frame_info {
	raw_spinlock_t lock;
	unsigned int frame_rate;
	unsigned int frame_interval;
	unsigned int frame_vutil;
	unsigned int frame_max_util;
	unsigned int frame_min_util;
	unsigned int vutil_time2max;
	unsigned int frame_state;
	atomic_t buffer_count;
	int next_vsync;
	int vutil_margin;
	u64 last_compose_time;
	bool clear_limit;
	int prev_draw_step;
	int curr_draw_step;
	struct frame_timer *ft;
	struct frame_group *fgrp;
};

int frame_info_init(void);

bool is_fbg(int grp_id);
bool is_multi_frame_fbg(int grp_id);
bool is_active_multi_frame_fbg(int grp_id);
struct frame_info *fbg_active_multi_frame_info(int id);
int set_static_fbg(int grp_id);
int alloc_multi_fbg(void);
void release_multi_fbg(int grp_id);

bool is_frame_fbg(int grp_id);
struct frame_info *fbg_frame_info(int grp_id);

void set_frame_state(int grp_id, unsigned int state, int buffer_count, int next_vsync);
unsigned int get_frame_state(int grp_id);
int set_frame_util_min(int grp_id, int min_util, bool clear);

bool set_frame_rate(int grp_id, unsigned int frame_rate);
bool is_high_frame_rate(int grp_id);
int get_frame_rate(int grp_id);
int set_frame_margin(int grp_id, int margin_ms);
bool check_last_compose_time(bool composition);

unsigned long get_frame_vutil(int grp_id, u64 delta, bool handler_busy, int *buffer_count);
unsigned long get_frame_putil(int grp_id, u64 delta, unsigned int frame_zone);

unsigned long frame_uclamp(int grp_id, unsigned long util);


static inline void set_frame_step(struct frame_info *info, int step)
{
	info->prev_draw_step = info->curr_draw_step;
	info->curr_draw_step = step;
}

static inline int get_frame_step(struct frame_info *info)
{
	return info->curr_draw_step;
}

int set_frame_step_by_grp(int grp_id, int step);
int get_frame_step_by_grp(int grp_id);
void do_frame_step_hint(int grp_id, int step);

struct frame_fps_data *lookup_frame_fps_data(int frame_rate);
int get_frame_step_boost_util(int frame_rate);
int get_frame_half_vsync_boost_util(int frame_rate);
#endif /* _OPLUS_FRAME_INFO_H */
