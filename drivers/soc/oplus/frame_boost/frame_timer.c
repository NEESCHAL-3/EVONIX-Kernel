// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#include <linux/minmax.h>
#include "frame_timer.h"
#include "frame_group.h"

struct frame_timer frame_timer[MULTI_FBG_NUM];


void start_frame_step_timer(struct frame_info *info, int step, int duration)
{
	struct frame_timer *ft;
	ktime_t k_dur;
	unsigned long flags;

	ft = info->ft;

	raw_spin_lock_irqsave(&ft->step_lock, flags);
	ft->step_state = step;
	k_dur = ms_to_ktime(duration);

	if (!hrtimer_active(&ft->frame_step_timer))
		hrtimer_start(&ft->frame_step_timer, k_dur, HRTIMER_MODE_REL);

	raw_spin_unlock_irqrestore(&ft->step_lock, flags);
}

void stop_frame_step_timer(struct frame_info *info, int step)
{
	struct frame_timer *ft;
	unsigned long flags;

	ft = info->ft;

	raw_spin_lock_irqsave(&ft->step_lock, flags);

	ft->step_state = step;
	hrtimer_cancel(&ft->frame_step_timer);

	raw_spin_unlock_irqrestore(&ft->step_lock, flags);
}

static enum hrtimer_restart frame_step_timeout(struct hrtimer *timer)
{
	struct frame_group *grp;
	struct frame_info *info;
	struct frame_timer *ft;
	int grp_id, step;
	unsigned int boost_util;

	ft = container_of(timer, struct frame_timer, frame_step_timer);
	if (!ft)
		return HRTIMER_NORESTART;

	info = ft->info;
	if (!info)
		return HRTIMER_NORESTART;

	grp = info->fgrp;
	grp_id = grp->id;

	step = get_frame_step_by_grp(grp_id);

	boost_util = get_frame_step_boost_util(info->frame_rate);

	if (is_multi_frame_fbg(grp_id)) {
		if (step >= FRAME_STEP_INPUT && step <= FRAME_STEP_TRAVERSAL) {
			if (info->frame_min_util)
				boost_util = max(info->frame_min_util, boost_util);
			set_frame_util_min(grp_id, boost_util, true);
			default_group_update_cpufreq(grp_id);
		}
	}

	return HRTIMER_NORESTART;
}

void start_frame_half_vsync_timer(struct frame_info *info, int step, int duration)
{
	struct frame_timer *ft;
	ktime_t k_dur;
	unsigned long flags;

	ft = info->ft;

	raw_spin_lock_irqsave(&ft->half_vsync_lock, flags);
	ft->half_vsync_state = step;

	k_dur = ms_to_ktime(duration);

	if (!hrtimer_active(&ft->frame_half_vsync_timer))
		hrtimer_start(&ft->frame_half_vsync_timer, k_dur, HRTIMER_MODE_REL);

	raw_spin_unlock_irqrestore(&ft->half_vsync_lock, flags);
}

void stop_frame_half_vsync_timer(struct frame_info *info, int step)
{
	struct frame_timer *ft;
	unsigned long flags;

	ft = info->ft;
	if (!ft)
		return;

	raw_spin_lock_irqsave(&ft->half_vsync_lock, flags);

	ft->half_vsync_state = step;

	hrtimer_cancel(&ft->frame_half_vsync_timer);

	raw_spin_unlock_irqrestore(&ft->half_vsync_lock, flags);
}


static enum hrtimer_restart frame_half_vsync_timeout(struct hrtimer *timer)
{
	struct frame_info *info;
	struct frame_group *grp;
	struct frame_timer *ft;
	int grp_id, step;
	unsigned int boost_util;

	ft = container_of(timer, struct frame_timer, frame_half_vsync_timer);
	if (!ft)
		return HRTIMER_NORESTART;

	info = ft->info;
	if (!info)
		return HRTIMER_NORESTART;

	grp = info->fgrp;
	grp_id = grp->id;

	step = get_frame_step_by_grp(grp_id);
	boost_util = get_frame_half_vsync_boost_util(info->frame_rate);

	if (is_multi_frame_fbg(grp_id)) {
		if ((step == FRAME_STEP_INPUT || step == FRAME_STEP_ANIMATION)) {
			if (info->prev_draw_step != FRAME_STEP_DO_FRAME_END) {
				if (info->frame_min_util)
					boost_util = max(info->frame_min_util, boost_util);
				set_frame_util_min(grp_id, boost_util, true);
				default_group_update_cpufreq(grp_id);
			}
		}
	}

	return HRTIMER_NORESTART;
}

void reset_frame_timer_by_grp(int grp_id)
{
	struct frame_info *info = fbg_active_multi_frame_info(grp_id);

	if (!info || !info->ft)
		return;

	stop_frame_half_vsync_timer(info, FRAME_STEP_DEFAULT);
	stop_frame_step_timer(info, FRAME_STEP_DEFAULT);
}
EXPORT_SYMBOL_GPL(reset_frame_timer_by_grp);

int frame_timer_init(void)
{
	struct frame_info *info;
	struct frame_timer *ft;
	int id;

	for (id = MULTI_FBG_ID; id < MULTI_FBG_ID + MULTI_FBG_NUM; id++) {
		info = fbg_frame_info(id);
		if (!info)
			return -1;

		ft = info->ft;
		if (!ft)
			return -1;

		raw_spin_lock_init(&ft->step_lock);
		hrtimer_init(&ft->frame_step_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ft->frame_step_timer.function = frame_step_timeout;
		ft->step_state = FRAME_STEP_DEFAULT;

		raw_spin_lock_init(&ft->half_vsync_lock);
		hrtimer_init(&ft->frame_half_vsync_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ft->frame_half_vsync_timer.function = frame_half_vsync_timeout;
		ft->half_vsync_state = FRAME_STEP_DEFAULT;

		ft->info = info;
	}

	return 0;
}
