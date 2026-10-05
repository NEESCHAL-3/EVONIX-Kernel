// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2024 Oplus. All rights reserved.
 */

#ifndef _FRAME_GROUP_H
#define _FRAME_GROUP_H
#include <linux/cpufreq.h>

enum STUNE_BOOST_TYPE {
	BOOST_DEF_MIGR = 0,
	BOOST_DEF_FREQ,
	BOOST_UTIL_FRAME_RATE,
	BOOST_UTIL_MIN_THRESHOLD,
	BOOST_UTIL_MIN_OBTAIN_VIEW,
	BOOST_UTIL_MIN_TIMEOUT,
	BOOST_SF_IN_GPU,
	BOOST_SF_MIGR_NONGPU,
	BOOST_SF_FREQ_NONGPU,
	BOOST_SF_MIGR_GPU,
	BOOST_SF_FREQ_GPU,
	BOOST_ED_TASK_MID_DURATION,
	BOOST_ED_TASK_MID_UTIL,
	BOOST_ED_TASK_MAX_DURATION,
	BOOST_ED_TASK_MAX_UTIL,
	BOOST_ED_TASK_TIME_OUT_DURATION,
	BOOST_MAX_TYPE,
};

/*********************************
 * frame group global data
 *********************************/
struct frame_group {
	int id;
	raw_spinlock_t lock;
	struct list_head tasks;

	u64 window_start;
	u64 prev_window_size;
	u64 window_size;

	u64 curr_window_scale;
	u64 curr_window_exec;
	u64 prev_window_scale;
	u64 prev_window_exec;

	u64 curr_end_time;
	u64 curr_end_exec;
	bool handler_busy;

	unsigned int window_busy;

	/* nr_running:
	 *     The number of running threads in the group
	 * mark_start:
	 *     Mark the start time of next load track
	 */
	int nr_running;
	u64 mark_start;

	unsigned int frame_zone;

	atomic64_t last_freq_update_time;
	atomic64_t last_util_update_time;

	/* For Surfaceflinger Process:
	 *     ui is "surfaceflinger", render is "RenderEngine"
	 * For Top Application:
	 *     ui is "UI Thread", render is "RenderThread"
	 */
	int ui_pid, render_pid, hwtid1, hwtid2;
	struct task_struct *ui, *render, *hwtask1, *hwtask2;

	/* Binder frame task information */
	int binder_thread_num;
	/*stune data for each group*/
	int stune_boost[BOOST_MAX_TYPE];
	/* Frame group task should be placed on these clusters */
	struct oplus_sched_cluster *preferred_cluster;
	struct oplus_sched_cluster *available_cluster;
	/* Util used to adjust cpu frequency */
	atomic64_t policy_util;
	atomic64_t curr_util;
};

#define MULTI_FBG_NUM 5
struct multi_fbg_id_manager {
	DECLARE_BITMAP(id_map, MULTI_FBG_NUM);
	unsigned int offset;
	rwlock_t lock;
};
enum FRAME_GROUP_ID {
	DEFAULT_FRAME_GROUP_ID = 1,
	SF_FRAME_GROUP_ID,
	GAME_FRAME_GROUP_ID,
	INPUTMETHOD_FRAME_GROUP_ID,
	MULTI_FBG_ID = 5,
	MAX_NUM_FBG_ID = MULTI_FBG_ID + MULTI_FBG_NUM,
};

enum DYNAMIC_TRANS_TYPE {
	DYNAMIC_TRANS_BINDER = 0,
	DYNAMIC_TRANS_TYPE_MAX,
};

enum freq_update_flags {
	FRAME_FORCE_UPDATE = (1 << 0),
	FRAME_NORMAL_UPDATE = (1 << 1),
};

enum {
	ED_TASK_BOOST_MID = 1,
	ED_TASK_BOOST_MAX,
};

enum fbg_err_no {
	SUCC = 0,
	INVALID_ARG,
	INVALID_OFB_MAGIC,
	INVALID_CMD_ID,
	INVALID_FBG_ID,
	INACTIVE_MULTI_FBG_ID,
	NO_FREE_MULTI_FBG,
	ERR_OTS,
	ERR_TASK,
	ERR_INFO,
	ERR_GROUP,
};

#define FRAME_ZONE       (1 << 0)
#define USER_ZONE        (1 << 1)

/*cpufreq update flags must be align with walt.h*/
#define SCHED_CPUFREQ_IMS_FRAMEBOOST    BIT(30)
#define SCHED_CPUFREQ_DEF_FRAMEBOOST    BIT(29)
#define SCHED_CPUFREQ_SF_FRAMEBOOST     BIT(28)
#define SCHED_CPUFREQ_EARLY_DET         BIT(27)

struct oplus_sched_cluster {
	struct list_head	list;
	struct cpumask	cpus;
	int id;
};

extern unsigned int ed_task_boost_type;
extern int num_sched_clusters;
/* FIXME */
#define MAX_CLS_NUM 5
extern struct oplus_sched_cluster *fb_cluster[MAX_CLS_NUM];
#ifdef CONFIG_OPLUS_ADD_CORE_CTRL_MASK
extern struct cpumask *fbg_cpu_halt_mask;
#endif

bool frame_boost_enabled(void);
bool is_fbg_task(struct task_struct *p);
int task_get_frame_group_id(int pid);
int frame_group_init(void);
u64 fbg_ktime_get_ns(void);
void fbg_add_update_freq_hook(void (*func)(struct rq *rq, unsigned int flags));
void register_frame_group_vendor_hooks(void);
int rollover_frame_group_window(int grp_id);
void set_frame_group_window_size(int grp_id, unsigned int window);
void update_frame_group_buffer_count(void);
void clear_all_static_frame_task_lock(int grp_id);
void set_ui_thread(int grp_id, int pid, int tid);
void set_render_thread(int grp_id, int pid, int tid);
void set_hwui_thread(int grp_id, int pid, int hwtid1, int hwtid2);
void set_sf_thread(int pid, int tid);
void set_renderengine_thread(int pid, int tid);
bool add_rm_related_frame_task(int grp_id, int pid, int tid, int add, int r_depth, int r_width);
bool add_task_to_game_frame_group(int tid, int add);
bool default_group_update_cpufreq(int grp_id);
void input_set_boost_start(int grp_id);
void inputmethod_update_cpufreq(int grp_id, struct task_struct *tsk);
int get_frame_group_ui(int grp_id);
static inline int check_group_condition(int fbg_cur_group, int grp_id)
{
	return fbg_cur_group && fbg_cur_group != grp_id;
}

void fbg_set_group_policy_util(int grp_id, int min_util);
bool fbg_freq_policy_util(unsigned int policy_flags, const struct cpumask *query_cpus,
	unsigned long *util);
bool set_frame_group_task_to_perfer_cpu(struct task_struct *p, int *target_cpu);
bool fbg_need_up_migration(struct task_struct *p, struct rq *rq);
bool fbg_skip_migration(struct task_struct *tsk, int src_cpu, int dst_cpu);
bool fbg_skip_rt_sync(struct rq *rq, struct task_struct *p, bool *sync);
bool check_putil_over_thresh(int grp_id, unsigned long thresh);
bool fbg_rt_task_fits_capacity(struct task_struct *tsk, int cpu);

void fbg_android_rvh_schedule_handler(struct task_struct *prev,
	struct task_struct *next, struct rq *rq);
void fbg_android_rvh_cpufreq_transition(struct cpufreq_policy *policy);

void fbg_get_frame_scale(unsigned long *frame_scale);
void fbg_get_frame_busy(unsigned int *frame_busy);
void fbg_get_prev_util(unsigned long *prev_util);
void fbg_get_curr_util(unsigned long *curr_util);
void fbg_set_end_exec(int grp_id);
int get_effect_stune_boost(struct task_struct *tsk, unsigned int type);

int info_show(struct seq_file *m, void *v);

void update_ed_task_boost_mid_duration(unsigned int ed_task_boost_mid_duration_scale);
void update_ed_task_boost_max_duration(unsigned int ed_task_boost_max_duration_scale);
void update_ed_task_boost_timeout_duration(unsigned int ed_task_boost_timeout_duration_scale);
void update_wake_up(struct task_struct *p);
bool fbg_is_ed_task(struct task_struct *tsk, u64 wall_clock);

void fbg_game_set_ed_info(int ed_duration, int ed_user_pid);
void fbg_game_get_ed_info(int *ed_duration, int *ed_user_pid);
void fbg_game_ed(struct rq *rq);

#ifdef CONFIG_OPLUS_ADD_CORE_CTRL_MASK
void init_fbg_halt_mask(struct cpumask *halt_mask);
#endif

/* EVONIX direct-call scheduler bridge */
void evonix_fbg_flush_task(struct task_struct *p);
void evonix_fbg_sched_fork(struct task_struct *p);
void evonix_fbg_try_to_wake_up(struct task_struct *p);
void evonix_fbg_new_task_stats(struct task_struct *p);

#endif /* _FRAME_GROUP_H */
