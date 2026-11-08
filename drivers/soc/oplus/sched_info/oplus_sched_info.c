// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include "osi_base.h"
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <trace/events/power.h>
#include "osi_tasktrack.h"
#include "osi_hotthread.h"
#include "osi_topology.h"
#include "osi_freq.h"
#include "osi_cputime.h"
#include "osi_cpuload.h"
#include "osi_compat_runtime.h"
#include "osi_version.h"
#include "osi_enable.h"
#include "osi_debug.h"
#include "osi_cpuloadmonitor.h"
#include "osi_healthinfo.h"
#include "osi_amu.h"
#include "../sched_assist/cputime.h"

static struct proc_dir_entry *d_task_info;
static struct proc_dir_entry *d_cpu_jank_info;

#if IS_ENABLED(CONFIG_OPLUS_PROCS_LOAD_STATE)
	extern int osi_procs_cpu_usage_init(void);
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
void android_vh_irqtime_account_process_tick_handler(
			void *unused,
			struct task_struct *p, struct rq *rq, int user_tick, int ticks)
{
	jankinfo_update_time_info(rq, p, ticks*TICK_NSEC);
}
#else
void android_vh_account_task_time_handler(void *unused,
			struct task_struct *p, struct rq *rq, int user_tick, int ticks)
{
	jankinfo_update_time_info(rq, p, ticks*TICK_NSEC);
}
#endif

static void rodin_cpu_frequency_handler(void *unused,
		unsigned int frequency, unsigned int cpu)
{
	osi_cpufreq_transition_handler(frequency, cpu);
}


static int jank_register_hook(void)
{
	int ret;

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	ret = register_trace_android_vh_irqtime_account_process_tick(
		android_vh_irqtime_account_process_tick_handler, NULL);
#else
	ret = register_trace_android_vh_account_task_time(
		android_vh_account_task_time_handler, NULL);
#endif
	if (ret)
		return ret;
	ret = register_trace_cpu_frequency(rodin_cpu_frequency_handler, NULL);
	if (ret) {
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
		unregister_trace_android_vh_irqtime_account_process_tick(
			android_vh_irqtime_account_process_tick_handler, NULL);
#else
		unregister_trace_android_vh_account_task_time(
			android_vh_account_task_time_handler, NULL);
#endif
		tracepoint_synchronize_unregister();
	}
	return ret;
}

static void jank_unregister_hook(void)
{
	unregister_trace_cpu_frequency(rodin_cpu_frequency_handler, NULL);
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	unregister_trace_android_vh_irqtime_account_process_tick(
		android_vh_irqtime_account_process_tick_handler, NULL);
#else
	unregister_trace_android_vh_account_task_time(
		android_vh_account_task_time_handler, NULL);
#endif
	tracepoint_synchronize_unregister();
}

#define JANK_INFO_DIR				"jank_info"
#define JANK_INFO_PROC_NODE			"cpu_jank_info"

static bool jank_sampling_active;
static bool jank_tasktrack_active;
static bool jank_calcload_active;
static bool jank_hotthread_active;
static bool jank_frequency_active;
static bool jank_health_active;

static void rodin_jank_stop(void)
{
	if (jank_sampling_active) {
		jank_unregister_hook();
		jank_sampling_active = false;
	}
	/* Remove and drain public interfaces before releasing their backing state. */
	proc_remove(d_task_info);
	d_task_info = NULL;
	d_cpu_jank_info = NULL;
	if (jank_health_active) {
		oplus_healthinfo_proc_fs_exit();
		jank_health_active = false;
	}
	if (jank_calcload_active) {
		jank_calcload_exit();
		jank_calcload_active = false;
	}
	if (jank_tasktrack_active) {
		tasktrack_deinit();
		jank_tasktrack_active = false;
	}
	if (jank_frequency_active) {
		osi_freq_exit(NULL);
		jank_frequency_active = false;
	}
	if (jank_hotthread_active) {
		osi_hotthread_proc_deinit(NULL);
		jank_hotthread_active = false;
	}
}

static int rodin_jank_start(void)
{
	int ret;

	ret = cluster_init();
	if (ret)
		return ret;
	jank_cpuload_init();
	osi_compat_runtime_init();
	ret = jank_calcload_init();
	if (ret)
		return ret;
	jank_calcload_active = true;
	ret = tasktrack_init();
	if (ret)
		goto fail;
	jank_tasktrack_active = true;
	d_task_info = proc_mkdir(JANK_INFO_DIR, NULL);
	if (!d_task_info) {
		ret = -ENOMEM;
		goto fail;
	}
	d_cpu_jank_info = proc_mkdir(JANK_INFO_PROC_NODE, d_task_info);
	if (!d_cpu_jank_info) {
		ret = -ENOMEM;
		goto fail;
	}
	if (!jank_enable_proc_init(d_cpu_jank_info) ||
	    !jank_debug_proc_init(d_cpu_jank_info) ||
	    !jank_tasktrack_proc_init(d_cpu_jank_info) ||
	    !jank_cpuload_proc_init(d_cpu_jank_info) ||
	    !jank_version_proc_init(d_cpu_jank_info) ||
	    !jank_calcload_proc_init(d_cpu_jank_info)) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = osi_hotthread_proc_init(d_cpu_jank_info);
	if (ret)
		goto fail;
	jank_hotthread_active = true;
	ret = osi_base_proc_init(d_cpu_jank_info);
	if (ret)
		goto fail;
	ret = osi_freq_init(d_cpu_jank_info);
	if (ret)
		goto fail;
	jank_frequency_active = true;
	ret = oplus_healthinfo_proc_fs_init();
	if (ret)
		goto fail;
	jank_health_active = true;
	ret = jank_register_hook();
	if (ret)
		goto fail;
	jank_sampling_active = true;
	return 0;
fail:
	rodin_jank_stop();
	return ret;
}

static atomic_t rodin_jank_requested = ATOMIC_INIT(0);
static bool rodin_jank_started;

static bool rodin_jank_policies_ready(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	bool ready;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return false;
		ready = policy->cpuinfo.max_freq != 0;
		cpufreq_cpu_put(policy);
		if (!ready)
			return false;
	}
	return true;
}

static void rodin_jank_start_workfn(struct work_struct *work)
{
	int ret = rodin_jank_start();

	if (ret) {
		if (ret == -EPROBE_DEFER)
			atomic_set(&rodin_jank_requested, 0);
		pr_err("Oplus scheduler information init failed: %d\n", ret);
		return;
	}
	WRITE_ONCE(rodin_jank_started, true);
}
static DECLARE_WORK(rodin_jank_start_work, rodin_jank_start_workfn);

static void rodin_jank_request_start(void)
{
	if (rodin_jank_policies_ready() &&
	    atomic_cmpxchg(&rodin_jank_requested, 0, 1) == 0)
		schedule_work(&rodin_jank_start_work);
}

static int rodin_jank_policy_event(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	if (event == CPUFREQ_CREATE_POLICY)
		rodin_jank_request_start();
	return NOTIFY_OK;
}

static struct notifier_block rodin_jank_policy_nb = {
	.notifier_call = rodin_jank_policy_event,
};

static int __init jank_info_init(void)
{
	int ret = cpufreq_register_notifier(&rodin_jank_policy_nb,
					  CPUFREQ_POLICY_NOTIFIER);

	if (ret)
		return ret;
	rodin_jank_request_start();
	return 0;
}

static void __exit jank_info_exit(void)
{
	cpufreq_unregister_notifier(&rodin_jank_policy_nb, CPUFREQ_POLICY_NOTIFIER);
	cancel_work_sync(&rodin_jank_start_work);
	if (READ_ONCE(rodin_jank_started))
		rodin_jank_stop();
}

module_init(jank_info_init);
module_exit(jank_info_exit);

MODULE_DESCRIPTION("oplus_schedinfo");
MODULE_LICENSE("GPL v2");

