// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/cred.h>
#include <linux/cpufreq.h>
#include <linux/cpufreq_times.h>
#include <linux/energy_model.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <trace/hooks/cpufreq.h>
#include "evonix_cos_task_energy.h"

#define EVX_ENERGY_WINDOW_NS (5ULL * NSEC_PER_SEC)
#define EVX_ENERGY_SAMPLES 8192
#define EVX_ENERGY_COPY_CHUNK 128

struct evx_energy_sample {
	u64 end_ns;
	u64 runtime_ns;
	u64 energy_ns;
	pid_t pid, tgid;
	char comm[TASK_COMM_LEN];
};

struct evx_energy_cpu {
	raw_spinlock_t lock;
	unsigned int next, count;
	u64 overwritten_end_ns;
	struct evx_energy_sample samples[EVX_ENERGY_SAMPLES];
};

struct evx_energy_history {
	atomic64_t last_bad_ns;
	struct evx_energy_cpu cpus[];
};

struct evx_energy_snapshot {
	struct evx_energy_sample *samples;
	unsigned int count;
};

static DEFINE_MUTEX(evx_energy_control_lock);
static struct evx_energy_history *evx_energy_history;

/* Same Energy Model power units as native Oplus task_cpustats. */
static int evx_energy_power(int cpu, unsigned int frequency, u64 *power)
{
	struct em_perf_domain *domain;
	struct em_perf_table *table;
	int low = 0, high, mid, ret = -ENODATA;

	if (!frequency)
		return -ENODATA;
	rcu_read_lock();
	domain = em_cpu_get(cpu);
	table = domain ? rcu_dereference(domain->em_table) : NULL;
	if (!table || em_is_artificial(domain) || domain->nr_perf_states <= 0)
		goto out;
	high = domain->nr_perf_states - 1;
	/* Match the Energy Model's ceiling-state lookup for an intermediate OPP. */
	while (low < high) {
		mid = low + (high - low) / 2;
		if (table->state[mid].frequency < frequency)
			low = mid + 1;
		else
			high = mid;
	}
	if (frequency > table->state[low].frequency || !table->state[low].power)
		goto out;
	*power = table->state[low].power;
	ret = 0;
out:
	rcu_read_unlock();
	return ret;
}

/* Callback ordering across CPUs must not move the last bad sample backward. */
static void evx_energy_mark_bad(struct evx_energy_history *history, u64 now)
{
	s64 previous = atomic64_read(&history->last_bad_ns);

	while ((u64)previous < now &&
	       !atomic64_try_cmpxchg(&history->last_bad_ns, &previous, now))
		cpu_relax();
}

static void evx_energy_record(void *data, u64 runtime_ns,
			      struct task_struct *task, unsigned int state)
{
	/* Pair with enable's release publication of the initialized histories. */
	struct evx_energy_history *history = smp_load_acquire(&evx_energy_history);
	struct evx_energy_cpu *cpu_history;
	struct evx_energy_sample *sample;
	unsigned long flags;
	unsigned int cpu = task_cpu(task);
	u64 power, energy, now;

	if (!history || !runtime_ns || is_idle_task(task))
		return;
	now = ktime_get_mono_fast_ns();
	if (cpu >= nr_cpu_ids || runtime_ns > now ||
	    evx_energy_power(cpu, cpufreq_times_lookup_frequency(cpu, state), &power) ||
	    check_mul_overflow(power, runtime_ns, &energy)) {
		evx_energy_mark_bad(history, now);
		return;
	}
	cpu_history = &history->cpus[cpu];
	raw_spin_lock_irqsave(&cpu_history->lock, flags);
	sample = &cpu_history->samples[cpu_history->next];
	if (cpu_history->count == EVX_ENERGY_SAMPLES)
		cpu_history->overwritten_end_ns = max(cpu_history->overwritten_end_ns,
						    sample->end_ns);
	sample->end_ns = now;
	sample->runtime_ns = runtime_ns;
	sample->energy_ns = energy;
	sample->pid = task_pid_nr(task);
	sample->tgid = task_tgid_nr(task);
	memcpy(sample->comm, task->comm, TASK_COMM_LEN);
	sample->comm[TASK_COMM_LEN - 1] = '\0';
	cpu_history->next = (cpu_history->next + 1) % EVX_ENERGY_SAMPLES;
	if (cpu_history->count < EVX_ENERGY_SAMPLES)
		cpu_history->count++;
	raw_spin_unlock_irqrestore(&cpu_history->lock, flags);
}

static int evx_energy_set_enabled(bool enabled)
{
	struct evx_energy_history *history;
	int cpu, ret = 0;

	mutex_lock(&evx_energy_control_lock);
	history = evx_energy_history;
	if (!!history == enabled)
		goto out;
	if (!enabled) {
		ret = unregister_trace_android_vh_cpufreq_acct_update_power(evx_energy_record, NULL);
		if (ret)
			goto out;
		tracepoint_synchronize_unregister();
		/* All producers are drained; process readers hold this mutex. */
		WRITE_ONCE(evx_energy_history, NULL);
		kvfree(history);
		goto out;
	}
	history = kvzalloc(struct_size(history, cpus, nr_cpu_ids), GFP_KERNEL);
	if (!history) {
		ret = -ENOMEM;
		goto out;
	}
	for_each_possible_cpu(cpu)
		raw_spin_lock_init(&history->cpus[cpu].lock);
	/* Publish initialized per-CPU locks and buffers to trace callbacks. */
	smp_store_release(&evx_energy_history, history);
	ret = register_trace_android_vh_cpufreq_acct_update_power(evx_energy_record, NULL);
	if (ret) {
		/* Registration failed, so no callback owns the allocation. */
		WRITE_ONCE(evx_energy_history, NULL);
		kvfree(history);
	}
out:
	mutex_unlock(&evx_energy_control_lock);
	return ret;
}

static int evx_energy_compare(const void *left, const void *right)
{
	const struct evx_energy_sample *a = left, *b = right;

	if (a->pid != b->pid)
		return (a->pid > b->pid) - (a->pid < b->pid);
	return (a->tgid > b->tgid) - (a->tgid < b->tgid);
}

static bool evx_energy_clip(struct evx_energy_sample *sample, u64 begin, u64 end)
{
	u64 sample_begin, overlap_begin, overlap_end;

	if (!sample->runtime_ns || sample->runtime_ns > sample->end_ns)
		return false;
	sample_begin = sample->end_ns - sample->runtime_ns;
	overlap_begin = max(sample_begin, begin);
	overlap_end = min(sample->end_ns, end);
	if (overlap_begin >= overlap_end)
		return false;
	sample->energy_ns = mul_u64_u64_div_u64(sample->energy_ns,
				 overlap_end - overlap_begin, sample->runtime_ns);
	return true;
}

static int evx_energy_collect(struct evx_energy_snapshot *snapshot)
{
	struct evx_energy_history *history = evx_energy_history;
	struct evx_energy_cpu *cpu_history;
	struct evx_energy_sample *samples = snapshot->samples;
	unsigned int copied, chunk, input, output = 0;
	unsigned long flags;
	u64 end = ktime_get_mono_fast_ns();
	u64 begin = end > EVX_ENERGY_WINDOW_NS ? end - EVX_ENERGY_WINDOW_NS : 0;
	int cpu;

	if (!history)
		return -EPERM;
	if (atomic64_read(&history->last_bad_ns) > begin)
		return -ENODATA;
	for_each_possible_cpu(cpu) {
		cpu_history = &history->cpus[cpu];
		/* Copy in small pieces to bound interrupt-disabled latency. */
		for (copied = 0; copied < EVX_ENERGY_SAMPLES; copied += chunk) {
			chunk = min_t(unsigned int, EVX_ENERGY_COPY_CHUNK,
				      EVX_ENERGY_SAMPLES - copied);
			raw_spin_lock_irqsave(&cpu_history->lock, flags);
			memcpy(samples + output + copied, cpu_history->samples + copied,
			       array_size(chunk, sizeof(*samples)));
			raw_spin_unlock_irqrestore(&cpu_history->lock, flags);
		}
		raw_spin_lock_irqsave(&cpu_history->lock, flags);
		if (cpu_history->overwritten_end_ns > begin) {
			raw_spin_unlock_irqrestore(&cpu_history->lock, flags);
			return -EOVERFLOW;
		}
		raw_spin_unlock_irqrestore(&cpu_history->lock, flags);
		for (input = 0; input < EVX_ENERGY_SAMPLES; input++) {
			struct evx_energy_sample *sample = samples + output + input;

			if (!evx_energy_clip(sample, begin, end))
				continue;
			/* Compact only this CPU's input; keep previous CPUs intact. */
			samples[snapshot->count++] = *sample;
		}
		output += EVX_ENERGY_SAMPLES;
	}
	if (atomic64_read(&history->last_bad_ns) > begin)
		return -ENODATA;
	sort(samples, snapshot->count, sizeof(*samples), evx_energy_compare, NULL);
	output = 0;
	for (input = 0; input < snapshot->count; input++) {
		if (output && samples[output - 1].pid == samples[input].pid &&
		    samples[output - 1].tgid == samples[input].tgid) {
			if (check_add_overflow(samples[output - 1].energy_ns,
					       samples[input].energy_ns,
					       &samples[output - 1].energy_ns))
				return -EOVERFLOW;
		} else {
			samples[output++] = samples[input];
		}
	}
	snapshot->count = output;
	return 0;
}

static int evx_task_energy_show(struct seq_file *m, void *v)
{
	struct evx_energy_snapshot *snapshot = m->private;
	unsigned int index;

	for (index = 0; index < snapshot->count; index++) {
		const struct evx_energy_sample *sample = &snapshot->samples[index];

		seq_printf(m, "%d\t%d\t%llu\t%s\n", sample->pid, sample->tgid,
			   div_u64(sample->energy_ns, NSEC_PER_MSEC), sample->comm);
	}
	return 0;
}

static int evx_task_energy_open(struct inode *inode, struct file *file)
{
	struct evx_energy_snapshot *snapshot;
	int ret;

	if (!READ_ONCE(evx_energy_history))
		return -EPERM;
	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	snapshot->samples = kvmalloc_array(array_size(nr_cpu_ids, EVX_ENERGY_SAMPLES),
					 sizeof(*snapshot->samples), GFP_KERNEL);
	if (!snapshot->samples) {
		kfree(snapshot);
		return -ENOMEM;
	}
	mutex_lock(&evx_energy_control_lock);
	ret = evx_energy_collect(snapshot);
	mutex_unlock(&evx_energy_control_lock);
	if (!ret)
		ret = single_open(file, evx_task_energy_show, snapshot);
	if (ret) {
		kvfree(snapshot->samples);
		kfree(snapshot);
	}
	return ret;
}

static int evx_task_energy_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct evx_energy_snapshot *snapshot = seq->private;

	kvfree(snapshot->samples);
	kfree(snapshot);
	return single_release(inode, file);
}

static int evx_task_energy_enable_show(struct seq_file *m, void *v)
{
	mutex_lock(&evx_energy_control_lock);
	seq_printf(m, "%u\n", !!evx_energy_history);
	mutex_unlock(&evx_energy_control_lock);
	return 0;
}

static int evx_task_energy_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_task_energy_enable_show, NULL);
}

static ssize_t evx_task_energy_enable_write(struct file *file, const char __user *buf,
					   size_t count, loff_t *ppos)
{
	unsigned int enabled;
	int ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
	    !uid_eq(current_euid(), KUIDT_INIT(1000)))
		return -EPERM;
	ret = kstrtouint_from_user(buf, count, 10, &enabled);

	if (ret)
		return ret;
	if (enabled > 1)
		return -EINVAL;
	ret = evx_energy_set_enabled(enabled);
	if (ret)
		return ret;
	return count;
}

const struct proc_ops evx_task_energy_proc_ops = {
	.proc_open = evx_task_energy_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = evx_task_energy_release,
};

const struct proc_ops evx_task_energy_enable_proc_ops = {
	.proc_open = evx_task_energy_enable_open,
	.proc_read = seq_read,
	.proc_write = evx_task_energy_enable_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
