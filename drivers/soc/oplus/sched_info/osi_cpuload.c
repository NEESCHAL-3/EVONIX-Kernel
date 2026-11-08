// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <uapi/linux/sched/types.h>

#include "osi_hotthread.h"
#include "osi_freq.h"
#include "osi_cpuload.h"
#include "osi_compat_runtime.h"
#include "osi_cputime.h"
#include "osi_topology.h"
#include "osi_tasktrack.h"


struct cputime cputime[CPU_NUMS];
struct cputime cputime_snap[CPU_NUMS];
static u64 cpustat_bak[CPU_NUMS][NR_STATS];



static DEFINE_PER_CPU(u64, jankinfo_last_account_tick);

void jankinfo_update_time_info(struct rq *rq,
				struct task_struct *p, u64 time)
{
	u32 i = 0;
	u64 now;
	u32 cpu;
	unsigned long irq_flags;
	u64 last_update_time;
	u32 now_idx, last_idx, idx;
	struct kernel_cpustat *kcs = NULL;
	u64 delta, delta_a, delta_cnt, delta_b;
	u64 runtime = 0, runtime_a, runtime_cnt, runtime_b;
	u64 total_time, calldelta;

	if (unlikely(!p))
		return;

	cpu = task_cpu(p);

	last_update_time = cputime[cpu].last_update_time;
	last_idx = cputime[cpu].last_index;

	/*
	 * Note: the jiffies value will overflow 5 minutes after boot
	 */
	now = ktime_get_ns();
	if (unlikely(now < last_update_time)) {
		cputime[cpu].last_update_time = now;
		delta = 0;
	} else {
		delta = now - last_update_time;
	}

	/*
	 * This function will be called on each cpu, we need to
	 * make sure that this function is called once per tick
	 */
	local_irq_save(irq_flags);
	calldelta = now - per_cpu(jankinfo_last_account_tick, cpu);
	if (calldelta < TICK_NSEC) {
		local_irq_restore(irq_flags);
		return;
	}
	per_cpu(jankinfo_last_account_tick, cpu) = now;
	local_irq_restore(irq_flags);

	jank_hotthread_update_tick(p, now);
	/*
	 * The following statistics are performed only when
	 * delta is greater than at least one time window
	 *
	 * Note:
	 *   When CONFIG_NO_HZ is enabled, delta may be
	 *   greater than JANK_WIN_SIZE_IN_NS
	 */
	if (delta < JANK_WIN_SIZE_IN_NS)
		return;


	kcs = &kcpustat_cpu(cpu);
	runtime  = kcs->cpustat[CPUTIME_USER] -
				cpustat_bak[cpu][CPUTIME_USER];			/* in ns */
	runtime += kcs->cpustat[CPUTIME_NICE] -
				cpustat_bak[cpu][CPUTIME_NICE];
	runtime += kcs->cpustat[CPUTIME_SYSTEM] -
				cpustat_bak[cpu][CPUTIME_SYSTEM];
	runtime += kcs->cpustat[CPUTIME_IRQ] -
				cpustat_bak[cpu][CPUTIME_IRQ];
	runtime += kcs->cpustat[CPUTIME_SOFTIRQ] -
				cpustat_bak[cpu][CPUTIME_SOFTIRQ];
	runtime += kcs->cpustat[CPUTIME_STEAL] -
				cpustat_bak[cpu][CPUTIME_STEAL];

	total_time = runtime + kcs->cpustat[CPUTIME_IDLE] +
				kcs->cpustat[CPUTIME_IOWAIT];

	cpustat_bak[cpu][CPUTIME_USER] = kcs->cpustat[CPUTIME_USER];
	cpustat_bak[cpu][CPUTIME_NICE] = kcs->cpustat[CPUTIME_NICE];
	cpustat_bak[cpu][CPUTIME_SYSTEM] = kcs->cpustat[CPUTIME_SYSTEM];
	cpustat_bak[cpu][CPUTIME_IRQ] = kcs->cpustat[CPUTIME_IRQ];
	cpustat_bak[cpu][CPUTIME_SOFTIRQ] = kcs->cpustat[CPUTIME_SOFTIRQ];
	cpustat_bak[cpu][CPUTIME_STEAL] = kcs->cpustat[CPUTIME_STEAL];
	cpustat_bak[cpu][CPUTIME_GUEST] = kcs->cpustat[CPUTIME_GUEST];
	cpustat_bak[cpu][CPUTIME_GUEST_NICE] =
					kcs->cpustat[CPUTIME_GUEST_NICE];
	/*
	 * Note:
	 * CPUTIME_IRQ and CPUTIME_SOFTIRQ were driven by sched_clock,
	 * while CPUTIME_USER and CPUTIME_NICE were driven by tick, they
	 * have different precision, so it is possible to have runtime
	 * greater than delta
	 */
	runtime = min_t(u64, delta, runtime);
	if (!runtime)
		return;

	total_time = min_t(u64, delta, total_time);

	now_idx = time2winidx(now);

	split_window(now, delta, &delta_a, &delta_cnt, &delta_b);
	split_window(now, runtime, &runtime_a, &runtime_cnt, &runtime_b);
	if (!delta_cnt && last_idx == now_idx) {
		/* In the same time window */
		cputime[cpu].running_time[now_idx] += runtime_b;
	} else if (!delta_cnt &&
			((last_idx+1) & JANK_WIN_CNT_MASK) == now_idx) {
				/* Spanning two time Windows */
		cputime[cpu].running_time[now_idx] = runtime_b;

	} else {
				/* Spanning multiple time Windows */
		for (i = 0; i < min_t(u64, delta_cnt, JANK_WIN_CNT); i++) {
			/* clear new windows */
			idx = (last_idx + 1 + i + JANK_WIN_CNT) & JANK_WIN_CNT_MASK;
			cputime[cpu].running_time[idx] = 0;

		}

		cputime[cpu].running_time[now_idx] = runtime_b;


		for (i = 0; i < min_t(u64, runtime_cnt, (JANK_WIN_CNT-1)); i++) {
			idx = (now_idx - 1 - i + JANK_WIN_CNT) & JANK_WIN_CNT_MASK;

			cputime[cpu].running_time[idx] = JANK_WIN_SIZE_IN_NS;

		}

		if (runtime_cnt < JANK_WIN_CNT-1 && runtime_a) {
			idx = (now_idx + JANK_WIN_CNT - 1 -
				min_t(u64, runtime_cnt, JANK_WIN_CNT)) & JANK_WIN_CNT_MASK;
			cputime[cpu].running_time[idx] += runtime_a;
		}

	}
	/* update */
	cputime[cpu].last_index = now_idx;
	cputime[cpu].last_update_time = now;
}

u64 get_kcpustat_increase(u32 cpu)
{
	u64 runtime;
	struct kernel_cpustat *kcs = NULL;

	kcs = &kcpustat_cpu(cpu);
	runtime  = kcs->cpustat[CPUTIME_USER] -
		cpustat_bak[cpu][CPUTIME_USER];		/* in ns */
	runtime += kcs->cpustat[CPUTIME_NICE] -
		cpustat_bak[cpu][CPUTIME_NICE];
	runtime += kcs->cpustat[CPUTIME_SYSTEM] -
		cpustat_bak[cpu][CPUTIME_SYSTEM];
	runtime += kcs->cpustat[CPUTIME_IRQ] -
		cpustat_bak[cpu][CPUTIME_IRQ];
	runtime += kcs->cpustat[CPUTIME_SOFTIRQ] -
		cpustat_bak[cpu][CPUTIME_SOFTIRQ];
	runtime += kcs->cpustat[CPUTIME_STEAL] -
		cpustat_bak[cpu][CPUTIME_STEAL];

	return runtime;
}

u32 get_cpu_load32_scale(u32 win_cnt, struct cpumask *mask)
{
	/* MediaTek does not apply the Qualcomm WALT execution scaling. */
	return osi_compat_cpu_load(win_cnt, mask);
}

u32 get_cpu_load32(u32 win_cnt, struct cpumask *mask)
{
	return osi_compat_cpu_load(win_cnt, mask);
}

u32 get_cpu_load(u32 win_cnt, struct cpumask *mask)
{
	u32 i, j;
	u32 now_index, last_index, last_index_r, idx;
	u32 cpu_nr;
	u64 now, now_a, now_b;
	u64 delta, delta_a, delta_cnt, delta_b;
	u64 last_update_time, last_runtime, last_runtime_r;
	u64 last_win_delta, last_win_delta_r;
	u64 increase, increase_a, increase_cnt, increase_b;
	u64 tmp_time = 0, run_time = 0, total_time = 0;
	u32 ret;

	if (!win_cnt || !mask)
		return 0;

	cpu_nr = cpumask_weight(mask);
	if (!cpu_nr)
		return 0;

	now = ktime_get_ns();
	now_index = time2winidx(now);

	if (win_cnt == 1) {
		now_b = now & JANK_WIN_SIZE_IN_NS_MASK;
		now_a = JANK_WIN_SIZE_IN_NS - now_b;

		for_each_cpu(i, mask) {
			last_update_time = cputime[i].last_update_time;
			last_win_delta = last_update_time & JANK_WIN_SIZE_IN_NS_MASK;
			last_win_delta_r = JANK_WIN_SIZE_IN_NS - last_win_delta;

			last_index = cputime[i].last_index;
			last_runtime = cputime[i].running_time[last_index];

			last_index_r = (last_index + JANK_WIN_CNT - 1) &
							JANK_WIN_CNT_MASK;
			last_runtime_r = cputime[i].running_time[last_index_r];

			delta = (now > last_update_time) ? now - last_update_time : 0;
			split_window(now, delta, &delta_a, &delta_cnt, &delta_b);


			increase = get_kcpustat_increase(i);
			increase = min_t(u64, delta, increase);
			split_window(now, increase,
						&increase_a, &increase_cnt, &increase_b);

			if (!delta_cnt && last_index == now_index) {
				tmp_time = (last_runtime + increase);
				tmp_time += (last_runtime_r * now_a) >>
							JANK_WIN_SIZE_SHIFT_IN_NS;
			} else if (!delta_cnt &&
					((last_index+1) & JANK_WIN_CNT_MASK) == now_index) {
				tmp_time = increase_b;
				tmp_time += ((last_runtime + increase_a) * now_a) >>
							JANK_WIN_SIZE_SHIFT_IN_NS;
			} else {
				tmp_time = increase_b;
				tmp_time += (JANK_WIN_SIZE_IN_NS * now_a) >>
							JANK_WIN_SIZE_SHIFT_IN_NS;
			}

			jank_dbg("r1:cpu%d, tmp_time=%llu, true1=%d, "
				"d=%llu, d_cnt=%d, li=%d, ni=%d, "
				"lr=%llu, true2=%d lr_r=%llu, "
				"ic=%llu, ic_a=%llu, ic_n=%llu, ic_b=%llu, "
				"na=%llu, nb=%llu, "
				"lwd=%llu, lwd_r=%llu, "
				"n=%llu, la=%llu, true3=%d\n",
				i, tmp_time>>20, tmp_time > JANK_WIN_SIZE_IN_NS,
				delta>>20, delta_cnt, last_index, now_index,
				last_runtime>>20, last_runtime > JANK_WIN_SIZE_IN_NS,
				last_runtime_r>>20,
				increase>>20, increase_a>>20,
				increase_cnt, increase_b>>20,
				now_a>>20, now_b>>20,
				last_win_delta>>20, last_win_delta_r>>20,
				now, last_update_time, now < last_update_time);

			tmp_time = min_t(u64, tmp_time, JANK_WIN_SIZE_IN_NS);

			jank_dbg("r2: tmp_time=%llu, true=%d\n",
				tmp_time>>20, tmp_time > JANK_WIN_SIZE_IN_NS);

			run_time += tmp_time;
			total_time += JANK_WIN_SIZE_IN_NS;
		}
	} else {
		for (j = 0; j < win_cnt; j++) {
			for_each_cpu(i, mask) {
				last_update_time = cputime[i].last_update_time;
				last_win_delta =
					last_update_time & JANK_WIN_SIZE_IN_NS_MASK;
				last_index = cputime[i].last_index;
				last_runtime = cputime[i].running_time[last_index];

				delta = (now > last_update_time) ?
							now - last_update_time : 0;
				split_window(now, delta, &delta_a, &delta_cnt, &delta_b);

				if (!delta_cnt && last_index == now_index) {
					if (j == 0) {
						run_time += last_runtime;
						total_time += last_win_delta;
					} else {
						idx = (last_index + JANK_WIN_CNT - j) &
									JANK_WIN_CNT_MASK;
						run_time += cputime[i].running_time[idx];
						total_time += JANK_WIN_SIZE_IN_NS;
					}
				} else if (delta_cnt < JANK_WIN_CNT-1) {
					if (j <= delta_cnt) {
						run_time += 0;
						total_time += (j == 0) ?
							last_win_delta : JANK_WIN_SIZE_IN_NS;
					} else {
						idx = (now_index + JANK_WIN_CNT - j) &
								JANK_WIN_CNT_MASK;
						if (idx == last_index) {
							run_time += last_runtime;
							total_time += last_win_delta;
						} else {
							run_time += cputime[i].running_time[idx];
							total_time += JANK_WIN_SIZE_IN_NS;
						}
					}
				} else {
					run_time += 0;
					total_time += JANK_WIN_SIZE_IN_NS;
				}
			}
		}
	}

	/* Prevents the error of dividing by zero */
	if (!total_time)
		total_time = JANK_WIN_SIZE_IN_NS * cpu_nr * win_cnt;

	ret = (run_time * 100) / total_time;
	return ret;
}

void jank_group_ratio_show(struct seq_file *m, u32 win_idx, u64 now)
{
}

void jank_busy_ratio_show(struct seq_file *m, u32 win_idx, u64 now)
{
}

static int cpu_info_dump_win(struct seq_file *m, void *v, u32 win_cnt)
{
	u32 i;
	u64 now = ktime_get_ns();

	memcpy(cputime_snap, cputime, sizeof(struct cputime)*CPU_NUMS);

	for (i = 0; i < win_cnt; i++) {
		/* busy/total */
		jank_busy_ratio_show(m, i, now);

		/* fg/busy */
		jank_group_ratio_show(m, i, now);

		/* cpu_frep */

		/* target_freq > policy->max */

		/* target_freq > policy->cur */


		/* hot thread */
		jank_hotthread_show(m, i, now);

		seq_puts(m, "\n");
	}
#ifdef JANK_DEBUG
	/* seq_printf(m, "\n"); */
#endif

	return 0;
}

static int proc_cpu_info_show(struct seq_file *m, void *v)
{
	return cpu_info_dump_win(m, v, JANK_WIN_CNT);
}

static int proc_cpu_info_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, proc_cpu_info_show, inode);
}

static int proc_cpu_info_sig_show(struct seq_file *m, void *v)
{
	return cpu_info_dump_win(m, v, 1);
}

static int proc_cpu_info_sig_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, proc_cpu_info_sig_show, inode);
}

static int proc_cpu_load32_show(struct seq_file *m, void *v)
{
	u32 i;
	struct cpumask mask;

	cpumask_clear(&mask);
	for_each_possible_cpu(i)
		cpumask_set_cpu(i, &mask);

	seq_printf(m, "%d\n", get_cpu_load32(1, &mask));
	return 0;
}

static int proc_cpu_load32_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, proc_cpu_load32_show, inode);
}

static int proc_cpu_load32_scale_show(struct seq_file *m, void *v)
{
	u32 i;
	struct cpumask mask;

	cpumask_clear(&mask);
	for_each_possible_cpu(i)
		cpumask_set_cpu(i, &mask);

	seq_printf(m, "%d\n", get_cpu_load32_scale(1, &mask));
	return 0;
}

static int proc_cpu_load32_scale_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, proc_cpu_load32_scale_show, inode);
}

static int proc_cpu_load_show(struct seq_file *m, void *v)
{
	u32 i;
	struct cpumask mask;

	cpumask_clear(&mask);
	for_each_possible_cpu(i)
		cpumask_set_cpu(i, &mask);

	seq_printf(m, "%d\n", get_cpu_load(1, &mask));
	return 0;
}

static int proc_cpu_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_cpu_load_show, inode);
}

static const struct proc_ops proc_cpu_info_operations = {
	.proc_open	=	proc_cpu_info_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release =	single_release,
};

static const struct proc_ops proc_cpu_info_sig_operations = {
	.proc_open	=	proc_cpu_info_sig_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release =	single_release,
};

static const struct proc_ops proc_cpu_load_operations = {
	.proc_open	=	proc_cpu_load_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release =	single_release,
};

static const struct proc_ops proc_cpu_load32_operations = {
	.proc_open	=	proc_cpu_load32_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release =	single_release,
};

static const struct proc_ops proc_cpu_load32_scale_operations = {
	.proc_open	=	proc_cpu_load32_scale_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release =	single_release,
};

struct proc_dir_entry *jank_cpuload_proc_init(
			struct proc_dir_entry *pde)
{
	struct proc_dir_entry *entry = NULL;

	entry = proc_create("cpu_info", S_IRUGO,
				pde, &proc_cpu_info_operations);
	if (!entry) {
		pr_err("create cpu_info fail\n");
		goto err_cpu_info;
	}

	entry = proc_create("cpu_info_sig", S_IRUGO,
				pde, &proc_cpu_info_sig_operations);
	if (!entry) {
		pr_err("create cpu_info_sig fail\n");
		goto err_cpu_info_sig;
	}

	entry = proc_create("cpu_load", S_IRUGO, pde,
				&proc_cpu_load_operations);
	if (!entry) {
		pr_err("create cpu_load fail\n");
		goto err_cpu_load;
	}

	entry = proc_create("cpu_load32", S_IRUGO, pde,
				&proc_cpu_load32_operations);
	if (!entry) {
		pr_err("create cpu_load32 fail\n");
		goto err_cpu_load32;
	}

	entry = proc_create("cpu_load32_scale", S_IRUGO, pde,
				&proc_cpu_load32_scale_operations);
	if (!entry) {
		pr_err("create cpu_load32_scale fail\n");
		goto err_cpu_load32_scale;
	}
	return entry;

err_cpu_load32_scale:
	remove_proc_entry("cpu_load32", pde);

err_cpu_load32:
	remove_proc_entry("cpu_load", pde);

err_cpu_load:
	remove_proc_entry("cpu_info_sig", pde);

err_cpu_info_sig:
	remove_proc_entry("cpu_info", pde);

err_cpu_info:
	return NULL;
}

void jank_cpuload_proc_deinit(struct proc_dir_entry *pde)
{
	remove_proc_entry("cpu_load32_scale", pde);
	remove_proc_entry("cpu_load32", pde);
	remove_proc_entry("cpu_load", pde);
	remove_proc_entry("cpu_info_sig", pde);
	remove_proc_entry("cpu_info", pde);
}

void jank_cpuload_init(void)
{
	struct kernel_cpustat *kcs = NULL;
	u32 cpu;

	memset(cputime, 0, sizeof(struct cputime)*CPU_NUMS);
	memset(cputime_snap, 0, sizeof(struct cputime)*CPU_NUMS);

	for_each_possible_cpu(cpu) {
		kcs = &kcpustat_cpu(cpu);
		cpustat_bak[cpu][CPUTIME_USER] =
				kcs->cpustat[CPUTIME_USER];
		cpustat_bak[cpu][CPUTIME_NICE] =
				kcs->cpustat[CPUTIME_NICE];
		cpustat_bak[cpu][CPUTIME_SYSTEM] =
				kcs->cpustat[CPUTIME_SYSTEM];
		cpustat_bak[cpu][CPUTIME_IRQ] =
				kcs->cpustat[CPUTIME_IRQ];
		cpustat_bak[cpu][CPUTIME_SOFTIRQ] =
				kcs->cpustat[CPUTIME_SOFTIRQ];
		cpustat_bak[cpu][CPUTIME_STEAL] =
				kcs->cpustat[CPUTIME_STEAL];
		cpustat_bak[cpu][CPUTIME_GUEST] =
				kcs->cpustat[CPUTIME_GUEST];
		cpustat_bak[cpu][CPUTIME_GUEST_NICE] =
				kcs->cpustat[CPUTIME_GUEST_NICE];
	}
}

