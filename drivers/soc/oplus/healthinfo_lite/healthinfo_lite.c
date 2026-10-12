// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX OPlus HealthInfo compatibility layer.
 *
 * Native interface compatibility based on OPlus MT6899 sched_info:
 *   /proc/oplus_healthinfo/cpu_loading
 *   /proc/oplus_healthinfo/cpu_info
 *   /proc/fg_info/fg_uids
 *
 * CPU load uses differential kernel_cpustat sampling with a 100 ms cache.
 */

#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/topology.h>
#include <linux/seq_file.h>
#include <linux/tick.h>
#include <linux/uaccess.h>

#define HEALTHINFO_PROC_NODE "oplus_healthinfo"
#define CPU_LOADING_NODE     "cpu_loading"
#define CPU_INFO_NODE        "cpu_info"

#define FG_INFO_PROC_NODE    "fg_info"
#define FG_UIDS_NODE         "fg_uids"

#define CPU_LOAD_CACHE_MS    100

struct oplus_cpu_load_stat {
	u64 t_user;
	u64 t_system;
	u64 t_idle;
	u64 t_iowait;
	u64 t_irq;
	u64 t_softirq;
};

static struct proc_dir_entry *healthinfo_dir;
static struct proc_dir_entry *fg_info_dir;

static DEFINE_MUTEX(cpu_load_lock);
static struct oplus_cpu_load_stat prev_cpu_sample;
static unsigned long last_load_jiffies;
static bool have_prev_cpu_sample;
static int cached_cpu_load;

static int fg_uid = -555;

#ifdef arch_idle_time

static u64 oplus_healthinfo_get_idle_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 idle = kcs->cpustat[CPUTIME_IDLE];

	if (cpu_online(cpu) && !nr_iowait_cpu(cpu))
		idle += arch_idle_time(cpu);

	return idle;
}

static u64 oplus_healthinfo_get_iowait_time(struct kernel_cpustat *kcs,
					     int cpu)
{
	u64 iowait = kcs->cpustat[CPUTIME_IOWAIT];

	if (cpu_online(cpu) && nr_iowait_cpu(cpu))
		iowait += arch_idle_time(cpu);

	return iowait;
}

#else

static u64 oplus_healthinfo_get_idle_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 idle_usecs = -1ULL;

	if (cpu_online(cpu))
		idle_usecs = get_cpu_idle_time_us(cpu, NULL);

	if (idle_usecs == -1ULL)
		return kcs->cpustat[CPUTIME_IDLE];

	return idle_usecs * NSEC_PER_USEC;
}

static u64 oplus_healthinfo_get_iowait_time(struct kernel_cpustat *kcs,
					     int cpu)
{
	u64 iowait_usecs = -1ULL;

	if (cpu_online(cpu))
		iowait_usecs = get_cpu_iowait_time_us(cpu, NULL);

	if (iowait_usecs == -1ULL)
		return kcs->cpustat[CPUTIME_IOWAIT];

	return iowait_usecs * NSEC_PER_USEC;
}

#endif

static void oplus_healthinfo_snapshot(struct oplus_cpu_load_stat *stat)
{
	int cpu;

	memset(stat, 0, sizeof(*stat));

	for_each_online_cpu(cpu) {
		struct kernel_cpustat *kcs = &kcpustat_cpu(cpu);

		stat->t_user +=
			kcs->cpustat[CPUTIME_USER] +
			kcs->cpustat[CPUTIME_NICE];

		stat->t_system += kcs->cpustat[CPUTIME_SYSTEM];
		stat->t_idle += oplus_healthinfo_get_idle_time(kcs, cpu);
		stat->t_iowait += oplus_healthinfo_get_iowait_time(kcs, cpu);
		stat->t_irq += kcs->cpustat[CPUTIME_IRQ];
		stat->t_softirq += kcs->cpustat[CPUTIME_SOFTIRQ];
	}
}

static inline u64 safe_delta(u64 now, u64 old)
{
	return now >= old ? now - old : 0;
}

static int oplus_healthinfo_get_cur_cpuload(void)
{
	struct oplus_cpu_load_stat cpu_sample;
	u64 d_user;
	u64 d_system;
	u64 d_idle;
	u64 d_iowait;
	u64 d_irq;
	u64 d_softirq;
	u64 busy;
	u64 total;
	unsigned long now = jiffies;
	int ret;

	mutex_lock(&cpu_load_lock);

	if (have_prev_cpu_sample &&
	    time_before(now,
			last_load_jiffies +
			msecs_to_jiffies(CPU_LOAD_CACHE_MS))) {
		ret = cached_cpu_load;
		goto out;
	}

	oplus_healthinfo_snapshot(&cpu_sample);

	if (!have_prev_cpu_sample) {
		prev_cpu_sample = cpu_sample;
		last_load_jiffies = now;
		have_prev_cpu_sample = true;
		ret = cached_cpu_load;
		goto out;
	}

	d_user = safe_delta(cpu_sample.t_user, prev_cpu_sample.t_user);
	d_system = safe_delta(cpu_sample.t_system, prev_cpu_sample.t_system);
	d_idle = safe_delta(cpu_sample.t_idle, prev_cpu_sample.t_idle);
	d_iowait = safe_delta(cpu_sample.t_iowait, prev_cpu_sample.t_iowait);
	d_irq = safe_delta(cpu_sample.t_irq, prev_cpu_sample.t_irq);
	d_softirq = safe_delta(cpu_sample.t_softirq, prev_cpu_sample.t_softirq);

	/*
	 * Keep the same OPlus accounting semantics:
	 * iowait is included in both total and busy load.
	 */
	busy = d_user + d_system + d_iowait + d_irq + d_softirq;
	total = busy + d_idle;

	if (total) {
		cached_cpu_load = (int)((busy * 100ULL) / total);

		if (cached_cpu_load < 0)
			cached_cpu_load = 0;
		else if (cached_cpu_load > 100)
			cached_cpu_load = 100;
	}

	prev_cpu_sample = cpu_sample;
	last_load_jiffies = now;

	ret = cached_cpu_load;

out:
	mutex_unlock(&cpu_load_lock);
	return ret;
}

/*
 * /proc/oplus_healthinfo/cpu_loading
 */
static ssize_t cpu_loading_read(struct file *file,
				char __user *buf,
				size_t count,
				loff_t *ppos)
{
	char page[160];
	int len;
	int load;

	load = oplus_healthinfo_get_cur_cpuload();

	len = scnprintf(page, sizeof(page),
			"cur_cpuloading: %d\n"
			"cur_cpu_ctrl: true\n"
			"cur_cpu_logon: false\n"
			"cur_cpu_trig: false\n",
			load);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops cpu_loading_proc_ops = {
	.proc_read = cpu_loading_read,
	.proc_lseek = default_llseek,
};

/*
 * /proc/oplus_healthinfo/cpu_info
 *
 * Matches OPlus MT6899 behaviour: print one max frequency for
 * each unique CPU capacity / cluster.
 */
static int cpu_info_show(struct seq_file *m, void *v)
{
	unsigned int cpu;
	unsigned long capacity;
	unsigned long last_capacity = ULONG_MAX;
	bool first = true;

	for_each_possible_cpu(cpu) {
		unsigned int max_freq;

		capacity = arch_scale_cpu_capacity(cpu);

		if (capacity == last_capacity)
			continue;

		last_capacity = capacity;
		max_freq = cpufreq_quick_get_max(cpu);

		if (!first)
			seq_putc(m, ' ');

		seq_printf(m, "%u", max_freq);
		first = false;
	}

	seq_putc(m, '\n');

	return 0;
}

static int cpu_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpu_info_show, NULL);
}

static const struct proc_ops cpu_info_proc_ops = {
	.proc_open = cpu_info_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*
 * Native OPlus foreground UID interface.
 */
bool is_fg(int uid)
{
	return uid == READ_ONCE(fg_uid);
}
EXPORT_SYMBOL_GPL(is_fg);

static int fg_uids_show(struct seq_file *m, void *v)
{
	seq_printf(m, "fg_uids: %d\n", READ_ONCE(fg_uid));
	return 0;
}

static int fg_uids_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_uids_show, NULL);
}

static ssize_t fg_uids_write(struct file *file,
			     const char __user *buf,
			     size_t count,
			     loff_t *ppos)
{
	char buffer[32];
	int uid;
	int ret;

	if (!count)
		return 0;

	if (count >= sizeof(buffer))
		return -EINVAL;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';

	ret = kstrtoint(strstrip(buffer), 0, &uid);
	if (ret)
		return ret;

	WRITE_ONCE(fg_uid, uid);

	return count;
}

static const struct proc_ops fg_uids_proc_ops = {
	.proc_open = fg_uids_open,
	.proc_read = seq_read,
	.proc_write = fg_uids_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_healthinfo_lite_init(void)
{
	struct proc_dir_entry *entry;

	oplus_healthinfo_snapshot(&prev_cpu_sample);
	last_load_jiffies = jiffies;
	have_prev_cpu_sample = true;

	healthinfo_dir = proc_mkdir(HEALTHINFO_PROC_NODE, NULL);
	if (!healthinfo_dir) {
		pr_err("OPlus HealthInfo Lite: failed to create /proc/%s\n",
		       HEALTHINFO_PROC_NODE);
		return -ENOMEM;
	}

	entry = proc_create(CPU_LOADING_NODE, 0444,
			    healthinfo_dir, &cpu_loading_proc_ops);
	if (!entry)
		goto err_healthinfo;

	entry = proc_create(CPU_INFO_NODE, 0444,
			    healthinfo_dir, &cpu_info_proc_ops);
	if (!entry)
		goto err_healthinfo;

	fg_info_dir = proc_mkdir(FG_INFO_PROC_NODE, NULL);
	if (!fg_info_dir)
		goto err_healthinfo;

	entry = proc_create(FG_UIDS_NODE, 0666,
			    fg_info_dir, &fg_uids_proc_ops);
	if (!entry)
		goto err_all;

	pr_info("OPlus HealthInfo Lite: cpu_loading, cpu_info and fg_uids ready\n");

	return 0;

err_all:
	remove_proc_subtree(FG_INFO_PROC_NODE, NULL);
	fg_info_dir = NULL;

err_healthinfo:
	remove_proc_subtree(HEALTHINFO_PROC_NODE, NULL);
	healthinfo_dir = NULL;

	return -ENOMEM;
}

static void __exit oplus_healthinfo_lite_exit(void)
{
	if (fg_info_dir)
		remove_proc_subtree(FG_INFO_PROC_NODE, NULL);

	if (healthinfo_dir)
		remove_proc_subtree(HEALTHINFO_PROC_NODE, NULL);

	fg_info_dir = NULL;
	healthinfo_dir = NULL;
}

module_init(oplus_healthinfo_lite_init);
module_exit(oplus_healthinfo_lite_exit);

MODULE_DESCRIPTION("EVONIX OPlus HealthInfo MT6899 compatibility");
MODULE_LICENSE("GPL");
