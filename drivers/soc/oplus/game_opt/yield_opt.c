#include "yield_opt.h"
#include "game_ctrl.h"

int game_pid_yield = 0;

struct yield_opt_params yield_opt_params = {
	.enable = 0,
	.frame_per_sec = 120,
	.frame_time_ns = NSEC_PER_SEC / 120,
	.yield_headroom = 10,
};

DEFINE_PER_CPU(struct sched_yield_state, ystate);

static int yield_opt_param_show(struct seq_file *m, void *v)
{
	seq_printf(m, "yield_opt:{\"enable\":%d; \"frame_per_sec\":%d; \"headroom\":%d}\n",
			yield_opt_params.enable, yield_opt_params.frame_per_sec, yield_opt_params.yield_headroom);
	return 0;
}

static int yield_opt_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yield_opt_param_show, pde_data(inode));
}

static ssize_t yield_opt_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char *data;
	int enable_tmp, frame_per_sec_tmp, yield_headroom_tmp, cpu;
	unsigned long flags;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if(copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d %d %d", &enable_tmp, &frame_per_sec_tmp, &yield_headroom_tmp) != 3) {
		kfree(data);
		return -EINVAL;
	}

	if ((enable_tmp != 0 && enable_tmp != 1) ||
		(yield_headroom_tmp < 1 || yield_headroom_tmp > 20)) {
		kfree(data);
		return -EINVAL;
	}

	yield_opt_params.frame_time_ns = NSEC_PER_SEC / frame_per_sec_tmp;
	yield_opt_params.frame_per_sec = frame_per_sec_tmp;
	yield_opt_params.yield_headroom = yield_headroom_tmp;
	yield_opt_params.enable = enable_tmp;

	for_each_possible_cpu(cpu) {
		struct sched_yield_state *ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);
		ys->last_yield_time = 0;
		ys->last_update_time = 0;
		ys->sleep_end = 0;
		ys->yield_cnt = 0;
		ys->yield_cnt_after_sleep = 0;
		ys->sleep = 0;
		ys->sleep_times = 0;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}

	kfree(data);
	return count;
}

static const struct proc_ops yield_opt_proc_ops = {
	.proc_write		= yield_opt_proc_write,
    .proc_open      = yield_opt_proc_open,
    .proc_read      = seq_read,
	.proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static int yield_pid_param_show(struct seq_file *m, void *v)
{
	seq_printf(m, "gamepid:%d\n", game_pid_yield);
	return 0;
}

static int yield_pid_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yield_pid_param_show, pde_data(inode));
}

static ssize_t yield_pid_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char *data;
	int pid_tmp;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if(copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d", &pid_tmp) != 1) {
		kfree(data);
		return -EINVAL;
	}

	game_pid_yield = pid_tmp;

	kfree(data);
	return count;
}

static const struct proc_ops yield_pid_proc_ops = {
	.proc_write		= yield_pid_proc_write,
    .proc_open      = yield_pid_proc_open,
    .proc_read      = seq_read,
	.proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static inline void yield_state_update(struct sched_yield_state *ys)
{
	if (!raw_spin_is_locked(&ys->lock))
		return;
	int yield_headroom = yield_opt_params.yield_headroom;

	if (ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH || ys->sleep_times > 1
						|| ys->yield_cnt_after_sleep > yield_headroom) {
		ys->sleep = min(ys->sleep + yield_headroom * YIELD_DURATION, MAX_YIELD_SLEEP);
	} else if (!ys->yield_cnt && (ys->sleep_times == 1) && !ys->yield_cnt_after_sleep) {
		ys->sleep = max(ys->sleep - yield_headroom * YIELD_DURATION, MIN_YIELD_SLEEP);
	}
	ys->yield_cnt = 0;
	ys->sleep_times = 0;
	ys->yield_cnt_after_sleep = 0;
}

static void android_rvh_before_do_sched_yield_handler(void *unused, long *skip)
{
	if (!yield_opt_params.enable)
		return;
	unsigned long flags, sleep_now = 0;
	struct sched_yield_state *ys;
	int cpu = raw_smp_processor_id(), cont_yield, new_frame;
	int frame_time_ns = yield_opt_params.frame_time_ns, yield_headroom = yield_opt_params.yield_headroom;
	u64 wc;
	struct rq *rq = cpu_rq(cpu);
	if (rq->curr->pid != game_pid_yield || !(*skip)) {
		wc = sched_clock();
		ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);

		cont_yield = (wc - ys->last_yield_time) < MIN_YIELD_SLEEP;
		new_frame = (wc - ys->last_update_time) > (frame_time_ns >> 1);

		if (!cont_yield && new_frame) {
			yield_state_update(ys);
			ys->last_update_time = wc;
			ys->sleep_end = ys->last_yield_time + frame_time_ns - yield_headroom * YIELD_DURATION;
		}

		if (ys->sleep > MIN_YIELD_SLEEP || ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH) {
			*skip = 1;

			sleep_now = ys->sleep_times ?
							max(ys->sleep >> ys->sleep_times, MIN_YIELD_SLEEP):ys->sleep;
			if (wc + sleep_now > ys->sleep_end) {
				u64 delta = ys->sleep_end - wc;
				if (ys->sleep_end > wc && delta > 3 * YIELD_DURATION)
					sleep_now = delta;
				else
					sleep_now = 0;
			}
			raw_spin_unlock_irqrestore(&ys->lock, flags);
			if (sleep_now) {
				sleep_now = div64_u64(sleep_now, 1000);
				usleep_range_state(sleep_now, sleep_now, TASK_IDLE);
			}
			ys->sleep_times++;
			ys->last_yield_time = sched_clock();
			return;
		}
		if (ys->sleep_times)
			ys->yield_cnt_after_sleep++;
		else
			(ys->yield_cnt)++;
		ys->last_yield_time = wc;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}
}

static void register_yield_vendor_hook(void)
{
	int ret;

	REGISTER_TRACE_VH(android_rvh_before_do_sched_yield,
				android_rvh_before_do_sched_yield_handler);
}

int yield_opt_init(void)
{
    register_yield_vendor_hook();

    proc_create_data("yield_opt", 0664, game_opt_dir, &yield_opt_proc_ops, NULL);
    proc_create_data("yield_pid", 0664, game_opt_dir, &yield_pid_proc_ops, NULL);
    return 0;
}
