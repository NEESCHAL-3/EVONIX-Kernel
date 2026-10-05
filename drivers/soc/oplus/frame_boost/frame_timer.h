/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#ifndef _FRAME_TIMER_H
#define _FRAME_TIMER_H

#include <linux/hrtimer.h>
#include "frame_info.h"


struct frame_timer {
	struct hrtimer frame_step_timer;
	raw_spinlock_t step_lock;
	int step_state;

	struct hrtimer frame_half_vsync_timer;
	raw_spinlock_t half_vsync_lock;
	int half_vsync_state;

	struct frame_info *info;
};


void start_frame_half_vsync_timer(struct frame_info *info, int step, int duration);
void stop_frame_half_vsync_timer(struct frame_info *info, int step);
void start_frame_step_timer(struct frame_info *info, int step, int duration);
void stop_frame_step_timer(struct frame_info *info, int step);
int frame_timer_init(void);
void reset_frame_timer_by_grp(int grp_id);
#endif /* _FRAME_TIMER_H */
