// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>

#include "game_ctrl.h"


/* To handle cpufreq min/max request */
struct cpu_freq_status {
	unsigned int min;
	unsigned int max;
};

/* gpa: game performance adaptive, from user gameopt service */
static DEFINE_PER_CPU(struct cpu_freq_status, gpa_cpu_stats);
/* rls: early detect boost, from kernel early_detect module */
static DEFINE_PER_CPU(struct cpu_freq_status, edb_cpu_stats);
/* fst: frame short timeout boost, from kernel early_detect module */
static DEFINE_PER_CPU(struct cpu_freq_status, fst_cpu_stats);
/* flt: frame long timeout boost, from kernel early_detect module */
static DEFINE_PER_CPU(struct cpu_freq_status, flt_cpu_stats);
/* ctb: critical task boost, from kernel critical_task module*/
static DEFINE_PER_CPU(struct cpu_freq_status, ctb_cpu_stats);
/* htb: heavy task boost, from kernel heavy_task module*/
static DEFINE_PER_CPU(struct cpu_freq_status, htb_cpu_stats);
/* chtb: critical and heavy task boost, from GPA*/
static DEFINE_PER_CPU(struct cpu_freq_status, chtb_cpu_stats);
static DEFINE_PER_CPU(struct cpu_freq_status, final_cpu_stats);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_min);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_max);

static cpumask_var_t limit_cpumask;

static atomic_t ready_for_freq_updates = ATOMIC_INIT(0);
static struct hrtimer reinit_hrtimer;
static struct work_struct reinit_work;

/*
 * sameone[uah] can disable GPA cpufreq limit,
 * by writing 1 to /proc/game_opt/disable_cpufreq_limit.
 */
static bool disable_cpufreq_limit = false;
static bool timeout_release_cpufreq_limit = false;
static unsigned int ed_boost_type = ED_BOOST_NONE;
static DEFINE_MUTEX(g_mutex);

/*
 * This is a safeguard mechanism.
 *
 * If GPA made frequency QoS request but not released under some extreme conditions.
 * Kernel releases the frequency QoS request after FREQ_QOS_REQ_MAX_MS.
 * It will probably never happen.
 */
#define FREQ_QOS_REQ_MAX_MS  (60 * MSEC_PER_SEC) /* 60s */
static struct delayed_work freq_qos_req_reset_work;

static int freq_qos_request_init(void)
{
	unsigned int cpu;
	int ret;

	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EINVAL;
			goto cleanup;
		}
		per_cpu(gpa_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(edb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(fst_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(flt_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(ctb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(htb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(chtb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(final_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		req = &per_cpu(qos_req_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(gpa_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		per_cpu(edb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(fst_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(flt_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(ctb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(htb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(chtb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE; /* by designed */
		per_cpu(final_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		req = &per_cpu(qos_req_max, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add max freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		cpufreq_cpu_put(policy);
	}
	return 0;

cleanup:
	for_each_present_cpu(cpu) {
		req = &per_cpu(qos_req_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		req = &per_cpu(qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		per_cpu(gpa_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(gpa_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		per_cpu(edb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(edb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(fst_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(fst_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(flt_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(flt_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(ctb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(ctb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(htb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(htb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(chtb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(chtb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;  /* by designed */
		per_cpu(final_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(final_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	return ret;
}

static void max_freq_qos_update_request(int cpu, struct freq_qos_request *req, unsigned int max_freq)
{
	freq_qos_update_request(req, max_freq);
}

static void __freq_qos_request_restore(void)
{
	int i, j, cpu;
	struct cpumask present_mask;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;

	cpumask_copy(&present_mask, cpu_present_mask);

	cpus_read_lock();
	for_each_cpu(i, &present_mask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, &present_mask);

		cpu = policy.cpu;

		if (per_cpu(final_cpu_stats, cpu).min != per_cpu(gpa_cpu_stats, cpu).min) {
			per_cpu(final_cpu_stats, cpu).min = per_cpu(gpa_cpu_stats, cpu).min;
			req = &per_cpu(qos_req_min, cpu);
			freq_qos_update_request(req, per_cpu(final_cpu_stats, cpu).min);
		}

		if (per_cpu(final_cpu_stats, cpu).max != per_cpu(gpa_cpu_stats, cpu).max) {
			per_cpu(final_cpu_stats, cpu).max = per_cpu(gpa_cpu_stats, cpu).max;
			req = &per_cpu(qos_req_max, cpu);
			max_freq_qos_update_request(cpu, req, per_cpu(final_cpu_stats, cpu).max);
		}
	}
	cpus_read_unlock();
}

static void __freq_qos_request_reset(void)
{
	int i, j, cpu;
	struct cpumask present_mask;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;

	cpumask_copy(&present_mask, cpu_present_mask);

	cpus_read_lock();
	for_each_cpu(i, &present_mask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, &present_mask);

		cpu = policy.cpu;

		if (per_cpu(final_cpu_stats, cpu).min != FREQ_QOS_MIN_DEFAULT_VALUE) {
			per_cpu(final_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
			req = &per_cpu(qos_req_min, cpu);
			freq_qos_update_request(req, FREQ_QOS_MIN_DEFAULT_VALUE);
		}

		if (per_cpu(final_cpu_stats, cpu).max != FREQ_QOS_MAX_DEFAULT_VALUE) {
			per_cpu(final_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
			req = &per_cpu(qos_req_max, cpu);
			freq_qos_update_request(req, FREQ_QOS_MAX_DEFAULT_VALUE);
		}
	}
	cpus_read_unlock();
}

static void freq_qos_request_reset(struct work_struct *work)
{
	unsigned int cpu;

	mutex_lock(&g_mutex);
	timeout_release_cpufreq_limit = true;
	for_each_present_cpu(cpu) {
		per_cpu(gpa_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
		per_cpu(gpa_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	__freq_qos_request_reset();
	mutex_unlock(&g_mutex);
}

static void __ed_freq_boost_request(void)
{
	int i, j, cpu;
	struct cpumask present_mask;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	unsigned int gpa_min, edb_min, fst_min, flt_min, ctb_min, htb_min, final_min;
	unsigned int gpa_max, edb_max, fst_max, flt_max, ctb_max, htb_max, final_max;

	cpumask_copy(&present_mask, cpu_present_mask);

	cpus_read_lock();
	for_each_cpu(i, &present_mask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, &present_mask);

		cpu = policy.cpu;

		final_min = gpa_min = per_cpu(gpa_cpu_stats, cpu).min;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_min = per_cpu(edb_cpu_stats, cpu).min;
			final_min = max(gpa_min, edb_min);
		}
		if (ed_boost_type & ED_BOOST_FST) {
			fst_min = per_cpu(fst_cpu_stats, cpu).min;
			final_min = max(final_min, fst_min);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_min = per_cpu(flt_cpu_stats, cpu).min;
			final_min = max(final_min, flt_min);
		}
		ctb_min = per_cpu(ctb_cpu_stats, cpu).min;
		final_min = max(final_min, ctb_min);
		htb_min = per_cpu(htb_cpu_stats, cpu).min;
		final_min = max(final_min, htb_min);
		if (per_cpu(final_cpu_stats, cpu).min != final_min) {
			per_cpu(final_cpu_stats, cpu).min = final_min;
			req = &per_cpu(qos_req_min, cpu);
			freq_qos_update_request(req, final_min);
		}

		final_max = gpa_max = per_cpu(gpa_cpu_stats, cpu).max;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_max = per_cpu(edb_cpu_stats, cpu).max;
			final_max = max(gpa_max, edb_max);
		}
		if (ed_boost_type & (ED_BOOST_RML | ED_BOOST_FST)) {
			fst_max = per_cpu(fst_cpu_stats, cpu).max;
			final_max = max(final_max, fst_max);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_max = per_cpu(flt_cpu_stats, cpu).max;
			final_max = max(final_max, flt_max);
		}
		ctb_max = per_cpu(ctb_cpu_stats, cpu).max;
		final_max = max(final_max, ctb_max);
		htb_max = per_cpu(htb_cpu_stats, cpu).max;
		final_max = max(final_max, htb_max);
		if (per_cpu(final_cpu_stats, cpu).max != final_max) {
			per_cpu(final_cpu_stats, cpu).max = final_max;
			req = &per_cpu(qos_req_max, cpu);
			max_freq_qos_update_request(cpu, req, per_cpu(final_cpu_stats, cpu).max);
		}
	}
	cpus_read_unlock();
}

void ed_freq_boost_request(unsigned int boost_type)
{
	if (atomic_read(&ready_for_freq_updates) == 0) {
		return;
	}

	mutex_lock(&g_mutex);

	if (boost_type > ED_BOOST_NONE) {
		if (ed_boost_type & boost_type)
			goto unlock;
		ed_boost_type |= boost_type;
	} else {
		if (ed_boost_type == ED_BOOST_NONE)
			goto unlock;
		ed_boost_type = ED_BOOST_NONE;
	}

	if (disable_cpufreq_limit)
		goto unlock;

	if (ed_boost_type > ED_BOOST_NONE)
		__ed_freq_boost_request();
	else
		__freq_qos_request_restore();

	systrace_c_printk("ed_cpufreq_boost", ed_boost_type);

unlock:
	mutex_unlock(&g_mutex);
}

void ch_freq_boost_request(cpumask_var_t control_cpumask, enum CH_BOOST_ACTION action)
{
	if (atomic_read(&ready_for_freq_updates) == 0) {
		return;
	}

	mutex_lock(&g_mutex);
	if (disable_cpufreq_limit) {
		goto unlock;
	}
	int i, j, cpu;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	unsigned int gpa_min, edb_min, fst_min, flt_min, ctb_min, htb_min, final_min;
	unsigned int gpa_max, edb_max, fst_max, flt_max, ctb_max, htb_max, final_max;

	cpus_read_lock();
	for_each_cpu(i, control_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, control_cpumask);

		cpu = policy.cpu;

		final_min = gpa_min = per_cpu(gpa_cpu_stats, cpu).min;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_min = per_cpu(edb_cpu_stats, cpu).min;
			final_min = max(gpa_min, edb_min);
		}
		if (ed_boost_type & ED_BOOST_FST) {
			fst_min = per_cpu(fst_cpu_stats, cpu).min;
			final_min = max(final_min, fst_min);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_min = per_cpu(flt_cpu_stats, cpu).min;
			final_min = max(final_min, flt_min);
		}
		if (action == CT_REQUSET_BOOST) {
			ctb_min = per_cpu(ctb_cpu_stats, cpu).min = per_cpu(chtb_cpu_stats, cpu).min;
			final_min = max(final_min, ctb_min);
		} else if (action == CT_RELEASE_BOOST) {
			ctb_min = per_cpu(ctb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
			final_min = max(final_min, ctb_min);
		}
		if (action == HT_REQUSET_BOOST) {
			htb_min = per_cpu(htb_cpu_stats, cpu).min = per_cpu(chtb_cpu_stats, cpu).min;
			final_min = max(final_min, htb_min);
		} else if (action == HT_RELEASE_BOOST) {
			htb_min = per_cpu(htb_cpu_stats, cpu).min = FREQ_QOS_MIN_DEFAULT_VALUE;
			final_min = max(final_min, htb_min);
		}
		if (per_cpu(final_cpu_stats, cpu).min != final_min) {
			per_cpu(final_cpu_stats, cpu).min = final_min;
			req = &per_cpu(qos_req_min, cpu);
			freq_qos_update_request(req, final_min);
		}

		final_max = gpa_max = per_cpu(gpa_cpu_stats, cpu).max;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_max = per_cpu(edb_cpu_stats, cpu).max;
			final_max = max(gpa_max, edb_max);
		}
		if (ed_boost_type & (ED_BOOST_RML | ED_BOOST_FST)) {
			fst_max = per_cpu(fst_cpu_stats, cpu).max;
			final_max = max(final_max, fst_max);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_max = per_cpu(flt_cpu_stats, cpu).max;
			final_max = max(final_max, flt_max);
		}
		if (action == CT_REQUSET_BOOST) {
			ctb_max = per_cpu(ctb_cpu_stats, cpu).max = per_cpu(chtb_cpu_stats, cpu).max;
			final_max = max(final_max, ctb_max);
		} else if (action == CT_RELEASE_BOOST) {
			ctb_max = per_cpu(ctb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;
			final_max = max(final_max, ctb_max);
		}
		if (action == HT_REQUSET_BOOST) {
			htb_max = per_cpu(htb_cpu_stats, cpu).max = per_cpu(chtb_cpu_stats, cpu).max;
			final_max = max(final_max, htb_max);
		} else if (action == HT_RELEASE_BOOST) {
			htb_max = per_cpu(htb_cpu_stats, cpu).max = FREQ_QOS_MIN_DEFAULT_VALUE;
			final_max = max(final_max, htb_max);
		}
		if (per_cpu(final_cpu_stats, cpu).max != final_max) {
			per_cpu(final_cpu_stats, cpu).max = final_max;
			req = &per_cpu(qos_req_max, cpu);
			max_freq_qos_update_request(cpu, req, per_cpu(final_cpu_stats, cpu).max);
		}
	}
	cpus_read_unlock();

unlock:
	mutex_unlock(&g_mutex);
}

static ssize_t set_cpu_min_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	unsigned int gpa_min, edb_min, fst_min, flt_min, final_min;
	bool chtb_status = false;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(gpa_cpu_stats, cpu).min = val;
		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	/*
	 * Since on synchronous systems policy is shared amongst multiple
	 * CPUs only one CPU needs to be updated for the limit to be
	 * reflected for the entire cluster. We can avoid updating the policy
	 * of other CPUs in the cluster once it is done for at least one CPU
	 * in the cluster
	 */
	cpus_read_lock();
	for_each_cpu(i, limit_cpumask) {
		if (per_cpu(ctb_cpu_stats, cpu).min != FREQ_QOS_MIN_DEFAULT_VALUE || per_cpu(htb_cpu_stats, cpu).min != FREQ_QOS_MIN_DEFAULT_VALUE) {
			chtb_status = true;
			break;
		}
	}
	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(gpa_cpu_stats, j).min = per_cpu(gpa_cpu_stats, i).min;
		}

		if (disable_cpufreq_limit)
			continue;

		if (chtb_status) {
			continue;
		}

		cpu = policy.cpu;

		final_min = gpa_min = per_cpu(gpa_cpu_stats, cpu).min;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_min = per_cpu(edb_cpu_stats, cpu).min;
			final_min = max(final_min, edb_min);
		}
		if (ed_boost_type & ED_BOOST_FST) {
			fst_min = per_cpu(fst_cpu_stats, cpu).min;
			final_min = max(final_min, fst_min);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_min = per_cpu(flt_cpu_stats, cpu).min;
			final_min = max(final_min, flt_min);
		}

		if (per_cpu(final_cpu_stats, cpu).min != final_min) {
			per_cpu(final_cpu_stats, cpu).min = final_min;
			req = &per_cpu(qos_req_min, cpu);
			freq_qos_update_request(req, final_min);
		}
	}
	cpus_read_unlock();

	return count;
}

static ssize_t cpu_min_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	cancel_delayed_work_sync(&freq_qos_req_reset_work);
	mutex_lock(&g_mutex);
	timeout_release_cpufreq_limit = false;
	ret = set_cpu_min_freq(page, ret);
	mutex_unlock(&g_mutex);
	schedule_delayed_work(&freq_qos_req_reset_work, msecs_to_jiffies(FREQ_QOS_REQ_MAX_MS));

	return ret;
}

static int cpu_min_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu) {
		if (disable_cpufreq_limit)
			seq_printf(m, "%d:%u ", cpu, FREQ_QOS_MIN_DEFAULT_VALUE);
		else
			seq_printf(m, "%d:%u ", cpu, per_cpu(gpa_cpu_stats, cpu).min);
	}
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int cpu_min_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, cpu_min_freq_show, inode);
}

static const struct proc_ops cpu_min_freq_proc_ops = {
	.proc_open		= cpu_min_freq_proc_open,
	.proc_write 	= cpu_min_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t set_cpu_max_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	unsigned int gpa_max, edb_max, fst_max, flt_max, final_max;
	bool chtb_status = false;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(gpa_cpu_stats, cpu).max = min_t(uint, val,
			(unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);
		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	cpus_read_lock();
	for_each_cpu(i, limit_cpumask) {
		if (per_cpu(ctb_cpu_stats, cpu).min != FREQ_QOS_MIN_DEFAULT_VALUE || per_cpu(htb_cpu_stats, cpu).min != FREQ_QOS_MIN_DEFAULT_VALUE) {
			chtb_status = true;
			break;
		}
	}
	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(gpa_cpu_stats, j).max = per_cpu(gpa_cpu_stats, i).max;
		}

		if (disable_cpufreq_limit)
			continue;

		if (chtb_status) {
			continue;
		}

		cpu = policy.cpu;

		final_max = gpa_max = per_cpu(gpa_cpu_stats, cpu).max;
		if (ed_boost_type & ED_BOOST_EDB) {
			edb_max = per_cpu(edb_cpu_stats, cpu).max;
			final_max = max(final_max, edb_max);
		}
		if (ed_boost_type & (ED_BOOST_RML | ED_BOOST_FST)) {
			fst_max = per_cpu(fst_cpu_stats, cpu).max;
			final_max = max(final_max, fst_max);
		}
		if (ed_boost_type & ED_BOOST_FLT) {
			flt_max = per_cpu(flt_cpu_stats, cpu).max;
			final_max = max(final_max, flt_max);
		}

		if (per_cpu(final_cpu_stats, cpu).max != final_max) {
			per_cpu(final_cpu_stats, cpu).max = final_max;
			req = &per_cpu(qos_req_max, cpu);
			max_freq_qos_update_request(cpu, req, per_cpu(final_cpu_stats, cpu).max);
		}
	}
	cpus_read_unlock();

	return count;
}

static ssize_t cpu_max_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	cancel_delayed_work_sync(&freq_qos_req_reset_work);
	mutex_lock(&g_mutex);
	timeout_release_cpufreq_limit = false;
	ret = set_cpu_max_freq(page, ret);
	mutex_unlock(&g_mutex);
	schedule_delayed_work(&freq_qos_req_reset_work, msecs_to_jiffies(FREQ_QOS_REQ_MAX_MS));

	return ret;
}

static int cpu_max_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu) {
		if (disable_cpufreq_limit)
			seq_printf(m, "%d:%u ", cpu, FREQ_QOS_MAX_DEFAULT_VALUE);
		else
			seq_printf(m, "%d:%u ", cpu, per_cpu(gpa_cpu_stats, cpu).max);
	}
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int cpu_max_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, cpu_max_freq_show, inode);
}

static const struct proc_ops cpu_max_freq_proc_ops = {
	.proc_open		= cpu_max_freq_proc_open,
	.proc_write 	= cpu_max_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t edb_set_cpu_min_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(edb_cpu_stats, cpu).min = val;

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(edb_cpu_stats, j).min = per_cpu(edb_cpu_stats, i).min;
		}
	}

	return count;
}

static ssize_t edb_cpu_min_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = edb_set_cpu_min_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int edb_cpu_min_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(edb_cpu_stats, cpu).min);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int edb_cpu_min_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, edb_cpu_min_freq_show, inode);
}

static const struct proc_ops edb_cpu_min_freq_proc_ops = {
	.proc_open		= edb_cpu_min_freq_proc_open,
	.proc_write 	= edb_cpu_min_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t edb_set_cpu_max_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(edb_cpu_stats, cpu).max = min_t(uint, val,
			(unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(edb_cpu_stats, j).max = per_cpu(edb_cpu_stats, i).max;
		}
	}

	return count;
}

static ssize_t edb_cpu_max_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = edb_set_cpu_max_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int edb_cpu_max_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(edb_cpu_stats, cpu).max);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int edb_cpu_max_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, edb_cpu_max_freq_show, inode);
}

static const struct proc_ops edb_cpu_max_freq_proc_ops = {
	.proc_open		= edb_cpu_max_freq_proc_open,
	.proc_write 	= edb_cpu_max_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t fst_set_cpu_max_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(fst_cpu_stats, cpu).max = min_t(uint, val,
			(unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(fst_cpu_stats, j).max = per_cpu(fst_cpu_stats, i).max;
		}
	}

	return count;
}

static ssize_t fst_cpu_max_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = fst_set_cpu_max_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int fst_cpu_max_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(fst_cpu_stats, cpu).max);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int fst_cpu_max_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, fst_cpu_max_freq_show, inode);
}

static const struct proc_ops fst_cpu_max_freq_proc_ops = {
	.proc_open		= fst_cpu_max_freq_proc_open,
	.proc_write 	= fst_cpu_max_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t fst_set_cpu_min_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(fst_cpu_stats, cpu).min = val;

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(fst_cpu_stats, j).min = per_cpu(fst_cpu_stats, i).min;
		}
	}

	return count;
}

static ssize_t fst_cpu_min_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = fst_set_cpu_min_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int fst_cpu_min_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(fst_cpu_stats, cpu).min);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int fst_cpu_min_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, fst_cpu_min_freq_show, inode);
}

static const struct proc_ops fst_cpu_min_freq_proc_ops = {
	.proc_open		= fst_cpu_min_freq_proc_open,
	.proc_write		= fst_cpu_min_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t flt_set_cpu_min_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(flt_cpu_stats, cpu).min = val;

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(flt_cpu_stats, j).min = per_cpu(flt_cpu_stats, i).min;
		}
	}

	return count;
}

static ssize_t flt_cpu_min_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = flt_set_cpu_min_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int flt_cpu_min_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(flt_cpu_stats, cpu).min);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int flt_cpu_min_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, flt_cpu_min_freq_show, inode);
}

static const struct proc_ops flt_cpu_min_freq_proc_ops = {
	.proc_open		= flt_cpu_min_freq_proc_open,
	.proc_write		= flt_cpu_min_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t flt_set_cpu_max_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(flt_cpu_stats, cpu).max = min_t(uint, val,
			(unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(flt_cpu_stats, j).max = per_cpu(flt_cpu_stats, i).max;
		}
	}

	return count;
}

static ssize_t flt_cpu_max_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = flt_set_cpu_max_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int flt_cpu_max_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(flt_cpu_stats, cpu).max);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int flt_cpu_max_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, flt_cpu_max_freq_show, inode);
}

static const struct proc_ops flt_cpu_max_freq_proc_ops = {
	.proc_open		= flt_cpu_max_freq_proc_open,
	.proc_write		= flt_cpu_max_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t chtb_set_cpu_min_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(chtb_cpu_stats, cpu).min = val;

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(chtb_cpu_stats, j).min = per_cpu(chtb_cpu_stats, i).min;
		}
	}

	return count;
}

static ssize_t chtb_cpu_min_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = chtb_set_cpu_min_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int chtb_cpu_min_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(chtb_cpu_stats, cpu).min);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int chtb_cpu_min_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, chtb_cpu_min_freq_show, inode);
}

static const struct proc_ops chtb_cpu_min_freq_proc_ops = {
	.proc_open		= chtb_cpu_min_freq_proc_open,
	.proc_write 	= chtb_cpu_min_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t chtb_set_cpu_max_freq(const char *buf, size_t count)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpufreq_policy policy;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_cpumask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		per_cpu(chtb_cpu_stats, cpu).max = min_t(uint, val,
			(unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);

		cpumask_set_cpu(cpu, limit_cpumask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	for_each_cpu(i, limit_cpumask) {
		if (cpufreq_get_policy(&policy, i))
			continue;

		for_each_cpu(j, policy.related_cpus) {
			cpumask_clear_cpu(j, limit_cpumask);
			per_cpu(chtb_cpu_stats, j).max = per_cpu(chtb_cpu_stats, i).max;
		}
	}

	return count;
}

static ssize_t chtb_cpu_max_freq_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[256] = {0};
	int ret;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	mutex_lock(&g_mutex);
	ret = chtb_set_cpu_max_freq(page, ret);
	mutex_unlock(&g_mutex);

	return ret;
}

static int chtb_cpu_max_freq_show(struct seq_file *m, void *v)
{
	int cpu;

	mutex_lock(&g_mutex);
	for_each_present_cpu(cpu)
		seq_printf(m, "%d:%u ", cpu, per_cpu(chtb_cpu_stats, cpu).max);
	seq_printf(m, "\n");
	mutex_unlock(&g_mutex);

	return 0;
}

static int chtb_cpu_max_freq_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, chtb_cpu_max_freq_show, inode);
}

static const struct proc_ops chtb_cpu_max_freq_proc_ops = {
	.proc_open		= chtb_cpu_max_freq_proc_open,
	.proc_write 	= chtb_cpu_max_freq_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t disable_cpufreq_limit_proc_write(struct file *file,
	const char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int ret, value;
	bool disable;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &value);
	if (ret != 1)
		return -EINVAL;

	if (value != 0 && value != 1)
		return -EINVAL;

	disable = value == 1;

	mutex_lock(&g_mutex);

	if (disable_cpufreq_limit != disable) {
		disable_cpufreq_limit = disable;

		if (timeout_release_cpufreq_limit)
			goto unlock;

		if (disable_cpufreq_limit)
			__freq_qos_request_reset();
		else
			__freq_qos_request_restore();
	}

unlock:
	mutex_unlock(&g_mutex);
	return count;
}

static ssize_t disable_cpufreq_limit_proc_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	char page[32] = {0};
	int len;

	mutex_lock(&g_mutex);
	len = sprintf(page, "%d\n", disable_cpufreq_limit);
	mutex_unlock(&g_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops disable_cpufreq_limit_proc_ops = {
	.proc_write		= disable_cpufreq_limit_proc_write,
	.proc_read		= disable_cpufreq_limit_proc_read,
	.proc_lseek		= default_llseek,
};

int __cpufreq_limits_init(void)
{
	int ret;

	pr_info("%s: into\n", __func__);

	ret = freq_qos_request_init();
	if (ret) {
		pr_err("%s: Failed to init qos requests policy for ret=%d\n",
			__func__, ret);
		return ret;
	}

	INIT_DELAYED_WORK(&freq_qos_req_reset_work, freq_qos_request_reset);

	proc_create_data("cpu_min_freq", 0664, game_opt_dir, &cpu_min_freq_proc_ops, NULL);
	proc_create_data("cpu_max_freq", 0664, game_opt_dir, &cpu_max_freq_proc_ops, NULL);
	proc_create_data("edb_cpu_min_freq", 0664, early_detect_dir, &edb_cpu_min_freq_proc_ops, NULL);
	proc_create_data("edb_cpu_max_freq", 0664, early_detect_dir, &edb_cpu_max_freq_proc_ops, NULL);
	proc_create_data("fst_cpu_min_freq", 0664, early_detect_dir, &fst_cpu_min_freq_proc_ops, NULL);
	proc_create_data("fst_cpu_max_freq", 0664, early_detect_dir, &fst_cpu_max_freq_proc_ops, NULL);
	proc_create_data("flt_cpu_min_freq", 0664, early_detect_dir, &flt_cpu_min_freq_proc_ops, NULL);
	proc_create_data("flt_cpu_max_freq", 0664, early_detect_dir, &flt_cpu_max_freq_proc_ops, NULL);
	proc_create_data("chtb_cpu_min_freq", 0664, game_opt_dir, &chtb_cpu_min_freq_proc_ops, NULL);
	proc_create_data("chtb_cpu_max_freq", 0664, game_opt_dir, &chtb_cpu_max_freq_proc_ops, NULL);
	proc_create_data("disable_cpufreq_limit", 0664, game_opt_dir, &disable_cpufreq_limit_proc_ops, NULL);

	atomic_set(&ready_for_freq_updates, 1);

	return 0;
}

static void reinit_cpufreq_limits(struct work_struct *work)
{
	int ret;

	ret = __cpufreq_limits_init();
	if (ret) {
		hrtimer_start(&reinit_hrtimer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}
}

static enum hrtimer_restart reinit_hrtimer_callback(struct hrtimer *timer)
{
	schedule_work(&reinit_work);
	return HRTIMER_NORESTART;
}

int cpufreq_limits_init(void)
{
	int ret;

	if (!alloc_cpumask_var(&limit_cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = __cpufreq_limits_init();
	if (ret) {
		INIT_WORK(&reinit_work, reinit_cpufreq_limits);

		hrtimer_init(&reinit_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		reinit_hrtimer.function = reinit_hrtimer_callback;
		hrtimer_start(&reinit_hrtimer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}

	return 0;
}
