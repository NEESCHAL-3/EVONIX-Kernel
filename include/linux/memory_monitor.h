/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MEMORY_MONITOR_H__
#define __MEMORY_MONITOR_H__

#include <linux/sched.h>
#include <linux/errno.h>

struct process_mem {
	char comm[TASK_COMM_LEN];
	int pid;
	int ppid;
	int oom_score_adj;
	unsigned long rss;
	unsigned long rssfile;
	unsigned long swapents_ori;
	unsigned int uid;
	unsigned long gl_dev;
	unsigned long egl;
};

#ifdef CONFIG_DUMP_TASKS_MEM
int mm_get_all_pmem_bounded(int *cnt, struct process_mem *val, unsigned int capacity);
void mm_get_all_pmem(int *cnt, struct process_mem *val);
#else
static inline int mm_get_all_pmem_bounded(int *cnt, struct process_mem *val,
					  unsigned int capacity)
{
	if (cnt)
		*cnt = 0;
	return -EOPNOTSUPP;
}

static inline void mm_get_all_pmem(int *cnt, struct process_mem *val)
{
	if (cnt)
		*cnt = 0;
}
#endif

#endif /* __MEMORY_MONITOR_H__ */
