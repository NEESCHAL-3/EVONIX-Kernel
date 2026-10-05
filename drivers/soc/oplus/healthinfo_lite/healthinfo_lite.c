// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal OPlus HealthInfo compatibility layer for EVONIX.
 *
 * Provides:
 *   /proc/oplus_healthinfo/cpu_loading
 *
 * CPU load calculation based on OPlus MT6899 osi_loadinfo.c.
 */

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/stat.h>
#include <linux/tick.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#define HEALTHINFO_PROC_NODE "oplus_healthinfo"
#define CPU_LOADING_NODE     "cpu_loading"

struct oplus_cpu_load_stat {
	u64 t_user;
	u64 t_system;
	u64 t_idle;
	u64 t_iowait;
	u64 t_irq;
	u64 t_softirq;
};

static struct proc_dir_entry *healthinfo_dir;

#ifdef arch_idle_time

static u64 oplus_healthinfo_get_idle_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 idle;

	idle = kcs->cpustat[CPUTIME_IDLE];

	if (cpu_online(cpu) && !nr_iowait_cpu(cpu))
		idle += arch_idle_time(cpu);

	return idle;
}

static u64 oplus_healthinfo_get_iowait_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 iowait;

	iowait = kcs->cpustat[CPUTIME_IOWAIT];

	if (cpu_online(cpu) && nr_iowait_cpu(cpu))
		iowait += arch_idle_time(cpu);

	return iowait;
}

#else

static u64 oplus_healthinfo_get_idle_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 idle;
	u64 idle_usecs = -1ULL;

	if (cpu_online(cpu))
		idle_usecs = get_cpu_idle_time_us(cpu, NULL);

	if (idle_usecs == -1ULL)
		idle = kcs->cpustat[CPUTIME_IDLE];
	else
		idle = idle_usecs * NSEC_PER_USEC;

	return idle;
}

static u64 oplus_healthinfo_get_iowait_time(struct kernel_cpustat *kcs, int cpu)
{
	u64 iowait;
	u64 iowait_usecs = -1ULL;

	if (cpu_online(cpu))
		iowait_usecs = get_cpu_iowait_time_us(cpu, NULL);

	if (iowait_usecs == -1ULL)
		iowait = kcs->cpustat[CPUTIME_IOWAIT];
	else
		iowait = iowait_usecs * NSEC_PER_USEC;

	return iowait;
}

#endif

static void oplus_healthinfo_snapshot(struct oplus_cpu_load_stat *stat)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct kernel_cpustat *kcs = &kcpustat_cpu(cpu);

		stat->t_user += kcs->cpustat[CPUTIME_USER];
		stat->t_system += kcs->cpustat[CPUTIME_SYSTEM];
		stat->t_idle += oplus_healthinfo_get_idle_time(kcs, cpu);
		stat->t_iowait += oplus_healthinfo_get_iowait_time(kcs, cpu);
		stat->t_irq += kcs->cpustat[CPUTIME_IRQ];
		stat->t_softirq += kcs->cpustat[CPUTIME_SOFTIRQ];
	}
}

static int oplus_healthinfo_get_cur_cpuload(void)
{
	struct oplus_cpu_load_stat first = { 0 };
	struct oplus_cpu_load_stat second = { 0 };
	clock_t user;
	clock_t system;
	clock_t idle;
	clock_t iowait;
	clock_t irq;
	clock_t softirq;
	clock_t load;
	clock_t sum;

	oplus_healthinfo_snapshot(&first);

	msleep(25);

	oplus_healthinfo_snapshot(&second);

	user = nsec_to_clock_t(second.t_user) -
	       nsec_to_clock_t(first.t_user);

	system = nsec_to_clock_t(second.t_system) -
		 nsec_to_clock_t(first.t_system);

	idle = nsec_to_clock_t(second.t_idle) -
	       nsec_to_clock_t(first.t_idle);

	iowait = nsec_to_clock_t(second.t_iowait) -
		 nsec_to_clock_t(first.t_iowait);

	irq = nsec_to_clock_t(second.t_irq) -
	      nsec_to_clock_t(first.t_irq);

	softirq = nsec_to_clock_t(second.t_softirq) -
		  nsec_to_clock_t(first.t_softirq);

	sum = user + system + idle + iowait + irq + softirq;
	load = user + system + iowait + irq + softirq;

	if (!sum)
		return 0;

	return (int)(100 * load / sum);
}

static ssize_t cpu_loading_read(struct file *file,
				char __user *buf,
				size_t count,
				loff_t *ppos)
{
	char page[160];
	int len;
	int load;

	load = oplus_healthinfo_get_cur_cpuload();

	if (load < 0)
		load = 0;
	else if (load > 100)
		load = 100;

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

static int __init oplus_healthinfo_lite_init(void)
{
	struct proc_dir_entry *entry;

	healthinfo_dir = proc_mkdir(HEALTHINFO_PROC_NODE, NULL);
	if (!healthinfo_dir) {
		pr_err("OPlus HealthInfo Lite: failed to create /proc/%s\n",
		       HEALTHINFO_PROC_NODE);
		return -ENOMEM;
	}

	entry = proc_create(CPU_LOADING_NODE, 0444,
			    healthinfo_dir, &cpu_loading_proc_ops);
	if (!entry) {
		pr_err("OPlus HealthInfo Lite: failed to create cpu_loading\n");
		remove_proc_subtree(HEALTHINFO_PROC_NODE, NULL);
		healthinfo_dir = NULL;
		return -ENOMEM;
	}

	pr_info("OPlus HealthInfo Lite: /proc/oplus_healthinfo/cpu_loading ready\n");

	return 0;
}

static void __exit oplus_healthinfo_lite_exit(void)
{
	if (healthinfo_dir)
		remove_proc_subtree(HEALTHINFO_PROC_NODE, NULL);

	healthinfo_dir = NULL;
}

module_init(oplus_healthinfo_lite_init);
module_exit(oplus_healthinfo_lite_exit);

MODULE_DESCRIPTION("EVONIX OPlus HealthInfo Lite compatibility");
MODULE_LICENSE("GPL");
