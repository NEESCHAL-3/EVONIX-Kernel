/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_SA_PRIORITY_H_
#define _OPLUS_SA_PRIORITY_H_

#include "linux/sched.h"

#define ENABLE_PRESET_VRUNTIME 1

#define PRIORITY_LEVEL_NUM     11
#define PRIO_EXEC_GAP          4000000ULL  /* 4ms */
#define NICE_EXEC_GAP           500000ULL  /* 0.5ms */
#define CFS_SCHED_MIN_GRAN      750000ULL
#define CFS_SCHED_NR_LATENCY            8
#define CFS_SCHED_LATENCY      6000000ULL
#define CFS_WAKEUP_GRAN        2000000ULL

int ux_state_to_priority(int ux_state);
int ux_type_to_nice(int type);
int ux_type_to_priority(struct oplus_task_struct *ots, int ux_type);
inline u64 max_vruntime(u64 max_vruntime, u64 vruntime);
inline u64 min_vruntime(u64 min_vrt, u64 vruntime);
int vruntime_before(u64 a_vruntime, u64 b_vruntime);
u64 calc_delta_fair_se(u64 delta, struct sched_entity *se);
void exclude_ux_vruntime(struct sched_entity *se);
void initial_prio_nice_and_vruntime(struct oplus_rq *orq, struct oplus_task_struct *ots, int ux_prio, int ux_nice);
void update_vruntime_task_detach(struct oplus_rq *orq, struct oplus_task_struct *ots);
void update_vruntime_task_attach(struct oplus_rq *orq, struct oplus_task_struct *ots);
void insert_task_to_ux_timeline(struct oplus_task_struct *ots, struct oplus_rq *orq);
void update_ux_timeline_task_change(struct oplus_rq *orq, struct oplus_task_struct *ots, int new_prio, int new_nice);
void update_ux_timeline_task_tick(struct oplus_rq *orq, struct oplus_task_struct *ots);
void update_ux_timeline_task_removal(struct oplus_rq *orq, struct oplus_task_struct *ots, __maybe_unused struct sched_entity *se, __maybe_unused bool is_curr);
bool need_resched_ux(struct oplus_rq *orq, struct oplus_task_struct *curr, unsigned long delta_exec);
bool need_wakeup_preempt(struct oplus_rq *orq, struct oplus_task_struct *curr);
void android_vh_sched_stat_runtime_handler(void *unused, struct task_struct *task, u64 delta_exec, u64 vruntime);
bool pick_next_ux_exec(struct oplus_task_struct *ots, u64 pre_exec_time, int *next_type);
int ux_max_exec_time(int types);
#endif /* _OPLUS_SA_PRIORITY_H_ */
