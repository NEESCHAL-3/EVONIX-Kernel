// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2022 Oplus. All rights reserved. */
#include <linux/cpufreq.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include "osi_base.h"
#include "osi_topology.h"
#include "osi_freq.h"

struct osi_policy_history {
	raw_spinlock_t lock;
	unsigned int count;
	unsigned int current_index;
	u32 *frequencies;
	u64 *longest_ns;
	u64 since_ns;
};
static struct osi_policy_history policy_history[CPU_NUMS];
static bool frequency_ready;

/* Observe successful frequency transitions, not pre-switch requests. */
void osi_cpufreq_transition_handler(unsigned int frequency, unsigned int cpu)
{
	struct osi_policy_history *history;
	unsigned int leader, index;
	unsigned long flags;
	u64 now;

	if (!smp_load_acquire(&frequency_ready) || cpu >= CPU_NUMS)
		return;
	leader = get_start_cpu(cpu);
	if (leader >= CPU_NUMS)
		return;
	history = &policy_history[leader];
	raw_spin_lock_irqsave(&history->lock, flags);
	now = ktime_get_ns();
	for (index = 0; index < history->count; index++)
		if (history->frequencies[index] == frequency)
			break;
	if (index == history->current_index) {
		raw_spin_unlock_irqrestore(&history->lock, flags);
		return;
	}
	if (history->current_index < history->count)
		history->longest_ns[history->current_index] = max(
			history->longest_ns[history->current_index], now - history->since_ns);
	history->current_index = index;
	history->since_ns = now;
	raw_spin_unlock_irqrestore(&history->lock, flags);
}

static inline int cpufreq_table_find_index(struct cpufreq_policy *policy,
					     unsigned int target_freq)
{
	if (policy->freq_table_sorted == CPUFREQ_TABLE_SORTED_ASCENDING)
		return cpufreq_table_find_index_al(policy, target_freq, 1);
	else
		return cpufreq_table_find_index_dl(policy, target_freq, 1);
}

void get_cpufreq_info(bool *is_sample)
{
	int cls;
	int start_cpu;
	int cur_idx, max_idx, min_idx, orig_max_id, orig_min_id;
	struct cpufreq_policy *pol;

	for (cls = 0; cls < cluster_num; cls++) {
		start_cpu = cluster[cls].start_cpu;
		pol = cpufreq_cpu_get(start_cpu);
		if (likely(pol)) {
			down_read(&pol->rwsem);
			if (!pol->freq_table) {
				up_read(&pol->rwsem);
				cpufreq_cpu_put(pol);
				continue;
			}
			cur_idx = cpufreq_table_find_index(pol, pol->cur);
			min_idx = cpufreq_table_find_index(pol, pol->min);
			max_idx = cpufreq_table_find_index(pol, pol->max);
			orig_max_id = cpufreq_table_find_index(pol, pol->cpuinfo.max_freq);
			orig_min_id = cpufreq_table_find_index(pol, pol->cpuinfo.min_freq);
			osi_debug("cpu:%d,pol->cur:%d,idx:%d min_idx:%d,max_idx:%d,orig_min_idx:%d,orig_max_idx:%d",
				start_cpu, pol->cur, cur_idx, min_idx, max_idx, orig_min_id, orig_max_id);
			if (pol->freq_table_sorted == CPUFREQ_TABLE_SORTED_DESCENDING) {
				if ((max_idx <= orig_min_id - 4) && (cur_idx <= max_idx + 2))
					*is_sample = true;
			} else {
				if ((max_idx >= orig_min_id + 4) && (cur_idx >= max_idx - 2))
					*is_sample = true;
			}
			up_read(&pol->rwsem);
			cpufreq_cpu_put(pol);
		}
	}
}

static void osi_release_frequency_tables(void)
{
	unsigned int cpu;

	WRITE_ONCE(frequency_ready, false);
	for (cpu = 0; cpu < CPU_NUMS; cpu++) {
		kfree(policy_history[cpu].frequencies);
		kfree(policy_history[cpu].longest_ns);
		policy_history[cpu].frequencies = NULL;
		policy_history[cpu].longest_ns = NULL;
		policy_history[cpu].count = 0;
	}
}

static int proc_long_freq_duration_show(struct seq_file *m, void *v)
{
	struct osi_policy_history *history;
	unsigned int cluster_index, cpu, index;
	unsigned long flags;
	u64 *snapshot, now, duration;
	bool reset = false;
	unsigned long im_flag;

	if (!smp_load_acquire(&frequency_ready))
		return -EAGAIN;
	im_flag = oplus_get_im_flag(current->group_leader);
	reset = test_bit(IM_FLAG_MIDASD, &im_flag);
	for (cluster_index = 0; cluster_index < cluster_num; cluster_index++) {
		cpu = cluster[cluster_index].start_cpu;
		history = &policy_history[cpu];
		snapshot = kcalloc(history->count, sizeof(*snapshot), GFP_KERNEL);
		if (!snapshot)
			return -ENOMEM;
		raw_spin_lock_irqsave(&history->lock, flags);
		now = ktime_get_ns();
		for (index = 0; index < history->count; index++) {
			duration = history->longest_ns[index];
			if (index == history->current_index)
				duration = max(duration, now - history->since_ns);
			snapshot[index] = div_u64(duration, NSEC_PER_MSEC);
		}
		if (reset) {
			memset(history->longest_ns, 0,
				array_size(history->count, sizeof(u64)));
			history->since_ns = now;
		}
		raw_spin_unlock_irqrestore(&history->lock, flags);
		seq_printf(m, "cpu(%d), %7s, %10s\n", cpu, "opp", "duration");
		for (index = 0; index < history->count; index++)
			seq_printf(m, "%6d, %7u, %10llu\n", index,
				history->frequencies[index], snapshot[index]);
		seq_puts(m, "\n");
		kfree(snapshot);
	}
	return 0;
}

static int proc_long_freq_duration_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_long_freq_duration_show, NULL);
}

static const struct proc_ops proc_long_freq_duration_operations = {
	.proc_open = proc_long_freq_duration_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int osi_freq_init(struct proc_dir_entry *pde)
{
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *entry;
	struct osi_policy_history *history;
	unsigned int cluster_index, cpu, count, index;
	int ret = -ENOMEM;

	for (cluster_index = 0; cluster_index < cluster_num; cluster_index++) {
		cpu = cluster[cluster_index].start_cpu;
		history = &policy_history[cpu];
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			ret = -EPROBE_DEFER;
			goto fail;
		}
		down_read(&policy->rwsem);
		if (!policy->freq_table) {
			up_read(&policy->rwsem);
			cpufreq_cpu_put(policy);
			ret = -ENODATA;
			goto fail;
		}
		count = 0;
		cpufreq_for_each_valid_entry(entry, policy->freq_table)
			count++;
		if (!count) {
			up_read(&policy->rwsem);
			cpufreq_cpu_put(policy);
			ret = -ENODATA;
			goto fail;
		}
		history->frequencies = kcalloc(count, sizeof(u32), GFP_KERNEL);
		history->longest_ns = kcalloc(count, sizeof(u64), GFP_KERNEL);
		if (!history->frequencies || !history->longest_ns) {
			up_read(&policy->rwsem);
			cpufreq_cpu_put(policy);
			goto fail;
		}
		raw_spin_lock_init(&history->lock);
		history->count = count;
		history->current_index = count;
		index = 0;
		cpufreq_for_each_valid_entry(entry, policy->freq_table) {
			history->frequencies[index] = entry->frequency;
			if (entry->frequency == policy->cur)
				history->current_index = index;
			index++;
		}
		history->since_ns = ktime_get_ns();
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);
	}
	smp_store_release(&frequency_ready, true);
	if (!proc_create("long_freq_duration", 0444, pde,
			 &proc_long_freq_duration_operations))
		goto fail;
	return 0;
fail:
	osi_release_frequency_tables();
	return ret;
}

void osi_freq_exit(struct proc_dir_entry *pde)
{
	if (pde)
		remove_proc_entry("long_freq_duration", pde);
	osi_release_frequency_tables();
}
