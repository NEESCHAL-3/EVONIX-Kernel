// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <uapi/linux/sched/types.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/sort.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/syscore_ops.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/cpufreq.h>
#include <linux/sched/cpufreq.h>
#include <trace/hooks/sched.h>

#include "game_ctrl.h"

static pid_t render_task_pid = -1;

static bool ed_enable = false;
static int target_fps = 0;
static int real_fps = 0;
static int frame_drop_threshold = 0;
static u64 std_frame_length;

static u64 render_frame_runtime = 0;
#define MAX_RUNTIME_HISTORY_SIZE 4
static u64 runtime_history[MAX_RUNTIME_HISTORY_SIZE];
static bool time_sensitive = false;

static bool render_running = false;
static bool render_queque_buffer_sync_binder = false;
static bool render_queque_buffer_just_now = false;
static pid_t sf_binder_task_pid = 0;

static u64 render_start_runing_ts;
static u64 new_frame_produce_ts;

static atomic_t queued_buffer_num = ATOMIC_INIT(0);
static atomic_t hrtimer_expiry_nr = ATOMIC_INIT(0);
static atomic_t render_wakeup_times = ATOMIC_INIT(0);
static atomic_t render_wakeup_too_many_times_need_boost = ATOMIC_INIT(0);

static struct hrtimer render_long_sleep_hrtimer; /* render sleep for a long time */
static struct hrtimer render_short_runtime_hrtimer; /* render short runtime over a peroid of time */

static struct hrtimer frame_drop_release_max_limits_hrtimer;
static struct hrtimer frame_short_timeout_hrtimer;
static struct hrtimer frame_long_timeout_hrtimer;
static struct hrtimer max_boot_time_hrtimer; /* If no frame produce for a long time [ex. 50ms], boost abort */

static struct kthread_work ed_work;
static struct kthread_worker ed_worker;

#define FRAME_STATS_MAX_RECENT_AVG		0
#define FRAME_STATS_MAX		1

static int arg_render_busy_time_mode = FRAME_STATS_MAX_RECENT_AVG;
static int arg_render_busy_time_us = 0;

static bool render_long_sleep_detect = false;
static atomic_t arg_render_sleep_time_us = ATOMIC_INIT(0);
static atomic_t arg_buffer2_render_sleep_time_us = ATOMIC_INIT(0); /* buffer_num == 2 */

static bool render_short_runtime_detect = false;
static atomic_t arg_render_runtime_us = ATOMIC_INIT(0);
static atomic_t arg_frame_elapsed_time_us = ATOMIC_INIT(0);

static bool render_wakeup_too_many_times_detect = false;
static atomic_t arg_render_wakeup_times = ATOMIC_INIT(0);

static int arg_buffer2_frame_timeout_offset = 20; /* allowed value: [10 ~ 30] */
static int arg_frame_drop_release_max_limits_offset = -20; /* allowed value: [-30 ~ 0] */

static bool frame_short_timeout_detect = false;
static int arg_frame_short_timeout_offset = -100; /* allowed value: [-20 ~ 100] */

static bool frame_long_timeout_detect = false;
static int arg_frame_long_timeout_offset = -100; /* allowed value: [-20 ~ 100] */

static u32 arg_max_boost_time_ms = 100;

static DEFINE_MUTEX(ed_mutex);
static DEFINE_RAW_SPINLOCK(ed_lock);

void ed_set_render_task(struct task_struct *render_task)
{
	if (render_task)
		render_task_pid = render_task->pid;
	else
		render_task_pid = -1;
}

static void update_render_frame_runtime(u64 runtime, bool reset)
{
	if (reset) {
		render_frame_runtime = 0;
	} else {
		render_frame_runtime += runtime;

		if (render_frame_runtime > std_frame_length)
			render_frame_runtime = std_frame_length;
	}

	systrace_c_printk("render_frame_runtime", render_frame_runtime);
}

static void apply_render_frame_runtime(int buffer_num)
{
	int i;
	u64 sum = 0, max = 0, avg, demand;

	for (i = 0; i < MAX_RUNTIME_HISTORY_SIZE - 1; i++) {
		runtime_history[i] = runtime_history[i + 1];
	}
	runtime_history[i] = render_frame_runtime;

	if (arg_render_busy_time_mode == FRAME_STATS_MAX_RECENT_AVG) {
		for (i = 0; i < MAX_RUNTIME_HISTORY_SIZE; i++) {
			sum += runtime_history[i];
		}
		avg = div64_u64(sum, MAX_RUNTIME_HISTORY_SIZE);
		demand = max(avg, render_frame_runtime);
	} else { /* FRAME_STATS_MAX */
		for (i = 0; i < MAX_RUNTIME_HISTORY_SIZE; i++) {
			if (runtime_history[i] > max)
				max = runtime_history[i];
		}
		demand = max;
	}

	if (buffer_num == 1 || buffer_num == 2)
		time_sensitive = demand > arg_render_busy_time_us * NSEC_PER_USEC;
	else
		time_sensitive = false;

	systrace_c_printk("demand", demand);
	systrace_c_printk("time_sensitive", time_sensitive ? 1: 0);
}

static void reset_render_wakeup_times(void)
{
	if (render_wakeup_too_many_times_detect) {
		systrace_c_printk("render_wakeup_times", atomic_read(&render_wakeup_times));
		atomic_set(&render_wakeup_times, 0);
	}
}

/* within ed_mutex mutex_lock */
static void ed_hrtimer_cancel(void)
{
	if (render_short_runtime_detect)
		hrtimer_cancel(&render_short_runtime_hrtimer);

	if (frame_short_timeout_detect) {
		hrtimer_cancel(&frame_drop_release_max_limits_hrtimer);
		hrtimer_cancel(&frame_short_timeout_hrtimer);
	}

	if (frame_long_timeout_detect)
		hrtimer_cancel(&frame_long_timeout_hrtimer);

	hrtimer_cancel(&max_boot_time_hrtimer);
}

/* within ed_mutex mutex_lock */
static void ed_hrtimer_cancel_all(void)
{
	if (render_long_sleep_detect)
		hrtimer_cancel(&render_long_sleep_hrtimer);

	ed_hrtimer_cancel();
}

/* within ed_mutex mutex_lock */
static void ed_hrtimer_start(int buffer_num)
{
	int offset;
	u64 expire_nsecs, now, elapsed = 0;

	if (buffer_num == 1)
		offset = 0;
	else if (buffer_num == 2)
		offset = arg_buffer2_frame_timeout_offset;
	else
		offset = arg_buffer2_frame_timeout_offset + 20;

	/* if render_task_pid == -1, invalid render pid, downgrade to frame timeout boost only */
	if (render_short_runtime_detect && time_sensitive && (buffer_num == 1) && (render_task_pid != -1)) {
		expire_nsecs = atomic_read(&arg_frame_elapsed_time_us) * NSEC_PER_USEC;
		hrtimer_start(&render_short_runtime_hrtimer, ktime_set(0, expire_nsecs), HRTIMER_MODE_REL);
	}

	if (frame_short_timeout_detect || frame_long_timeout_detect) {
		now = ktime_get_ns();
		if (now > new_frame_produce_ts)
			elapsed = now - new_frame_produce_ts;

		systrace_c_printk("elapsed", elapsed);

		if (frame_short_timeout_detect) {
			expire_nsecs = std_frame_length * (100 + offset + arg_frame_short_timeout_offset) / 100;
			if (elapsed > 0)
				expire_nsecs -= elapsed;
			hrtimer_start(&frame_short_timeout_hrtimer, ktime_set(0, expire_nsecs), HRTIMER_MODE_REL);

			if ((arg_frame_drop_release_max_limits_offset < 0) &&
				(target_fps - real_fps > frame_drop_threshold) &&
				(buffer_num <= 2)) {
				int frame_drop_offset = arg_frame_drop_release_max_limits_offset;
				int dividend;
				u64 expire_nsecs_frame_drop;

				if ((frame_drop_offset < -10) && (buffer_num == 2))
					frame_drop_offset = -10;
				dividend = 100 + offset + arg_frame_short_timeout_offset + frame_drop_offset;
				expire_nsecs_frame_drop = std_frame_length * dividend / 100;
				if (elapsed > 0)
					expire_nsecs_frame_drop -= elapsed;

				hrtimer_start(&frame_drop_release_max_limits_hrtimer,
					ktime_set(0, expire_nsecs_frame_drop), HRTIMER_MODE_REL);
			}
		}

		if (frame_long_timeout_detect) {
			expire_nsecs = std_frame_length * (100 + offset + arg_frame_long_timeout_offset) / 100;
			if (elapsed > 0)
				expire_nsecs -= elapsed;
			hrtimer_start(&frame_long_timeout_hrtimer, ktime_set(0, expire_nsecs), HRTIMER_MODE_REL);
		}
	}

	expire_nsecs = std_frame_length + arg_max_boost_time_ms * NSEC_PER_MSEC;
	hrtimer_start(&max_boot_time_hrtimer, ktime_set(0, expire_nsecs), HRTIMER_MODE_REL);
}

static ssize_t frame_produce_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int ret;
	int buffer_num;
	u64 frame_produce_ts;
	unsigned long flags;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%llu %d %d %d", &frame_produce_ts, &buffer_num, &real_fps, &frame_drop_threshold);
	if (ret != 4)
		return -EINVAL;

	mutex_lock(&ed_mutex);

	if (!ed_enable)
		goto unlock;

	systrace_c_printk("frame_produce", 1);
	systrace_c_printk("frame_produce", 0);
	systrace_c_printk("buffer_num", buffer_num);
	systrace_c_signed_printk("target_fps-real_fps", target_fps - real_fps);
	systrace_c_signed_printk("frame_drop_threshold", frame_drop_threshold);

	raw_spin_lock_irqsave(&ed_lock, flags);
	new_frame_produce_ts = frame_produce_ts;
	if (render_running && new_frame_produce_ts > render_start_runing_ts) {
		u64 runtime = new_frame_produce_ts - render_start_runing_ts;
		render_start_runing_ts = new_frame_produce_ts;
		update_render_frame_runtime(runtime, false);
	}
	apply_render_frame_runtime(buffer_num);
	update_render_frame_runtime(0, true);
	reset_render_wakeup_times();
	raw_spin_unlock_irqrestore(&ed_lock, flags);

	atomic_set(&queued_buffer_num, buffer_num);

	ed_hrtimer_cancel();
	kthread_cancel_work_sync(&ed_work);
	ed_freq_boost_request(ED_BOOST_NONE);
	ed_hrtimer_start(buffer_num);

unlock:
	mutex_unlock(&ed_mutex);

	return count;
}

static const struct proc_ops frame_produce_proc_ops = {
	.proc_write		= frame_produce_proc_write,
	.proc_lseek		= default_llseek,
};

static ssize_t target_fps_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int ret;
	int fps;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &fps);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&ed_mutex);
	target_fps = fps;
	if (target_fps > 0)
		std_frame_length = NSEC_PER_SEC / target_fps;
	mutex_unlock(&ed_mutex);

	return count;
}

static ssize_t target_fps_proc_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int len;

	mutex_lock(&ed_mutex);
	len = sprintf(page, "%d\n", target_fps);
	mutex_unlock(&ed_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops target_fps_proc_ops = {
	.proc_write		= target_fps_proc_write,
	.proc_read		= target_fps_proc_read,
	.proc_lseek		= default_llseek,
};

static ssize_t ed_enable_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int ret, value;
	bool enable;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &value);
	if (ret != 1)
		return -EINVAL;

	if (value != 0 && value != 1)
		return -EINVAL;

	enable = value == 1;

	mutex_lock(&ed_mutex);
	if (ed_enable != enable) {
		ed_enable = enable;
		if (!ed_enable) {
			ed_hrtimer_cancel_all();
			kthread_cancel_work_sync(&ed_work);
			ed_freq_boost_request(ED_BOOST_NONE);
			reset_render_wakeup_times();
		}
	}
	mutex_unlock(&ed_mutex);

	return count;
}

static ssize_t ed_enable_proc_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int len;

	mutex_lock(&ed_mutex);
	len = sprintf(page, "%d\n", ed_enable ? 1 : 0);
	mutex_unlock(&ed_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops ed_enable_proc_ops = {
	.proc_write		= ed_enable_proc_write,
	.proc_read		= ed_enable_proc_read,
	.proc_lseek		= default_llseek,
};

static ssize_t ed_args_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	int a_max_boost_time_ms;
	int a_buffer2_frame_timeout_offset;
	int a_frame_drop_release_max_limits_offset;
	int a_frame_short_timeout_offset;
	int a_frame_long_timeout_offset;
	int a_render_busy_time_mode;
	int a_render_busy_time_us;
	int a_render_sleep_time_us;
	int a_buffer2_render_sleep_time_us;
	int a_render_runtime_us;
	int a_frame_elapsed_time_us;
	int a_render_wakeup_times;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&ed_mutex);

	/* reset switches */
	frame_short_timeout_detect = false;
	frame_long_timeout_detect = false;
	render_long_sleep_detect = false;
	render_short_runtime_detect = false;
	render_wakeup_too_many_times_detect = false;

	/* reset arguments */
	arg_max_boost_time_ms = 100;
	arg_buffer2_frame_timeout_offset = 20;
	arg_frame_drop_release_max_limits_offset = -20;
	arg_frame_short_timeout_offset = -100;
	arg_frame_long_timeout_offset = -100;
	arg_render_busy_time_mode = FRAME_STATS_MAX_RECENT_AVG;
	arg_render_busy_time_us = 0;
	atomic_set(&arg_render_sleep_time_us, 0),
	atomic_set(&arg_buffer2_render_sleep_time_us, 0),
	atomic_set(&arg_render_runtime_us, 0),
	atomic_set(&arg_frame_elapsed_time_us, 0),
	atomic_set(&arg_render_wakeup_times, 0);

	ret = sscanf(page, "%d %d %d %d %d %d %d %d %d %d %d %d",
			&a_max_boost_time_ms,
			&a_buffer2_frame_timeout_offset,
			&a_frame_drop_release_max_limits_offset,
			&a_frame_short_timeout_offset,
			&a_frame_long_timeout_offset,
			&a_render_busy_time_mode,
			&a_render_busy_time_us,
			&a_render_sleep_time_us,
			&a_buffer2_render_sleep_time_us,
			&a_render_runtime_us,
			&a_frame_elapsed_time_us,
			&a_render_wakeup_times);

	if (ret != 12) {
		/* in order to reset switches and arguments */
		if (ret == 1 && a_max_boost_time_ms == -1)
			ret = count;
		else
			ret = -EINVAL;

		mutex_unlock(&ed_mutex);
		return ret;
	}

	if (a_max_boost_time_ms >= 50 && a_max_boost_time_ms <=  5 * 1000) {
		arg_max_boost_time_ms = a_max_boost_time_ms;
	}

	if (a_buffer2_frame_timeout_offset >= 10 && a_buffer2_frame_timeout_offset <= 30) {
		arg_buffer2_frame_timeout_offset = a_buffer2_frame_timeout_offset;
	}

	if (a_frame_drop_release_max_limits_offset >= -30 && a_frame_drop_release_max_limits_offset <= 0) {
		arg_frame_drop_release_max_limits_offset = a_frame_drop_release_max_limits_offset;
	}

	if (a_frame_short_timeout_offset >= -20 && a_frame_short_timeout_offset <= 100) {
		arg_frame_short_timeout_offset = a_frame_short_timeout_offset;
		frame_short_timeout_detect = true;
	}

	if (a_frame_long_timeout_offset >= -20 && a_frame_long_timeout_offset <= 100) {
		if (frame_short_timeout_detect &&
			(a_frame_long_timeout_offset - a_frame_short_timeout_offset >= 10)) {
			arg_frame_long_timeout_offset = a_frame_long_timeout_offset;
			frame_long_timeout_detect = true;
		}
	}

	if (a_render_busy_time_us > 0) {
		arg_render_busy_time_us = a_render_busy_time_us;

		if (a_render_busy_time_mode == FRAME_STATS_MAX) {
			arg_render_busy_time_mode = FRAME_STATS_MAX;
		}

		if (a_render_sleep_time_us > 0) {
			atomic_set(&arg_render_sleep_time_us, a_render_sleep_time_us);
			render_long_sleep_detect = true;
		}
		if (a_buffer2_render_sleep_time_us > 0) {
			atomic_set(&arg_buffer2_render_sleep_time_us, a_buffer2_render_sleep_time_us);
		}

		if (a_render_runtime_us > 0 && a_frame_elapsed_time_us > 0
			&& a_render_runtime_us < a_frame_elapsed_time_us) {
			atomic_set(&arg_render_runtime_us, a_render_runtime_us),
			atomic_set(&arg_frame_elapsed_time_us, a_frame_elapsed_time_us),
			render_short_runtime_detect = true;
		}
	}

	if (a_render_wakeup_times > 0) {
		atomic_set(&arg_render_wakeup_times, a_render_wakeup_times);
		render_wakeup_too_many_times_detect = true;
	}

	mutex_unlock(&ed_mutex);

	return count;
}

static ssize_t ed_args_proc_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	char page[256 + 64] = {0};
	int len;

	mutex_lock(&ed_mutex);
	len = sprintf(page, "mbt_ms=%d, b2ft_offset=%d, fdrml_offset=%d, fst_offset=%d, flt_offset=%d,"
		" rbt_mode=%d, rbt_us=%d, rst_us=%d, b2rst_us=%d, rrt_us=%d, fet_us=%d, rw_times=%d\n",
		arg_max_boost_time_ms,
		arg_buffer2_frame_timeout_offset,
		arg_frame_drop_release_max_limits_offset,
		arg_frame_short_timeout_offset,
		arg_frame_long_timeout_offset,
		arg_render_busy_time_mode,
		arg_render_busy_time_us,
		atomic_read(&arg_render_sleep_time_us),
		atomic_read(&arg_buffer2_render_sleep_time_us),
		atomic_read(&arg_render_runtime_us),
		atomic_read(&arg_frame_elapsed_time_us),
		atomic_read(&arg_render_wakeup_times));
	mutex_unlock(&ed_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops ed_args_proc_ops = {
	.proc_write		= ed_args_proc_write,
	.proc_read		= ed_args_proc_read,
	.proc_lseek		= default_llseek,
};

static bool is_sf_binder_task(struct task_struct *task)
{
	if (!strncmp(task->comm, "binder:", 7)) {
		struct task_struct *group_leader = task->group_leader;
		if (group_leader && !strcmp(group_leader->comm, "surfaceflinger")) {
			return true;
		}
	}

	return false;
}

void ed_render_wakeup_times_stat(struct task_struct *task)
{
	if (!ed_enable)
		return;

	if (task->pid == render_task_pid) {
		if (sf_binder_task_pid) {
			if ((current->pid == sf_binder_task_pid) && is_sf_binder_task(current)) {
				render_queque_buffer_sync_binder = true;
			} else {
				render_queque_buffer_sync_binder = false;
			}
		}
		sf_binder_task_pid = 0;

		if (render_wakeup_too_many_times_detect) {
			int a_render_wakeup_times = atomic_read(&arg_render_wakeup_times);
			if ((a_render_wakeup_times > 0)
				&& (atomic_add_return(1, &render_wakeup_times) == a_render_wakeup_times)) {
				atomic_set(&render_wakeup_too_many_times_need_boost, 1);
				kthread_queue_work(&ed_worker, &ed_work);
			}
		}
	} else if (current->pid == render_task_pid) {
		if (is_sf_binder_task(task)) {
			render_queque_buffer_just_now = true;
			sf_binder_task_pid = task->pid;
			systrace_c_printk("render_queque_buffer_sync_binder", render_queque_buffer_sync_binder ? 1 : 0);
		}
	}
}

static void sched_switch_hook(void *unused, bool preempt,
		struct task_struct *prev, struct task_struct *next, unsigned int prev_state)
{
	bool render_start = false;
	bool render_stop = false;
	u64 now;
	unsigned long flags;

	if (!ed_enable)
		return;

	if (next->pid == render_task_pid) /* render start running */
		render_start = true;
	else if (prev->pid == render_task_pid)  /* render stop running */
		render_stop = true;

	if (!render_start && !render_stop)
		return;

	now = ktime_get_ns();
	raw_spin_lock_irqsave(&ed_lock, flags);
	if (render_start) {
		render_running = true;
		render_start_runing_ts = now;
	} else { /* render_stop */
		u64 runtime;
		render_running = false;

		if (now > render_start_runing_ts) {
			runtime = now - render_start_runing_ts;
			update_render_frame_runtime(runtime, false);
		}
	}
	raw_spin_unlock_irqrestore(&ed_lock, flags);

	if (render_long_sleep_detect) {
		if (render_start) {
			hrtimer_try_to_cancel(&render_long_sleep_hrtimer);
		} else { /* render_stop */
			systrace_c_printk("prev_state", prev_state);
			if ((!render_queque_buffer_just_now || !render_queque_buffer_sync_binder)
					&& time_sensitive && (prev_state == (TASK_INTERRUPTIBLE | TASK_FREEZABLE))) {
				int buffer_num = atomic_read(&queued_buffer_num);
				int a_render_sleep_time_us = atomic_read(&arg_render_sleep_time_us);
				int a_buffer2_render_sleep_time_us = atomic_read(&arg_buffer2_render_sleep_time_us);
				u64 expire_nsecs = 0;

				if (buffer_num == 1 && a_render_sleep_time_us > 0) {
					expire_nsecs = NSEC_PER_USEC * a_render_sleep_time_us;
				} else if (buffer_num == 2 && a_buffer2_render_sleep_time_us > 0) {
					expire_nsecs = NSEC_PER_USEC * a_buffer2_render_sleep_time_us;
				}

				if (expire_nsecs > 0) {
					hrtimer_start(&render_long_sleep_hrtimer, ktime_set(0, expire_nsecs), HRTIMER_MODE_REL);
				}
			}
		}
	}

	render_queque_buffer_just_now = false;
}

static void register_ed_vendor_hooks(void)
{
	/* Register vender hook in kernel/sched/core.c */
	register_trace_sched_switch(sched_switch_hook, NULL);
}

static enum hrtimer_restart ed_hrtimer_callback(struct hrtimer *timer)
{
	int nr = 0;

	if (timer == &render_long_sleep_hrtimer)
		nr = 1;
	else if (timer == &render_short_runtime_hrtimer)
		nr = 2;
	else if (timer == &frame_drop_release_max_limits_hrtimer)
		nr = 3;
	else if (timer == &frame_short_timeout_hrtimer)
		nr = 4;
	else if (timer == &frame_long_timeout_hrtimer)
		nr = 5;
	else if (timer == &max_boot_time_hrtimer)
		nr = 6;

	if (nr > 0) {
		if (nr == 2) {
			systrace_c_printk("hrtimer_expiry_nr2", 1);
			systrace_c_printk("hrtimer_expiry_nr2", 0);
		} else {
			systrace_c_printk("hrtimer_expiry_nr", nr);
			systrace_c_printk("hrtimer_expiry_nr", 0);
		}

		atomic_set(&hrtimer_expiry_nr, nr);
		kthread_queue_work(&ed_worker, &ed_work);

		if (nr == 1) {
			if (render_short_runtime_detect)
				hrtimer_try_to_cancel(&render_short_runtime_hrtimer);
		}

		if (nr == 2) {
			if (render_long_sleep_detect)
				hrtimer_try_to_cancel(&render_long_sleep_hrtimer);
		}
	}

	return HRTIMER_NORESTART;
}

static void ed_hrtimer_init(void)
{
	hrtimer_init(&render_long_sleep_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&render_short_runtime_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&frame_drop_release_max_limits_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&frame_short_timeout_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&frame_long_timeout_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&max_boot_time_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	render_long_sleep_hrtimer.function = ed_hrtimer_callback;
	render_short_runtime_hrtimer.function = ed_hrtimer_callback;
	frame_drop_release_max_limits_hrtimer.function = ed_hrtimer_callback;
	frame_short_timeout_hrtimer.function = ed_hrtimer_callback;
	frame_long_timeout_hrtimer.function = ed_hrtimer_callback;
	max_boot_time_hrtimer.function = ed_hrtimer_callback;
}

static void ed_work_fn(struct kthread_work *work)
{
	int nr;

	nr = atomic_read(&hrtimer_expiry_nr);
	atomic_set(&hrtimer_expiry_nr, 0);

	if (!ed_enable || nr == 6) { /* ed_boost_abort */
		ed_freq_boost_request(ED_BOOST_NONE);
		return;
	}

	if (atomic_read(&render_wakeup_too_many_times_need_boost) == 1) {
		atomic_set(&render_wakeup_too_many_times_need_boost, 0);

		systrace_c_printk("render_wakeup_too_many_times", 1);
		systrace_c_printk("render_wakeup_too_many_times", 0);

		ed_freq_boost_request(ED_BOOST_EDB);
		return;
	}

	if (nr >= 1 && nr <= 5) {
		u64 now = ktime_get_ns();
		bool need_boost;
		unsigned long flags;

		raw_spin_lock_irqsave(&ed_lock, flags);
		need_boost = (now > new_frame_produce_ts) && ((now - new_frame_produce_ts) > (NSEC_PER_MSEC * 2));
		if (nr == 2) { /* render_short_running */
			if (need_boost) {
				int a_render_runtime_us = atomic_read(&arg_render_runtime_us);

				if (render_running && now > render_start_runing_ts) {
					u64 runtime = now - render_start_runing_ts;
					render_start_runing_ts = now;
					update_render_frame_runtime(runtime, false);
				}

				if (a_render_runtime_us > 0)
					need_boost = render_frame_runtime < a_render_runtime_us * NSEC_PER_USEC;
				else
					need_boost = false;

				if (need_boost && (g_debug_enable == 1)) {
					char buf[128];
					int a_frame_elapsed_time_us = atomic_read(&arg_frame_elapsed_time_us);

					snprintf(buf, sizeof(buf), "render_frame_runtime_%u_%u",
							a_render_runtime_us, a_frame_elapsed_time_us);

					systrace_c_printk(buf, render_frame_runtime);
					systrace_c_printk(buf, 0);
				}
			}
		}
		raw_spin_unlock_irqrestore(&ed_lock, flags);

		if (need_boost) {
			if (nr == 5) /* frame_long_timeout */
				ed_freq_boost_request(ED_BOOST_FLT);
			else if (nr == 4) /* frame_short_timeout */
				ed_freq_boost_request(ED_BOOST_FST);
			else if (nr == 3) /* frame drop Release Max frequency Limits */
				ed_freq_boost_request(ED_BOOST_RML);
			else /* render_long_sleep render_short_running */
				ed_freq_boost_request(ED_BOOST_EDB);
		}
	}
}

static int ed_kthread_create(void)
{
	int ret;
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

	kthread_init_work(&ed_work, ed_work_fn);
	kthread_init_worker(&ed_worker);
	thread = kthread_create(kthread_worker_fn, &ed_worker, "g_ed");
	if (IS_ERR(thread)) {
		pr_err("failed to create g_ed thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set g_ed thread SCHED_FIFO\n", __func__);
		return ret;
	}

	wake_up_process(thread);

	return 0;
}

int early_detect_init(void)
{
	int ret;

	ret = ed_kthread_create();
	if (ret)
		return ret;

	ed_hrtimer_init();

	register_ed_vendor_hooks();

	proc_create_data("frame_produce", 0220, early_detect_dir, &frame_produce_proc_ops, NULL);
	proc_create_data("target_fps", 0664, early_detect_dir, &target_fps_proc_ops, NULL);
	proc_create_data("ed_enable", 0664, early_detect_dir, &ed_enable_proc_ops, NULL);
	proc_create_data("ed_args", 0664, early_detect_dir, &ed_args_proc_ops, NULL);

	return 0;
}
