// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/ioctl.h>
#include <linux/compat.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "../sched_assist/sa_common.h"
#include "frame_boost.h"
#include "frame_group.h"
#include "frame_info.h"
#include "frame_timer.h"

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
#include "cfbt_boost.h"
#include "cfbt_boost_struct.h"
#include "cfbt_config.h"
#include <linux/uaccess.h>
#include <linux/string.h>
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

#include "frame_ioctl.h"

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
#include "frame_trace.h"
#include <../kernel/oplus_cpu/close_loop/close_loop.h>
#endif

#define FRAMEBOOST_PROC_NODE "oplus_frame_boost"
#define INVALID_VAL (INT_MIN)

static struct proc_dir_entry *frame_boost_proc;


#ifdef CONFIG_ARCH_MEDIATEK
u64 curr_frame_start;
EXPORT_SYMBOL(curr_frame_start);
u64 prev_frame_start;
EXPORT_SYMBOL(prev_frame_start);
#endif /* CONFIG_ARCH_MEDIATEK */

static void crtl_update_refresh_rate(int pid, int tid, unsigned int vsyncNs)
{
	unsigned int frame_rate =  NSEC_PER_SEC / (unsigned int)(vsyncNs);
	bool is_sf = false;
	int grp_id = 0;
	unsigned long im_flag = IM_FLAG_NONE;

	im_flag = oplus_get_im_flag(current);
	is_sf = (test_bit(IM_FLAG_SURFACEFLINGER, &im_flag));
	if (is_sf) {
		/*
		 * set sf frame rate as max frame rate
		 */
		set_frame_rate(SF_FRAME_GROUP_ID, frame_rate);
		set_frame_group_window_size(SF_FRAME_GROUP_ID, vsyncNs);
		return;
	}

	/* frame rate will be updated by UI thread(pid) and render thread(tid) */
	if (pid != current->pid && tid != current->pid)
		return;

	grp_id = task_get_frame_group_id(pid);
	if (!is_active_multi_frame_fbg(grp_id) && (grp_id != GAME_FRAME_GROUP_ID))
		return;

	if (set_frame_rate(grp_id, frame_rate))
		set_frame_group_window_size(grp_id, vsyncNs);
}

static inline int stage_mapping_step(int stage)
{
	int step;

	switch (stage) {
	case BOOST_STAGE_VSYNC_TO_INPUT:
		step = FRAME_STEP_VSYNC;
		break;
	case BOOST_STAGE_INPUT:
		step = FRAME_STEP_INPUT;
		break;
	case BOOST_STAGE_ANIMATION:
		step = FRAME_STEP_ANIMATION;
		break;
	case BOOST_STAGE_TRAVERSAL:
		step = FRAME_STEP_TRAVERSAL;
		break;
	case BOOST_STAGE_DO_FRAME_END:
		step = FRAME_STEP_DO_FRAME_END;
		break;
	case FRAME_BOOST_END: /* need to */
		step = FRAME_STEP_END;
		break;
	default:
		step = stage;
		break;
	}

	return step;
}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
static int frame_trace_enable = 1;
static int frame_trace_strace_en;
#endif
/*********************************
 * frame boost ioctl proc
 *********************************/
static long ofb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;
	int grp_id = 0;
	struct ofb_ctrl_data data;
	void __user *uarg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != OFB_MAGIC)
		return -EINVAL;

	if (_IOC_NR(cmd) >= CMD_ID_MAX)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data))) {
		ofb_err("invalid address");
		return -EFAULT;
	}

	if (unlikely(sysctl_frame_boost_debug & DEBUG_VERBOSE))
		ofb_debug("CMD_ID[%d], stage[%d], pid[%d], tid[%d] vsyncNs[%lld] buffer_count[%d] next_vsync[%d]\n",
			_IOC_NR(cmd), data.stage, data.pid, data.tid, data.vsyncNs,
			data.m_rtg.frame_buffer_count, data.m_rtg1.data);

	switch (cmd) {
	case CMD_ID_SET_FPS:
		if (data.vsyncNs < NSEC_PER_SEC / MAX_FRAME_RATE ||
		    data.vsyncNs > NSEC_PER_SEC / MIN_FRAME_RATE)
			return -EINVAL;
		crtl_update_refresh_rate(data.pid, data.tid, (unsigned int)data.vsyncNs);
		break;
	case CMD_ID_BOOST_HIT:
		/* App which is not our frame boost target may request frame vsync(like systemui),
		 * just ignore hint from them! Return zero to avoid too many androd error log
		 */

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
		if (data.stage == BOOST_TD_APP_NOTIFY) {
			if (frame_trace_enable) {
				if (frame_trace_strace_en) {
					f_systrace_c(0, "TED_TD_APP_NOTIFY");
				}
				cl_chk_td_period(true);
			}
			return 0;
		}

		if (data.stage == BOOST_RT_QBUF_END) {
			if (frame_trace_enable)
				cl_enq_update(data.pid);
		}
#endif

		if ((data.pid != current->pid) && (data.tid != current->pid) &&
					(data.stage != BOOST_STAGE_VSYNC_TO_INPUT))
				return ret;

		grp_id = task_get_frame_group_id(data.pid);
		if (grp_id <= 0)
			return grp_id;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
		if (data.stage == BOOST_RT_QBUF_END) {
			if (frame_trace_enable) {
				if (frame_trace_strace_en) {
					f_systrace_c(grp_id, "CL_DEBUG_GRPID_HINT");
				}
				if (grp_id >= MULTI_FBG_ID && grp_id < MAX_NUM_FBG_ID) {
					if (frame_trace_strace_en) {
						f_systrace_c(data.pid, "CL_DEBUG_RT_PID");
						f_systrace_c(current->pid, "CL_DEBUG_CURR_PID");
						f_systrace_c(fbg_ktime_get_ns(), "CL_DEBUG_FINISH");
					}
				}
				cl_chk_margin(data.pid);
			}
		}
#endif

		if (data.stage == BOOST_FRAME_START) {
#ifdef CONFIG_ARCH_MEDIATEK
			u64 frame_start_time = ktime_get_ns();

			if (curr_frame_start != frame_start_time)
					prev_frame_start = curr_frame_start;
			curr_frame_start = frame_start_time;
#endif /* CONFIG_ARCH_MEDIATEK */

			/*sf, inputmethod and game frame group handle frame state separately*/
			if (!is_multi_frame_fbg(grp_id))
				return -INVALID_FBG_ID;
			set_frame_state(grp_id, FRAME_START, data.m_rtg.frame_buffer_count, -1);
			rollover_frame_group_window(grp_id);
			default_group_update_cpufreq(grp_id);
		}

		if (data.stage == FRAME_BOOST_END) {
			/*sf, inputmethod and game frame group handle frame state separately*/
			if (!is_multi_frame_fbg(grp_id))
				return -INVALID_FBG_ID;

			fbg_set_end_exec(grp_id);
			set_frame_state(grp_id, FRAME_END, data.m_rtg.frame_buffer_count, data.m_rtg1.data);
			default_group_update_cpufreq(grp_id);
		}

		/* TODO stune for multi group, not for sf group. BOOST_UTIL_FRAME_RATE could be 0? */
		if (data.stage == BOOST_OBTAIN_VIEW) {
			if (get_frame_rate(grp_id) >= fbg_get_stune_boost(grp_id, BOOST_UTIL_FRAME_RATE)) {
				set_frame_util_min(grp_id, fbg_get_stune_boost(grp_id, BOOST_UTIL_MIN_OBTAIN_VIEW), true);
				default_group_update_cpufreq(grp_id);
			}
		}

		if (sysctl_frame_boost_stage_enabled && ((data.stage >= BOOST_STAGE_VSYNC_TO_INPUT &&
			data.stage <= BOOST_STAGE_TRAVERSAL) ||
			data.stage == BOOST_STAGE_DO_FRAME_END)) {
			if (!is_active_multi_frame_fbg(grp_id))
				return -INVALID_FBG_ID;
			int step;

			step = stage_mapping_step(data.stage);
			set_frame_step_by_grp(grp_id, step);
			do_frame_step_hint(grp_id, step);
		}

		if (data.stage == BOOST_SET_RENDER_THREAD)
			set_render_thread(grp_id, data.pid, data.tid);

		if (data.stage == BOOST_INPUT_START) {
			if (is_active_multi_frame_fbg(grp_id)) {
				set_frame_state(grp_id, FRAME_START, -1, 1);
				rollover_frame_group_window(grp_id);
				set_frame_util_min(grp_id, fbg_get_stune_boost(grp_id, BOOST_UTIL_MIN_TIMEOUT), true);
			} else if (grp_id != INPUTMETHOD_FRAME_GROUP_ID) {
				return ret;
			}
			input_set_boost_start(grp_id);
		}

		break;
	case CMD_ID_END_FRAME:
		/* ofb_debug("CMD_ID_END_FRAME pid:%d tid:%d", data.pid, data.tid); */
		break;
	case CMD_ID_SF_FRAME_MISSED:
		/* ofb_debug("CMD_ID_END_FRAME pid:%d tid:%d", data.pid, data.tid); */
		break;
	case CMD_ID_SF_COMPOSE_HINT:
		/* ofb_debug("CMD_ID_END_FRAME pid:%d tid:%d", data.pid, data.tid); */
		break;
	case CMD_ID_IS_HWUI_RT:
		/* ofb_debug("CMD_ID_END_FRAME pid:%d tid:%d", data.pid, data.tid); */
		break;
	case CMD_ID_SET_TASK_TAGGING:
		/* ofb_debug("CMD_ID_END_FRAME pid:%d tid:%d", data.pid, data.tid); */
		break;
	default:
		/* ret = -EINVAL; */
		break;
	}

	return ret;
}

static int fbg_set_task_preferred_cluster(void __user *uarg)
{
	struct ofb_ctrl_cluster data;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	return __fbg_set_task_preferred_cluster(data.tid, data.cluster_id);
}

static long fbg_add_task_to_group(void __user *uarg)
{
	struct ofb_key_thread_info info;
	unsigned int thread_num;
	unsigned int i;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&info, uarg, sizeof(struct ofb_key_thread_info))) {
		ofb_debug("%s: copy_from_user fail\n", __func__);
		return -EFAULT;
	}

	/* sanity check a loop boundary */
	thread_num = info.thread_num;
	if (thread_num > MAX_KEY_THREAD_NUM)
		thread_num = MAX_KEY_THREAD_NUM;

	for (i = 0; i < thread_num; i++)
		add_task_to_game_frame_group(info.tid[i], info.add);

	return 0;
}

static void get_frame_util_info(struct ofb_frame_util_info *info)
{
	memset(info, 0, sizeof(struct ofb_frame_util_info));
	fbg_get_frame_scale(&info->frame_scale);
	fbg_get_frame_busy(&info->frame_busy);
}

static long fbg_notify_frame_start(void __user *uarg)
{
	struct ofb_frame_util_info info;

	if (uarg == NULL)
		return -EINVAL;
	rollover_frame_group_window(GAME_FRAME_GROUP_ID);

	get_frame_util_info(&info);
	if (copy_to_user(uarg, &info, sizeof(struct ofb_frame_util_info))) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}

	return 0;
}

static bool is_ofb_extra_cmd(unsigned int cmd)
{
	return _IOC_TYPE(cmd) == OFB_EXTRA_MAGIC;
}

static long handle_ofb_extra_cmd(unsigned int cmd, void __user *uarg)
{
	switch (cmd) {
	case CMD_ID_SET_TASK_PREFERED_CLUSTER:
		return fbg_set_task_preferred_cluster(uarg);
	case CMD_ID_ADD_TASK_TO_GROUP:
		return fbg_add_task_to_group(uarg);
	case CMD_ID_NOTIFY_FRAME_START:
		return fbg_notify_frame_start(uarg);
	default:
		return -ENOTTY;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
static bool is_cfbt_cmd(unsigned int cmd)
{
	return _IOC_TYPE(cmd) == CFBT_MAGIC;
}

static long cfbt_set_scene_start(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_set_scene_start(&data);
	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_set_scene_end(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_set_scene_end(&data);
	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}
static long cfbt_request_frame_id(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_request_frame_id(&data);
	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_set_stage(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_set_stage(&data);
	data.header.ret = 0;
	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_add_stage_tid(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_add_stage_tid(&data);
	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_remove_stage_tid(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_remove_stage_tid(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_release_frame_id(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_release_frame_id(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_remove_tids(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_remove_common_tid(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_add_tids(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_add_common_tids(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long cfbt_set_frame_start(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_set_frame_start(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

int cfbt_notify_rescue_of_user(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_notify_rescue_of_user(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

int cfbt_notify_stop_rescue_of_user(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_notify_stop_rescue_of_user(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

int cfbt_notify_error_of_user(void __user *uarg)
{
	struct cfbt_struct data;
	int ret = -1;

	if (uarg == NULL)
		return -EINVAL;

	if (copy_from_user(&data, uarg, sizeof(data)))
		return -EFAULT;

	ret = __cfbt_notify_error_of_user(&data);

	if (copy_to_user(uarg, &data, sizeof(data)) && !ret) {
		ofb_debug("%s: copy_to_user fail\n", __func__);
		return -EFAULT;
	}
	return ret;
}

static long handle_cfbt_cmd(unsigned int cmd, void __user *uarg)
{
	if (!is_cfbt_enabled())
		return -1;

	switch (cmd) {
	case CMD_ID_CFBT_START:
		return cfbt_set_scene_start(uarg);
	case CMD_ID_CFBT_END:
		return cfbt_set_scene_end(uarg);
	case CMD_ID_CFBT_REQUEST_FRAME_ID:
		return cfbt_request_frame_id(uarg);
	case CMD_ID_CFBT_SET_STAGE:
		return cfbt_set_stage(uarg);
	case CMD_ID_CFBT_ADD_STAGE_TID:
		return cfbt_add_stage_tid(uarg);
	case CMD_ID_CFBT_REMOVE_STAGE_TID:
		return cfbt_remove_stage_tid(uarg);
	case CMD_ID_CFBT_ADD_TIDS:
		return cfbt_add_tids(uarg);
	case CMD_ID_CFBT_REMOVE_TIDS:
		return cfbt_remove_tids(uarg);
	case CMD_ID_CFBT_FRAME_START:
		return cfbt_set_frame_start(uarg);
	case CMD_ID_CFBT_FRAME_END:
		return cfbt_release_frame_id(uarg);
	case CFBT_NOTIFY_RESCUE_OF_USER:
		return cfbt_notify_rescue_of_user(uarg);
	case CFBT_NOTIFY_STOP_RESCUE_OF_USER:
		return cfbt_notify_stop_rescue_of_user(uarg);
	case CFBT_NOTIFY_ERR_OF_USER:
		return cfbt_notify_error_of_user(uarg);
	default:
		return -ENOTTY;
	}

	return 0;
}
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

/* TODO for each group */
static void setup_stune_data(struct ofb_stune_data *stune_data, int grp_id)
{
	if ((stune_data->util_frame_rate >= 0) && (stune_data->util_frame_rate <= 240))
		fbg_set_stune_boost(stune_data->util_frame_rate, grp_id, BOOST_UTIL_FRAME_RATE);
	if ((stune_data->vutil_margin >= -16) && (stune_data->vutil_margin <= 16))
		set_frame_margin(stune_data->m_rtg.group_id, stune_data->vutil_margin);
	if (is_multi_frame_fbg(grp_id)) {
		if ((stune_data->boost_freq >= 0) && (stune_data->boost_freq <= 100))
			fbg_set_stune_boost(stune_data->boost_freq, grp_id, BOOST_DEF_FREQ);
		if ((stune_data->boost_migr >= 0) && (stune_data->boost_migr <= 100))
			fbg_set_stune_boost(stune_data->boost_migr, grp_id, BOOST_DEF_MIGR);
		if ((stune_data->util_min_threshold >= 0) && (stune_data->util_min_threshold <= 1024))
			fbg_set_stune_boost(stune_data->util_min_threshold, grp_id, BOOST_UTIL_MIN_THRESHOLD);
		if ((stune_data->util_min_obtain_view >= 0) && (stune_data->util_min_obtain_view <= 1024))
			fbg_set_stune_boost(stune_data->util_min_obtain_view, grp_id, BOOST_UTIL_MIN_OBTAIN_VIEW);
		if ((stune_data->util_min_timeout >= 0) && (stune_data->util_min_timeout <= 1024))
			fbg_set_stune_boost(stune_data->util_min_timeout, grp_id, BOOST_UTIL_MIN_TIMEOUT);
		if (stune_data->ed_task_boost_mid_duration >= 0)
			fbg_set_stune_boost(stune_data->ed_task_boost_mid_duration, grp_id, BOOST_ED_TASK_MID_DURATION);
		if ((stune_data->ed_task_boost_mid_util >= 0) && (stune_data->ed_task_boost_mid_util <= 1024))
			fbg_set_stune_boost(stune_data->ed_task_boost_mid_util, grp_id, BOOST_ED_TASK_MID_UTIL);
		if (stune_data->ed_task_boost_max_duration >= 0)
			fbg_set_stune_boost(stune_data->ed_task_boost_max_duration, grp_id, BOOST_ED_TASK_MAX_DURATION);
		if ((stune_data->ed_task_boost_max_util >= 0) && (stune_data->ed_task_boost_max_util <= 1024))
			fbg_set_stune_boost(stune_data->ed_task_boost_max_util, grp_id, BOOST_ED_TASK_MAX_UTIL);
		if (stune_data->ed_task_boost_timeout_duration >= 0)
			fbg_set_stune_boost(stune_data->ed_task_boost_timeout_duration, grp_id, BOOST_ED_TASK_TIME_OUT_DURATION);
		if ((stune_data->boost_sf_freq_nongpu >= 0) && (stune_data->boost_sf_freq_nongpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_freq_nongpu, grp_id, BOOST_SF_FREQ_NONGPU);
		if ((stune_data->boost_sf_migr_nongpu >= 0) && (stune_data->boost_sf_migr_nongpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_migr_nongpu, grp_id, BOOST_SF_MIGR_NONGPU);
		if ((stune_data->boost_sf_freq_gpu >= 0) && (stune_data->boost_sf_freq_gpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_freq_gpu, grp_id, BOOST_SF_FREQ_GPU);
		if ((stune_data->boost_sf_migr_gpu >= 0) && (stune_data->boost_sf_migr_gpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_migr_gpu, grp_id, BOOST_SF_MIGR_GPU);
	} else if (grp_id == SF_FRAME_GROUP_ID) {
		if ((stune_data->boost_sf_freq_nongpu >= 0) && (stune_data->boost_sf_freq_nongpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_freq_nongpu, grp_id, BOOST_SF_FREQ_NONGPU);
		if ((stune_data->boost_sf_migr_nongpu >= 0) && (stune_data->boost_sf_migr_nongpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_migr_nongpu, grp_id, BOOST_SF_MIGR_NONGPU);
		if ((stune_data->boost_sf_freq_gpu >= 0) && (stune_data->boost_sf_freq_gpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_freq_gpu, grp_id, BOOST_SF_FREQ_GPU);
		if ((stune_data->boost_sf_migr_gpu >= 0) && (stune_data->boost_sf_migr_gpu <= 100))
			fbg_set_stune_boost(stune_data->boost_sf_migr_gpu, grp_id, BOOST_SF_MIGR_GPU);
	}
}

static long ofb_sys_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;
	int grp_id = 0;
	struct ofb_ctrl_data data;
	struct ofb_stune_data stune_data;
	void __user *uarg = (void __user *)arg;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
	if (is_cfbt_cmd(cmd)) {
		return handle_cfbt_cmd(cmd, uarg);
	}
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

#ifdef CONFIG_ARCH_MEDIATEK
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
	return 0;
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */
#endif

	if (is_ofb_extra_cmd(cmd)) {
		return handle_ofb_extra_cmd(cmd, uarg);
	}

	if (_IOC_TYPE(cmd) != OFB_MAGIC)
		return -EINVAL;

	if (_IOC_NR(cmd) >= CMD_ID_MAX)
		return -EINVAL;

	switch (cmd) {
	case CMD_ID_BOOST_HIT:
		if (copy_from_user(&data, uarg, sizeof(data))) {
			ofb_debug("invalid address");
			return -EFAULT;
		}

		if (unlikely(sysctl_frame_boost_debug & DEBUG_KMSG))
			ofb_debug("CMD_ID[%d], stage[%d], grp_id[%d], pid[%d], tid[%d]\n",
					_IOC_NR(cmd), data.stage, data.m_rtg.group_id, data.pid, data.tid);

		/* for multi group (app), game ui and render thread also use multi group */
		if (data.stage == BOOST_MOVE_FG) {
			grp_id = data.m_rtg.group_id;
			if (grp_id == -1) {
				grp_id = alloc_multi_fbg();
				ret = grp_id;
				if (grp_id < 0)
					return grp_id;
			} else if (grp_id == 0) { /*for single RTG*/
				grp_id = MULTI_FBG_ID + 1;
				ret = set_static_fbg(grp_id);
				if (ret < 0)
					return grp_id;

			} else if (!is_active_multi_frame_fbg(grp_id))
				return -INVALID_FBG_ID;
			else if (data.pid == -1 || data.tid == -1) {
				reset_frame_timer_by_grp(grp_id);
				clear_all_static_frame_task_lock(grp_id);
				release_multi_fbg(grp_id);
				return grp_id;
			}
			set_ui_thread(grp_id, data.pid, data.tid);
			set_render_thread(grp_id, data.pid, data.tid);
			set_frame_state(grp_id, FRAME_END, 0, 0);
			rollover_frame_group_window(grp_id);
			ret = grp_id;
		}

		/* input method app move to top */
		if (data.stage == BOOST_MOVE_FG_IMS) {
			grp_id = INPUTMETHOD_FRAME_GROUP_ID;
			if (data.pid == -1 || data.tid == -1) {
				clear_all_static_frame_task_lock(grp_id);
				return grp_id;
			}
			set_ui_thread(grp_id, data.pid, data.tid);
			set_render_thread(grp_id, data.pid, data.tid);
			/* bypass hwui setting for stability issue
			set_hwui_thread(grp_id, data.pid, data.hwtid1, data.hwtid2);
			*/
			/*TODO get inputmethod group stune data */
			fbg_set_group_policy_util(grp_id, fbg_get_stune_boost(grp_id, BOOST_UTIL_MIN_TIMEOUT));
			ret = grp_id;
		}

		if (data.stage == BOOST_ADD_FRAME_TASK) {
			/* Should this stage suport SF/GAME/INPUT group? */
			grp_id = data.m_rtg.group_id;
			if (grp_id == 0)
				grp_id = MULTI_FBG_ID + 1;
			if (!is_active_multi_frame_fbg(grp_id))
				return -INVALID_FBG_ID;
			if (add_rm_related_frame_task(grp_id, data.pid, data.tid, data.capacity_need,
					data.m_rtg.related_depth, data.m_rtg1.related_width))
				ret = grp_id;
			else
				ret = 0;
		}

		if (data.stage == BOOST_ADD_FRAME_TASK_IMS) {
			grp_id = INPUTMETHOD_FRAME_GROUP_ID;
			if (add_rm_related_frame_task(grp_id, data.pid, data.tid, data.capacity_need, data.m_rtg.related_depth, data.m_rtg1.related_width))
				ret = grp_id;
			else
				ret = 0;
		}

		break;
	case CMD_ID_SET_SF_MSG_TRANS:
		if (copy_from_user(&data, uarg, sizeof(data))) {
			ofb_debug("invalid address");
			return -EFAULT;
		}

		if (unlikely(sysctl_frame_boost_debug & DEBUG_VERBOSE))
			ofb_debug("CMD_ID[%d], stage[%d], pid[%d], tid[%d]\n",
					_IOC_NR(cmd), data.stage, data.pid, data.tid);

		if (data.stage == BOOST_MSG_TRANS_START) {
			rollover_frame_group_window(SF_FRAME_GROUP_ID);
			update_frame_group_buffer_count();
		}

		if (data.stage == BOOST_SF_EXECUTE) {
			if (data.pid == data.tid) {
				set_sf_thread(data.pid, data.tid);
			} else {
				set_renderengine_thread(data.pid, data.tid);
			}
		}

		break;
	case CMD_ID_BOOST_STUNE: {
		if (copy_from_user(&stune_data, uarg, sizeof(stune_data))) {
			ofb_debug("invalid address");
			return -EFAULT;
		}
		/* TODO stune for group id */
		grp_id = stune_data.m_rtg.group_id;
		if (!is_fbg(grp_id))
			return -INVALID_FBG_ID;

		if (STUNE_SF == stune_data.boost_freq && grp_id != SF_FRAME_GROUP_ID) {
			return -INVALID_FBG_ID;
		}

		if (grp_id == SF_FRAME_GROUP_ID) {
			if (STUNE_DEF == stune_data.boost_freq) {
				stune_data.boost_freq = 0;

				stune_data.boost_sf_freq_nongpu = 0;
				stune_data.boost_sf_migr_nongpu = 0;
				#ifdef CONFIG_ARCH_MEDIATEK
				stune_data.boost_sf_freq_gpu = 60;
				stune_data.boost_sf_migr_gpu = 60;
				#else
				stune_data.boost_sf_freq_gpu = 30;
				stune_data.boost_sf_migr_gpu = 30;
				#endif /* CONFIG_ARCH_MEDIATEK */
			} else if (STUNE_SF == stune_data.boost_freq) {
				stune_data.boost_freq = 0;
			}
		}
		setup_stune_data(&stune_data, grp_id);
		}
		break;
	case CMD_ID_BOOST_STUNE_GPU: {
		bool boost_allow;

		if (copy_from_user(&stune_data, uarg, sizeof(stune_data))) {
			ofb_debug("invalid address");
			return -EFAULT;
		}

		/* This frame is not using client composition if data.level is zero.
		* But we still keep client composition setting with one frame extension.
		*/
		/* now sf control stune_data itself */
		/* if (check_last_compose_time(stune_data.m_rtg.level) && !stune_data.m_rtg.level)
			boost_allow = false; */

		boost_allow = !!stune_data.m_rtg.level;
		fbg_set_stune_boost(boost_allow, SF_FRAME_GROUP_ID, BOOST_SF_IN_GPU);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
struct ofb_compat_frame_util_info {
	compat_ulong_t frame_scale;
	__u32 frame_busy;
};
#define CMD_ID_COMPAT_NOTIFY_FRAME_START \
	_IOWR(OFB_EXTRA_MAGIC, NOTIFY_FRAME_START, struct ofb_compat_frame_util_info)

static long ofb_ctrl_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return ofb_ioctl(file, cmd, (unsigned long)(compat_ptr(arg)));
}

static long ofb_sys_ctrl_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ofb_compat_frame_util_info result;
	struct ofb_frame_util_info info;
	void __user *uarg = compat_ptr(arg);

	if (cmd != CMD_ID_COMPAT_NOTIFY_FRAME_START)
		return ofb_sys_ioctl(file, cmd, (unsigned long)uarg);
	if (!access_ok(uarg, sizeof(result)))
		return -EFAULT;
	rollover_frame_group_window(GAME_FRAME_GROUP_ID);
	get_frame_util_info(&info);
	if (info.frame_scale > U32_MAX)
		return -EOVERFLOW;
	result.frame_scale = info.frame_scale;
	result.frame_busy = info.frame_busy;
	return copy_to_user(uarg, &result, sizeof(result)) ? -EFAULT : 0;
}
#endif

static int ofb_ctrl_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int ofb_ctrl_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct proc_ops ofb_ctrl_fops = {
	.proc_ioctl	= ofb_ioctl,
	.proc_open	= ofb_ctrl_open,
	.proc_release	= ofb_ctrl_release,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= ofb_ctrl_compat_ioctl,
#endif
	.proc_lseek		= default_llseek,
};

static const struct proc_ops ofb_sys_ctrl_fops = {
	.proc_ioctl	= ofb_sys_ioctl,
	.proc_open	= ofb_ctrl_open,
	.proc_release	= ofb_ctrl_release,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= ofb_sys_ctrl_compat_ioctl,
#endif
	.proc_lseek 	= default_llseek,
};

static ssize_t proc_stune_boost_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128], *cursor = buffer, *token;
	int values[3], argc = 0, ret;
	int grp_id, boost_type, boost_val;

	if (!count)
		return -EINVAL;
	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	while ((token = strsep(&cursor, " \t\r\n"))) {
		if (!*token)
			continue;
		if (argc >= ARRAY_SIZE(values))
			return -EINVAL;
		ret = kstrtoint(token, 10, &values[argc++]);
		if (ret)
			return ret;
	}
	if (argc != ARRAY_SIZE(values))
		return -EINVAL;
	grp_id = values[0];
	boost_type = values[1];
	boost_val = values[2];
	if (grp_id <= DEFAULT_FRAME_GROUP_ID || grp_id >= MAX_NUM_FBG_ID ||
	    boost_type < 0 || boost_type >= BOOST_MAX_TYPE)
		return -EINVAL;
	if (boost_type == BOOST_UTIL_FRAME_RATE)
		boost_val = clamp(boost_val, 0, 240);
	else if (boost_type == BOOST_UTIL_MIN_THRESHOLD ||
		 boost_type == BOOST_UTIL_MIN_OBTAIN_VIEW ||
		 boost_type == BOOST_UTIL_MIN_TIMEOUT)
		boost_val = clamp(boost_val, 0, 1024);
	else if (boost_type != BOOST_ED_TASK_MID_DURATION &&
		 boost_type != BOOST_ED_TASK_MID_UTIL &&
		 boost_type != BOOST_ED_TASK_MAX_DURATION &&
		 boost_type != BOOST_ED_TASK_MAX_UTIL &&
		 boost_type != BOOST_ED_TASK_TIME_OUT_DURATION)
		boost_val = min(boost_val, 100);
	fbg_set_stune_boost(boost_val, grp_id, boost_type);
	return count;
}

static char *get_stune_boost_name(int type)
{
	switch (type) {
	case BOOST_DEF_MIGR:
		return "migr";
	case BOOST_DEF_FREQ:
		return "freq";
	case BOOST_UTIL_FRAME_RATE:
		return "fps";
	case BOOST_UTIL_MIN_THRESHOLD:
		return "min_threshold";
	case BOOST_UTIL_MIN_OBTAIN_VIEW:
		return "min_obtain_view";
	case BOOST_UTIL_MIN_TIMEOUT:
		return "min_timeout";
	case BOOST_SF_IN_GPU:
		return "sf_in_gpu";
	case BOOST_SF_MIGR_NONGPU:
		return "sf_migr_nongpu";
	case BOOST_SF_FREQ_NONGPU:
		return "sf_freq_nongpu";
	case BOOST_SF_MIGR_GPU:
		return "sf_migr_gpu";
	case BOOST_SF_FREQ_GPU:
		return "sf_freq_gpu";
	case BOOST_ED_TASK_MID_DURATION:
		return "ed_min_duration";
	case BOOST_ED_TASK_MID_UTIL:
		return "ed_min_util";
	case BOOST_ED_TASK_MAX_DURATION:
		return "ed_max_duration";
	case BOOST_ED_TASK_MAX_UTIL:
		return "ed_max_util";
	case BOOST_ED_TASK_TIME_OUT_DURATION:
		return "ed_timeout";
	default:
		return "unknown";
	}
}

static int proc_stune_boost_show(struct seq_file *m, void *v)
{
	int grp_id, type;

	for (grp_id = 1; grp_id < MAX_NUM_FBG_ID; grp_id++) {
		seq_printf(m, "grp_id:%d\n", grp_id);
		for (type = 0; type < BOOST_MAX_TYPE; type++)
			seq_printf(m, "%s:%d ", get_stune_boost_name(type),
				   fbg_get_stune_boost(grp_id, type));
		seq_putc(m, '\n');
	}
	seq_putc(m, '\n');
	return 0;
}

static int proc_stune_boost_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_stune_boost_show, NULL);
}

static const struct proc_ops ofb_stune_boost_fops = {
	.proc_open = proc_stune_boost_open,
	.proc_write = proc_stune_boost_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int info_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, info_show, NULL);
}

static const struct proc_ops ofb_frame_group_info_fops = {
	.proc_open	= info_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t proc_game_ed_info_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128] = {0};
	int ret;
	int ed_duration;
	int ed_user_pid;

	if (count >= sizeof(buffer))
		return -E2BIG;
	ret = simple_write_to_buffer(buffer, sizeof(buffer) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(buffer, "%d %d", &ed_duration, &ed_user_pid);
	if (ret != 2)
		return -EINVAL;

	fbg_game_set_ed_info(ed_duration, ed_user_pid);

	return count;
}

static ssize_t proc_game_ed_info_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128];
	size_t len;
	int ed_duration;
	int ed_user_pid;

	fbg_game_get_ed_info(&ed_duration, &ed_user_pid);
	len = snprintf(buffer, sizeof(buffer), "ed_duration = %d ns, ed_user_pid = %d\n",
		ed_duration, ed_user_pid);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops ofb_game_ed_info_fops = {
	.proc_write		= proc_game_ed_info_write,
	.proc_read		= proc_game_ed_info_read,
	.proc_lseek		= default_llseek,
};

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
static ssize_t proc_trace_frame_enable_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[8] = {0};
	int ret;

	if (count >= sizeof(buffer))
		return -E2BIG;
	ret = simple_write_to_buffer(buffer, sizeof(buffer) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(buffer, "%d", &frame_trace_enable);
	if (ret != 1)
		return -EINVAL;

	return count;
}

static ssize_t proc_trace_frame_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[8];
	size_t len;

	len = snprintf(buffer, sizeof(buffer), "%d\n", frame_trace_enable);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops ofb_frame_trace_en_fops = {
	.proc_write		= proc_trace_frame_enable_write,
	.proc_read		= proc_trace_frame_enable_read,
	.proc_lseek		= default_llseek,
};

static ssize_t proc_ft_strace_en_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[8] = {0};
	int ret;

	if (count >= sizeof(buffer))
		return -E2BIG;
	ret = simple_write_to_buffer(buffer, sizeof(buffer) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(buffer, "%d", &frame_trace_strace_en);
	if (ret != 1)
		return -EINVAL;

	return count;
}

static ssize_t proc_ft_strace_en_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[8];
	size_t len;

	len = snprintf(buffer, sizeof(buffer), "%d\n", frame_trace_strace_en);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops ofb_ft_strace_en_fops = {
	.proc_write		= proc_ft_strace_en_write,
	.proc_read		= proc_ft_strace_en_read,
	.proc_lseek		= default_llseek,
};

static void cl_frame_ioctl_init(void)
{
	struct proc_dir_entry *cl_root;

	cl_root = cl_get_default_proc_dir_entry();
	if (!cl_root)
		return;

	/* don't block fbg init process */
	proc_create("trace_frame_enable", (S_IRUGO|S_IWUSR|S_IWGRP), cl_root, &ofb_frame_trace_en_fops);
	proc_create("cl_frame_ctl", (S_IRUGO|S_IWUGO), cl_root, &ofb_frame_trace_en_fops);
	proc_create("trace_frame_strace_en", (S_IRUGO|S_IWUSR|S_IWGRP), cl_root, &ofb_ft_strace_en_fops);
}
#endif /* CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP */

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
static ssize_t proc_cfbt_debug_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	enable_tracing(val);

	return count;
}

static ssize_t proc_cfbt_enable_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	enable_cfbt(val);

	return count;
}

static ssize_t proc_cfbt_suspend_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	suspend_cfbt(val);

	return count;
}

static ssize_t proc_cfbt_util_down_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	set_cfbt_util_down(val);

	return count;
}

#define MAX_INPUT_LEN 100
static ssize_t proc_cfbt_rescue_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	char input[MAX_INPUT_LEN];
	int i = 0;
	char *token;

	if (count > MAX_INPUT_LEN - 1)
		return -EINVAL;

	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	input[count] = '\0';

	token = strsep((char **)&input, " ");
	while (token && i < MAX_USER_CFG_COUNT) {
		sscanf(token, "%d", &user_configurations[i]);
		token = strsep(NULL, " ");
		i++;
	}

	return count;
}
static ssize_t proc_cfbt_debug_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[20];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "debug_enabled=%d\n", is_tracing_enabled());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_cfbt_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[20];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "%d\n", is_cfbt_enabled());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_cfbt_suspend_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[20];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "%d\n", is_cfbt_suspend());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_cfbt_util_down_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[20];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "%d\n", get_cfbt_util_down());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_cfbt_rescue_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128];
	char rescue_time[128] = {'\0'};
	size_t len = 0;

	cfb_get_rescue_rtime(rescue_time, 128);
	len = snprintf(buffer, sizeof(buffer), "state:%d %d %d %d %d %d, avg:%s\n", user_configurations[0], user_configurations[1],
		user_configurations[2], user_configurations[3], user_configurations[4], user_configurations[5], (char *)rescue_time);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static int parse_input(const char *buf, size_t count)
{
	char input[128] = {0};
	char *token, *cur;
	int ret = 0;
	int scene;
	u64 interval;
	char *sep = NULL;

	if (count == 0 || count >= sizeof(input)) {
		pr_warn("Input exceeds 128 bytes!\n");
		return -EINVAL;
	}

	strncpy(input, buf, count);

	cur = input;
	while ((token = strsep(&cur, ",")) != NULL) {
		sep = strchr(token, ':');
		if (!sep) {
			pr_warn("Invalid format: %s\n", token);
			ret = -EINVAL;
			continue;
		}
		*sep = '\0';

		if (kstrtoint(token, 10, &scene) || kstrtou64(sep + 1, 10, &interval)) {
			pr_warn("Invalid number in: %s\n", token);
			ret = -EINVAL;
			continue;
		}

		if (interval == 0) {
			pr_warn("Invalid interval:%llu (must >0)\n", interval);
			ret = -EINVAL;
			continue;
		}

		update_scene_interval(scene, interval);
	}

	return ret;
}

static int scene_proc_show(struct seq_file *m, void *v)
{
	print_scene_configs(m);
	return 0;
}

/* data format: echo "2:80000000,3:60000000" */
static ssize_t scene_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	char kbuf[128] = {0};

	if (count == 0 || count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	return parse_input(kbuf, count) ?: count;
}

static int scene_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, scene_proc_show, NULL);
}

static const struct proc_ops proc_cfbt_debug_fops = {
	.proc_write		= proc_cfbt_debug_write,
	.proc_read		= proc_cfbt_debug_read,
	.proc_lseek		= default_llseek,
};

static const struct proc_ops proc_cfbt_enable_fops = {
	.proc_write		= proc_cfbt_enable_write,
	.proc_read		= proc_cfbt_enable_read,
	.proc_lseek		= default_llseek,
};

static const struct proc_ops proc_cfbt_suspend_fops = {
	.proc_write		= proc_cfbt_suspend_write,
	.proc_read		= proc_cfbt_suspend_read,
	.proc_lseek		= default_llseek,
};

static const struct proc_ops proc_cfbt_util_down_fops = {
	.proc_write		= proc_cfbt_util_down_write,
	.proc_read		= proc_cfbt_util_down_read,
	.proc_lseek		= default_llseek,
};

static const struct proc_ops proc_cfbt_rescue_fops = {
	.proc_write		= proc_cfbt_rescue_write,
	.proc_read		= proc_cfbt_rescue_read,
	.proc_lseek		= default_llseek,
};

static const struct proc_ops proc_cfbt_scene_ops = {
	.proc_open = scene_proc_open,
	.proc_read = seq_read,
	.proc_write = scene_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t proc_cfbt_select_opt_enable_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	enable_selection_option(val);

	return count;
}

static ssize_t proc_cfbt_select_opt_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[30];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "cfbt_select_opt_enable=%d\n", is_selection_option_enabled());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops proc_cfbt_select_opt_enable_fops = {
	.proc_write		= proc_cfbt_select_opt_enable_write,
	.proc_read		= proc_cfbt_select_opt_enable_read,
	.proc_lseek		= default_llseek,
};

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS_FDRIVE)
static ssize_t proc_cfbt_cx_opt_enable_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	enable_cx_opt(val);

	return count;
}

static ssize_t proc_cfbt_cx_opt_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[30];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "cfbt_cx_opt_enable=%d\n", is_enable_cx_opt());

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops proc_cfbt_cx_opt_enable_fops = {
	.proc_write		= proc_cfbt_cx_opt_enable_write,
	.proc_read		= proc_cfbt_cx_opt_enable_read,
	.proc_lseek		= default_llseek,
};
#endif /* CONFIG_OPLUS_FEATURE_GEAS_FDRIVE */
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

#define GLOBAL_SYSTEM_UID KUIDT_INIT(1000)
#define GLOBAL_SYSTEM_GID KGIDT_INIT(1000)
int frame_ioctl_init(void)
{
	int ret = 0;
	struct proc_dir_entry *pentry;

	frame_boost_proc = proc_mkdir(FRAMEBOOST_PROC_NODE, NULL);
	if (!frame_boost_proc)
		return -ENOMEM;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS_FDRIVE)
	pentry = proc_create("cfbt_cx_opt_enable", 0666, frame_boost_proc, &proc_cfbt_cx_opt_enable_fops);
	if (!pentry)
		goto ERROR_INIT;
#endif /* CONFIG_OPLUS_FEATURE_GEAS_FDRIVE */
	pentry = proc_create("cfbt_select_opt_enable", 0666, frame_boost_proc, &proc_cfbt_select_opt_enable_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_debug", 0666, frame_boost_proc, &proc_cfbt_debug_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_enable", 0666, frame_boost_proc, &proc_cfbt_enable_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_suspend", 0666, frame_boost_proc, &proc_cfbt_suspend_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_util_down", 0666, frame_boost_proc, &proc_cfbt_util_down_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_rescue", 0666, frame_boost_proc, &proc_cfbt_rescue_fops);
	if (!pentry)
		goto ERROR_INIT;
	pentry = proc_create("cfbt_scene", 0666, frame_boost_proc, &proc_cfbt_scene_ops);
	if (!pentry)
		goto ERROR_INIT;
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

	pentry = proc_create("ctrl", 0666, frame_boost_proc, &ofb_ctrl_fops);
	if (!pentry)
		goto ERROR_INIT;

	pentry = proc_create("sys_ctrl", 0660, frame_boost_proc, &ofb_sys_ctrl_fops);
	if (!pentry) {
		goto ERROR_INIT;
	} else {
		proc_set_user(pentry, GLOBAL_SYSTEM_UID, GLOBAL_SYSTEM_GID);
	}

	pentry = proc_create("stune_boost", (S_IRUGO|S_IWUSR|S_IWGRP), frame_boost_proc, &ofb_stune_boost_fops);
	if (!pentry)
		goto ERROR_INIT;

	pentry = proc_create("info", S_IRUGO, frame_boost_proc, &ofb_frame_group_info_fops);
	if (!pentry)
		goto ERROR_INIT;

	pentry = proc_create("game_ed_info", (S_IRUGO|S_IWUSR|S_IWGRP), frame_boost_proc, &ofb_game_ed_info_fops);
	if (!pentry)
		goto ERROR_INIT;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_CLOSE_LOOP)
	cl_frame_ioctl_init();
#endif

	return ret;

ERROR_INIT:
	proc_remove(frame_boost_proc);
	frame_boost_proc = NULL;
	return -ENOMEM;
}

int frame_ioctl_exit(void)
{
	int ret = 0;

	proc_remove(frame_boost_proc);
	frame_boost_proc = NULL;

	return ret;
}
