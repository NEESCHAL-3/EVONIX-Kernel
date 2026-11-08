#include <linux/init.h>
#include <linux/cred.h>
#include "evonix_cos_cpu_tables.h"
#include "evonix_cos_task_energy.h"
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static ssize_t evx_read_one(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	const char *s = "1\n";
	return simple_read_from_buffer(buf, count, ppos, s, 2);
}

static ssize_t evx_read_zero(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	const char *s = "0\n";
	return simple_read_from_buffer(buf, count, ppos, s, 2);
}

static ssize_t evx_write_ok(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	return count;
}

static long evx_ioctl_ok(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static const struct proc_ops evx_one_rw_ops = {
	.proc_read  = evx_read_one,
	.proc_write = evx_write_ok,
	.proc_ioctl = evx_ioctl_ok,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_ioctl_ok,
#endif
};

static const struct proc_ops evx_zero_rw_ops = {
	.proc_read  = evx_read_zero,
	.proc_write = evx_write_ok,
	.proc_ioctl = evx_ioctl_ok,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_ioctl_ok,
#endif
};

static void evx_proc_touch(const char *path, umode_t mode,
			   const struct proc_ops *ops)
{
	proc_create(path, mode, NULL, ops);
}

static int __init evx_cos_proc_compat_init(void)
{
	struct proc_dir_entry *task_info;
	struct proc_dir_entry *task_cpustats;
	struct proc_dir_entry *healthinfo;

	task_info = proc_mkdir("task_info", NULL);
	if (task_info) {
		task_cpustats = proc_mkdir("task_cpustats", task_info);
		if (task_cpustats) {
			struct proc_dir_entry *enable;

			enable = proc_create("task_cpustats_enable",
				IS_ENABLED(CONFIG_EVONIX_OPLUS_TASK_ENERGY) ? 0660 : 0666,
				task_cpustats, IS_ENABLED(CONFIG_EVONIX_OPLUS_TASK_ENERGY) ?
				&evx_task_energy_enable_proc_ops : &evx_one_rw_ops);
			if (enable && IS_ENABLED(CONFIG_EVONIX_OPLUS_TASK_ENERGY))
				proc_set_user(enable, KUIDT_INIT(1000), KGIDT_INIT(1000));
			proc_create("sgeinfo", 0444,
				    task_cpustats, &evx_sgeinfo_proc_ops);
			proc_create("sgefreqinfo", 0444,
				    task_cpustats, &evx_sgefreqinfo_proc_ops);
		}

		{
			struct proc_dir_entry *task_load = proc_mkdir("task_load_info", task_info);
			if (task_load) {
				proc_create("monitor_status", 0666, task_load, &evx_zero_rw_ops);
				proc_create("normalize_real_time", 0666, task_load, &evx_zero_rw_ops);
				proc_create("normalize_running_time", 0666, task_load, &evx_zero_rw_ops);
				proc_create("real_time", 0666, task_load, &evx_zero_rw_ops);
				proc_create("running_time", 0666, task_load, &evx_zero_rw_ops);
				proc_create("target_process", 0666, task_load, &evx_zero_rw_ops);
			}
		}

		{
			struct proc_dir_entry *task_sched = proc_mkdir("task_sched_info", task_info);
			if (task_sched) {
				proc_create("d_backtrace", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("d_convert", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("io_backtrace", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("limit_table", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("pids_set", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("sched_info_threshold", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("task_sched_info_enable", 0666, task_sched, &evx_one_rw_ops);
				proc_create("tids_set", 0666, task_sched, &evx_zero_rw_ops);
				proc_create("version", 0444, task_sched, &evx_one_rw_ops);
			}
		}
	}

	/* Native health providers own this directory when selected. */
	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO) &&
	    !IS_ENABLED(CONFIG_EVONIX_OPLUS_HEALTHINFO_LITE)) {
		healthinfo = proc_mkdir("oplus_healthinfo", NULL);
		if (healthinfo)
			proc_create("alloc_wait", 0666, healthinfo, &evx_zero_rw_ops);
	}

	/* Use compatibility nodes only when native OPlus SchedAssist is absent. */
	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)) {
		proc_mkdir("oplus_scheduler", NULL);
		proc_mkdir("oplus_scheduler/sched_assist", NULL);
		proc_mkdir("oplus_scheduler/sched_assist/audio", NULL);

		evx_proc_touch("oplus_scheduler/sched_assist/ux_task",
			       0666, &evx_zero_rw_ops);
		evx_proc_touch("oplus_scheduler/sched_assist/ux_task_app",
			       0666, &evx_zero_rw_ops);
		evx_proc_touch("oplus_scheduler/sched_assist/audio/status",
			       0666, &evx_one_rw_ops);
	}

	/* The native protocol owns this tree when selected. */
	if (!IS_ENABLED(CONFIG_EVONIX_OPLUS_FRAME_IOCTL)) {
		proc_mkdir("oplus_frame_boost", NULL);
		evx_proc_touch("oplus_frame_boost/sys_ctrl", 0666, &evx_zero_rw_ops);
		evx_proc_touch("oplus_frame_boost/ctrl", 0666, &evx_zero_rw_ops);
	}


	pr_info("EVONIX-COS: compatibility interface registration complete\n");
	return 0;
}
late_initcall(evx_cos_proc_compat_init);
