// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/kernel_stat.h>
#include <linux/cpumask.h>
#include <linux/sched/cputime.h>
#include <linux/tick.h>
#include <asm/uaccess.h>
#include <linux/version.h>
#include <trace/hooks/dtask.h>
#include <trace/hooks/sched.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sort.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "osi_base.h"

#define MIN_WINDOW_TIME		1000000000L
#define MAX_WINDOW_TIME		(10 * 60 * MIN_WINDOW_TIME)

#define PDE_DATA pde_data

#define   TOP_PROCESS_USAGE    5
#define   MAX_PROCESS_USAGE    4096
#define pid_hashtask(nr, ns)	\
	hash_long((unsigned long)nr + (unsigned long)ns, pidhash_shift)
static struct hlist_head *task_struct_hash;
static unsigned int pidhash_shift = 14;
/* Cache for task_struct_data */
static struct kmem_cache *task_struct_data_cache;
struct task_struct_data {
	u64 sum_exec;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	int hit;
	struct hlist_node pid_chain;
	struct rcu_head rcu;
};
static struct task_struct_data task_info_top[MAX_PROCESS_USAGE];
static DEFINE_SPINLOCK(taskhash_lock);

#ifdef arch_idle_time
static u64 cpu_idle_time(int cpu)
{
	u64 idle;

	idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
	if (cpu_online(cpu) && !nr_iowait_cpu(cpu))
		idle += arch_idle_time(cpu);
	return idle;
}

static u64 cpu_iowait_time(int cpu)
{
	u64 iowait;

	iowait = kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
	if (cpu_online(cpu) && nr_iowait_cpu(cpu))
		iowait += arch_idle_time(cpu);
	return iowait;
}
#else
static u64 cpu_idle_time(int cpu)
{
	u64 idle, idle_usecs = -1ULL;

	if (cpu_online(cpu))
		idle_usecs = get_cpu_idle_time_us(cpu, NULL);

	if (idle_usecs == -1ULL)
		/* !NO_HZ or cpu offline so we can rely on cpustat.idle */
		idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
	else
		idle = idle_usecs * NSEC_PER_USEC;

	return idle;
}

static u64 cpu_iowait_time(int cpu)
{
	u64 iowait, iowait_usecs = -1ULL;

	if (cpu_online(cpu))
		iowait_usecs = get_cpu_iowait_time_us(cpu, NULL);

	if (iowait_usecs == -1ULL)
		/* !NO_HZ or cpu offline so we can rely on cpustat.iowait */
		iowait = kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
	else
		iowait = iowait_usecs * NSEC_PER_USEC;

	return iowait;
}
#endif

void free_data_rcu(struct rcu_head *rcu)
{
	struct task_struct_data *data = container_of(rcu, struct task_struct_data, rcu);
	if (data)
		kmem_cache_free(task_struct_data_cache, data);
}

static void init_task_struct_data(void *ptr)
{
	struct task_struct_data *tsd = ptr;

	memset(tsd, 0, sizeof(struct task_struct_data));
	INIT_HLIST_NODE(&tsd->pid_chain);
}

int oplus_task_struct_hash_init(void)
{
	int size = sizeof(*task_struct_hash) * (1 << pidhash_shift);

	task_struct_hash = (struct hlist_head *)kzalloc(size, GFP_KERNEL);
	if (!task_struct_hash) {
		pr_err("task_struct_hash [kzalloc_debug] %s alloc failed!\n", __func__);
		return -ENOMEM;
	}

	/* Create kmem_cache for task_struct_data */
	task_struct_data_cache = kmem_cache_create(
		"task_struct_data_cache",
		sizeof(struct task_struct_data),
		0,
		SLAB_PANIC | SLAB_ACCOUNT,
		init_task_struct_data);

	if (!task_struct_data_cache) {
		kfree(task_struct_hash);
		pr_err("task_struct_data_cache creation failed!\n");
		return -ENOMEM;
	}
	pr_info("task_struct_hash oplus_task_struct_hash_init succeed!\n");
	return 0;
}

void oplus_task_struct_hash_deinit(void)
{
	kmem_cache_destroy(task_struct_data_cache);
	kfree(task_struct_hash);
	pr_info("task_struct_hash oplus_task_struct_hash_deinit succeed!\n");
}

void oplus_task_struct_add_hash(pid_t pid, struct task_struct_data *data, struct task_struct *tsk)
{
	spin_lock_irq(&taskhash_lock);
	hlist_add_head_rcu(&data->pid_chain,
		&task_struct_hash[pid_hashtask(pid, pid)]);
	spin_unlock_irq(&taskhash_lock);
}

struct task_struct_data *oplus_task_struct_search_hash(pid_t pid, struct task_struct *tsk)
{
	struct task_struct_data *data;

	rcu_read_lock();
	hlist_for_each_entry_rcu(data,
		&task_struct_hash[pid_hashtask(pid, pid)], pid_chain) {
		if (!data) {
			rcu_read_unlock();
			return NULL;
		}

		if (data->tgid == pid) {
			rcu_read_unlock();
			return data;
		}
	}
	rcu_read_unlock();
	return NULL;
}

static u64 cpustat_time(struct kernel_cpustat *now, struct kernel_cpustat *last)
{
	int i;
	u64 total_time = 0;
	int possible_cpus_num = num_possible_cpus();

	for (i = CPUTIME_USER; i < NR_STATS; i++)
		total_time += now->cpustat[i] - last->cpustat[i];
	if (possible_cpus_num == 0)
		possible_cpus_num = 1;
	return total_time / possible_cpus_num;
}

static void get_cpustat(struct kernel_cpustat *stat)
{
	int i, cpu;

	memset(stat, 0, sizeof(struct kernel_cpustat));
	for_each_possible_cpu(cpu) {
		for (i = 0; i < NR_STATS; i++) {
			if (i == CPUTIME_IDLE)
				stat->cpustat[CPUTIME_IDLE] += cpu_idle_time(cpu);
			else if (i == CPUTIME_IOWAIT)
				stat->cpustat[CPUTIME_IOWAIT] += cpu_iowait_time(cpu);
			else
				stat->cpustat[i] += kcpustat_cpu(cpu).cpustat[i];
		}
	}
}

static int cmp_tasks(const void *a, const void *b)
{
	const struct task_struct_data *s1 = a;
	const struct task_struct_data *s2 = b;

	if (s1->sum_exec > s2->sum_exec)
		return -1;

	if (s1->sum_exec < s2->sum_exec)
		return 1;

	return 0;
}

static void android_vh_free_task_handler(void *unused, struct task_struct *tsk)
{
	struct task_struct_data *data = NULL;

	if (tsk->pid != tsk->tgid)
		return;

	spin_lock_irq(&taskhash_lock);
	data = oplus_task_struct_search_hash(tsk->tgid, tsk);
	if (data) {
		hlist_del_rcu(&data->pid_chain);
		call_rcu(&data->rcu, free_data_rcu);
	}
	spin_unlock_irq(&taskhash_lock);
}

static void osi_task_rename_handler(void *unused, struct task_struct *tsk, const char *buf)
{
	char comm[128];
	struct task_struct_data *task_struct_p = NULL;

	/*main thread set task_comm in exec after fork*/
	if (tsk->tgid != tsk->pid)
		return;

	task_struct_p = (struct task_struct_data *)kmem_cache_alloc(task_struct_data_cache, GFP_ATOMIC);
	if (!task_struct_p)
		return;

	strncpy(comm, buf, sizeof(comm));
	memcpy(task_struct_p->comm, comm, TASK_COMM_LEN);
	task_struct_p->tgid = tsk->tgid;
	task_struct_p->sum_exec = 0;
	task_struct_p->hit = 1;
	oplus_task_struct_add_hash(tsk->tgid, task_struct_p, tsk);
}

static void osi_sched_stat_runtime_handler(void *unused,
			struct task_struct *tsk, u64 runtime, u64 vruntime)
{
	struct task_struct_data *task_struct_p = NULL;

	if (tsk == NULL)
		return;

	if (tsk->pid <= 0 || tsk->tgid <= 0)
		return;

	task_struct_p = oplus_task_struct_search_hash(tsk->tgid, tsk);

	if (task_struct_p && tsk->tgid == task_struct_p->tgid) {
		task_struct_p->sum_exec += runtime;
		task_struct_p->hit = 1;
	}
}

static int procs_cpu_usage_show(struct seq_file *m, void *v)
{
	int rank_num = 0;
	int rank_curr = 0;
	ktime_t start_time, end_time, end_time_loop, elapsed_time_loop;
	static ktime_t last_win_start;
	u64 elapsed_time, window_time;
	u64 total_cputime;
	struct kernel_cpustat cpustat;
	static struct kernel_cpustat last_cpustat;
	struct task_struct_data *data;
	int bucket_count = 1 << pidhash_shift;

	start_time = ktime_get();
	window_time = ktime_sub(start_time, last_win_start);
	if (window_time < MIN_WINDOW_TIME) {
		pr_warn("window time too short: %lld ms.", ktime_to_ms(window_time));
		return -EAGAIN;
	}

	last_win_start = start_time;
	get_cpustat(&cpustat);
	total_cputime = cpustat_time(&cpustat, &last_cpustat);
	/*update delta_sum_exec per entry, and rank cpu usage*/
	spin_lock_irq(&taskhash_lock);
	for (int count = 0; count < bucket_count; count++) {
		hlist_for_each_entry_rcu(data, &task_struct_hash[count], pid_chain) {
			if (!data) {
				spin_unlock_irq(&taskhash_lock);
				return -EFAULT;
			}
			if (rank_num < MAX_PROCESS_USAGE) {
				if (data->hit == 1) {
					memcpy(&task_info_top[rank_num], data, sizeof(struct task_struct_data));
					data->hit = 0;
					data->sum_exec = 0;
					rank_num++;
				} else {
					hlist_del_rcu(&data->pid_chain);
					call_rcu(&data->rcu, free_data_rcu);
				}
			} else {
				if (data->hit == 1) {
					data->hit = 0;
					data->sum_exec = 0;
				} else {
					hlist_del_rcu(&data->pid_chain);
					call_rcu(&data->rcu, free_data_rcu);
				}
			}
		}
	}
	spin_unlock_irq(&taskhash_lock);
	end_time_loop = ktime_get();
	sort(task_info_top, rank_num, sizeof(struct task_struct_data), cmp_tasks, NULL);

	rank_curr = rank_num < TOP_PROCESS_USAGE ? rank_num : TOP_PROCESS_USAGE;
	for (int i = 0; i < rank_curr; i++) {
		seq_printf(m, "%d %lld %s\n", task_info_top[i].tgid,
			(task_info_top[i].sum_exec * 100ULL) / total_cputime, task_info_top[i].comm);
	}

	last_cpustat = cpustat;

	/* update stats cycle time */
	end_time = ktime_get();
	elapsed_time = ktime_to_us(ktime_sub(end_time, start_time));
	elapsed_time_loop = ktime_to_us(ktime_sub(end_time_loop, start_time));
	pr_info("procs_cpu_usage_show rank_num:%d spent time: %lld us, loop_time:%lld window time: %lld ms\n",
		rank_num, elapsed_time, elapsed_time_loop, window_time / 1000 / 1000);

	return 0;
}

static int procs_cpu_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, procs_cpu_usage_show, PDE_DATA(inode));
}

static struct proc_ops procs_cpu_usage_fops = {
	.proc_open		= procs_cpu_usage_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release   = single_release,
};

int  osi_procs_cpu_usage_init(void)
{
	int ret = 0;
	struct proc_dir_entry *p_parent;


	p_parent = proc_mkdir("oplus_power", NULL);
	if (!p_parent) {
		pr_err("%s: failed to create oplus_power directory.\n", __func__);
		goto err;
	}
	proc_create("top_process", 0444, p_parent, &procs_cpu_usage_fops);
	ret = oplus_task_struct_hash_init();
	if (ret != 0)
		return ret;

	REGISTER_TRACE_VH(sched_stat_runtime, osi_sched_stat_runtime_handler);
	REGISTER_TRACE_VH(task_rename, osi_task_rename_handler);
	REGISTER_TRACE_VH(android_vh_free_task, android_vh_free_task_handler);
	/* REGISTER_TRACE_VH(android_vh_exit_check, osi_exit_check_handler); */
	return 0;

err:
	return -ENOMEM;
}
