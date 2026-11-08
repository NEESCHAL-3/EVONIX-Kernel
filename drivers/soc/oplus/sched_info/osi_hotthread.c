// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/plist.h>
#include <linux/sort.h>
#include <linux/completion.h>
#include <uapi/linux/sched/types.h>
#include <trace/hooks/sched.h>

#include "osi_hotthread.h"
#include "osi_topology.h"
#include "osi_tasktrack.h"
#include "osi_netlink.h"
#include "../sched_assist/sa_group.h"

struct rq_num {
	int num;
	int avg_num;
};

DEFINE_PER_CPU(struct rq_num, percpu_rq_num);
struct kmem_cache *hot_thread_struct_cachep;

extern unsigned long high_load_switch;
extern int g_over_load;

static struct task_track_cpu task_track[MAX_CLUSTER];
struct hot_thread_struct  hot_thread_top[JANK_WIN_CNT][TOP_THREAD_CNT];

struct hot_thread_node {
	struct plist_node node;
	struct hot_thread_struct hot_thread_struct;
};

static PLIST_HEAD(hot_thread_head);
static DEFINE_RAW_SPINLOCK(hot_thread_lock);
static struct work_struct rqlen_notify_work;

/* Use the kernel priority-list implementation from lib/plist.c. */
int find_in_plist(struct task_struct *p,  struct oplus_task_struct *ots)
{
	struct hot_thread_node *tmp;
	bool is_find = false;

	plist_for_each_entry(tmp, &hot_thread_head, node) {
		if (tmp->hot_thread_struct.pid == p->pid) {
			/*find the same node, just update hot thread info*/
			if (osi_task_in_cpu_group(p, "top-app"))
				tmp->hot_thread_struct.top_app_cnt++;
			else
				tmp->hot_thread_struct.non_topapp_cnt++;
			tmp->hot_thread_struct.total_cnt++;
			is_find = true;
			break;
		}
	}
	if (is_find) {
		plist_del(&tmp->node, &hot_thread_head);
		plist_node_init(&tmp->node, INT_MAX - tmp->hot_thread_struct.total_cnt);
		plist_add(&tmp->node, &hot_thread_head);
		return 0;
	}
	return -ESRCH;
}

static int insert_hot_thread(struct oplus_task_struct *ots, struct task_struct *p, u32 now_idx)
{
	struct hot_thread_node  *hot_thread_node;
	unsigned long flags;
	struct task_struct *leader;
	const struct cred *tcred;
	uid_t uid;

	rcu_read_lock();
	tcred = __task_cred(p);
	if (!tcred) {
		rcu_read_unlock();
		pr_info("cpuload: tcred is NULL!");
		return -1;
	}
	uid = __kuid_val(tcred->uid);
	rcu_read_unlock();
	raw_spin_lock_irqsave(&hot_thread_lock, flags);
	if (find_in_plist(p, ots) == -ESRCH) {
			hot_thread_node = kmem_cache_zalloc(hot_thread_struct_cachep, GFP_ATOMIC);
			if (IS_ERR_OR_NULL(hot_thread_node))
				goto done;
			memcpy(hot_thread_node->hot_thread_struct.comm, p->comm, TASK_COMM_LEN);
			rcu_read_lock();
			if (pid_alive(p)) {
				leader = rcu_dereference(p->group_leader);
				if (pid_alive(leader))
					memcpy(hot_thread_node->hot_thread_struct.leader_comm, leader->comm, TASK_COMM_LEN);
			}
			rcu_read_unlock();
			hot_thread_node->hot_thread_struct.pid = p->pid;
			hot_thread_node->hot_thread_struct.tgid = p->tgid;
			hot_thread_node->hot_thread_struct.uid = uid;
			hot_thread_node->hot_thread_struct.top_app_cnt = 0;
			hot_thread_node->hot_thread_struct.non_topapp_cnt = 0;
			if (osi_task_in_cpu_group(p, "top-app"))
				hot_thread_node->hot_thread_struct.top_app_cnt = 1;
			else
				hot_thread_node->hot_thread_struct.non_topapp_cnt = 1;
			hot_thread_node->hot_thread_struct.total_cnt = 1;
			plist_node_init(&hot_thread_node->node, INT_MAX - hot_thread_node->hot_thread_struct.total_cnt);
			plist_add(&hot_thread_node->node, &hot_thread_head);
	}
done:
	raw_spin_unlock_irqrestore(&hot_thread_lock, flags);
	return 0;
}

static void  get_hot_thread(u32 now_idx, u64 now)
{
	struct hot_thread_node *hot_node, *tmp;
	unsigned long flags;
	int i = 0;

	memset(&hot_thread_top[now_idx][0], 0, TOP_THREAD_CNT * sizeof(struct hot_thread_struct));
	raw_spin_lock_irqsave(&hot_thread_lock, flags);
	plist_for_each_entry_safe(hot_node, tmp, &hot_thread_head, node) {
		if (!hot_node) {
			goto out;
		}
		if (i < TOP_THREAD_CNT) {
			memcpy(&hot_thread_top[now_idx][i], &hot_node->hot_thread_struct,
				sizeof(struct hot_thread_struct));
			i++;
		}
		plist_del(&hot_node->node, &hot_thread_head);
		kmem_cache_free(hot_thread_struct_cachep, hot_node);
	}
out:
	raw_spin_unlock_irqrestore(&hot_thread_lock, flags);
}

static void notify_rqlen_fn(struct work_struct *work)
{
	int rq_len[CPU_NUMS];
	int cpu;
	for_each_possible_cpu(cpu) {
		rq_len[cpu] = per_cpu(percpu_rq_num, cpu).avg_num;
	}
	send_to_user(NOTIFY_CPU_RQ, ARRAY_SIZE(rq_len), (int *)&rq_len);
}

static inline void count_rq_num(int cpu)
{
	struct rq_num *cur_rq;
	cur_rq = &per_cpu(percpu_rq_num, cpu);
	cur_rq->num += cpu_rq(cpu)->nr_running;
}

static void cal_rq_num(void)
{
	struct rq_num *cur_rq;
	int cpu;
	for_each_possible_cpu(cpu) {
		cur_rq = &per_cpu(percpu_rq_num, cpu);
		cur_rq->avg_num = cur_rq->num / TICK_PER_WIN;
		cur_rq->num = 0;
	}
}


/* updated in each tick */
void jank_hotthread_update_tick(struct task_struct *p, u64 now)
{
	struct oplus_task_struct *ots;
	u64 timestamp, timestamp_prewin;
	u32 now_idx;
	u32 cpu, cluster_id;
	static int cal_rq_cnt;

	if (unlikely(!p))
		return;
	ots = get_oplus_task_struct(p);
	cpu = task_cpu(p);
	cluster_id = get_cluster_id(cpu);


	now_idx = time2winidx(now);
	if (unlikely(g_over_load)) {
		insert_hot_thread(ots, p, now_idx);
		count_rq_num(cpu);
	}
	timestamp = task_track[cluster_id].track[now_idx].timestamp;
	timestamp_prewin = hot_thread_top[now_idx][0].timestamp;
	if (!is_same_idx(timestamp_prewin, now)) {
		if (unlikely(g_over_load)) {
			get_hot_thread(now_idx, now);
			cal_rq_num();
			if (cal_rq_cnt++ >= TICK_PER_WIN) {
				queue_work(system_wq, &rqlen_notify_work);
				cal_rq_cnt = 0;
			}
		}
		hot_thread_top[now_idx][0].timestamp = now;
	}
}

static void init_hot_thread_struct(void *ptr)
{
	struct hot_thread_node *hot_thread_node = ptr;
	memset(hot_thread_node, 0, sizeof(struct hot_thread_node));
}

void hotthread_show(struct seq_file *m, u32 win_idx, u64 now)
{
	u32 i, now_index, idx;
	bool nospace = false;
	struct hot_thread_struct  *tmp_track;
	u64 timestamp;
	pid_t  pid, tgid;
	uid_t uid;

	now_index =  time2winidx(now);
	for (i = 0; i < TOP_THREAD_CNT; i++) {
		nospace = (i == TOP_THREAD_CNT-1) ? true : false;
		idx = winidx_sub(now_index, win_idx);
		tmp_track = &hot_thread_top[idx][i];
		pid = tmp_track->pid;
		tgid = tmp_track->tgid;
		uid = tmp_track->uid;
		timestamp = tmp_track->timestamp;
		if (tmp_track->total_cnt) {
			seq_printf(m, "%d$%d$%s$%d$%s$%d$%d%s", uid, tgid, tmp_track->leader_comm,
			pid, tmp_track->comm, tmp_track->top_app_cnt, tmp_track->non_topapp_cnt,
			nospace ? "" : "  ");
		}
	}
}

void jank_hotthread_show(struct seq_file *m, u32 win_idx, u64 now)
{
}

static int  top_hotthread_dump_win(struct seq_file *m, void *v, u32 win_cnt)
{
	u32 i;
	u64 now = ktime_get_ns();

	for (i = 0; i < win_cnt; i++) {
		hotthread_show(m, i, now);
		seq_puts(m, "\n");
	}
	return 0;
}

static int proc_top_hotthread_show(struct seq_file *m, void *v)
{
	return top_hotthread_dump_win(m, v, JANK_WIN_CNT/2);
}
static int proc_top_hotthread_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, proc_top_hotthread_show, inode);
}

static const struct proc_ops proc_top_hotthread_info_operations = {
	.proc_open	=	proc_top_hotthread_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release   =	single_release,
};


int osi_hotthread_proc_init(struct proc_dir_entry *pde)
{
	hot_thread_struct_cachep = kmem_cache_create("hot_thread_node",
			sizeof(struct hot_thread_node), 0,
			SLAB_ACCOUNT, init_hot_thread_struct);
	if (!hot_thread_struct_cachep)
		return -ENOMEM;
	INIT_WORK(&rqlen_notify_work, notify_rqlen_fn);
	if (!proc_create("top_hotthread", 0444, pde,
			 &proc_top_hotthread_info_operations)) {
		kmem_cache_destroy(hot_thread_struct_cachep);
		hot_thread_struct_cachep = NULL;
		return -ENOMEM;
	}
	return 0;
}

void osi_hotthread_proc_deinit(struct proc_dir_entry *pde)
{
	struct hot_thread_node *node, *next;
	unsigned long flags;

	if (pde)
		remove_proc_entry("top_hotthread", pde);
	cancel_work_sync(&rqlen_notify_work);
	raw_spin_lock_irqsave(&hot_thread_lock, flags);
	plist_for_each_entry_safe(node, next, &hot_thread_head, node) {
		plist_del(&node->node, &hot_thread_head);
		kmem_cache_free(hot_thread_struct_cachep, node);
	}
	raw_spin_unlock_irqrestore(&hot_thread_lock, flags);
	kmem_cache_destroy(hot_thread_struct_cachep);
	hot_thread_struct_cachep = NULL;
}

