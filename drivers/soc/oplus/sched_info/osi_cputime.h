/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#ifndef __OPLUS_CPU_JANK_CPUTIME_H__
#define __OPLUS_CPU_JANK_CPUTIME_H__

#include "osi_base.h"

/* per-CPU struct */
struct cputime {
	u64 last_update_time;				/* timestamp: in ms */
	u32 last_index;
	u64 running_time[JANK_WIN_CNT];
	u64 running_time32[JANK_WIN_CNT];
	u64 running_time32_scale[JANK_WIN_CNT];
};


#endif  /* endif */
