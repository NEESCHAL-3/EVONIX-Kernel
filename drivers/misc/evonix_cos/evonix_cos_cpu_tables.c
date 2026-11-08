// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/energy_model.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include "evonix_cos_cpu_tables.h"

/* Preserve native Oplus sgeinfo formatting and Energy Model power units. */
static int evx_sgeinfo_show(struct seq_file *m, void *v)
{
	struct cpufreq_policy *policy;
	struct em_perf_domain *domain;
	struct em_perf_table *table;
	unsigned int min_freq, max_freq;
	bool header, any = false;
	int cpu, index;

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		down_read(&policy->rwsem);
		min_freq = policy->cpuinfo.min_freq;
		max_freq = policy->cpuinfo.max_freq;
		rcu_read_lock();
		domain = em_cpu_get(cpu);
		table = domain ? rcu_dereference(domain->em_table) : NULL;
		header = false;
		/* An artificial table is not a physical CPU power estimate. */
		if (table && !em_is_artificial(domain)) {
			for (index = domain->nr_perf_states - 1; index >= 0; index--) {
				const struct em_perf_state *state = &table->state[index];

				if (state->frequency < min_freq || state->frequency > max_freq)
					continue;
				if (!header) {
					seq_printf(m, "cpu %d\n", cpu);
					header = true;
				}
				seq_printf(m, "freq %lu pwr %lu\n", state->frequency, state->power);
				any = true;
			}
		}
		rcu_read_unlock();
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);
	}
	cpus_read_unlock();
	return any ? 0 : -ENODATA;
}

static int evx_sgefreqinfo_show(struct seq_file *m, void *v)
{
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *entry;
	bool header, any = false;
	int cpu;

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		down_read(&policy->rwsem);
		header = false;
		if (policy->freq_table) {
			cpufreq_for_each_valid_entry(entry, policy->freq_table) {
				if (entry->flags & CPUFREQ_BOOST_FREQ)
					continue;
				if (!header) {
					seq_printf(m, "cpu %d\n", cpu);
					header = true;
				}
				seq_printf(m, "%u\n", entry->frequency);
				any = true;
			}
		}
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);
	}
	cpus_read_unlock();
	return any ? 0 : -ENODATA;
}

static int evx_sgeinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_sgeinfo_show, NULL);
}

static int evx_sgefreqinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_sgefreqinfo_show, NULL);
}

const struct proc_ops evx_sgeinfo_proc_ops = {
	.proc_open = evx_sgeinfo_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

const struct proc_ops evx_sgefreqinfo_proc_ops = {
	.proc_open = evx_sgefreqinfo_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
