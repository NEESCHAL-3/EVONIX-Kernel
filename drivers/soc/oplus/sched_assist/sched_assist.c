// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/cgroup-defs.h>
#include <linux/sched/cputime.h>
#include <kernel/sched/sched.h>
#include <linux/cgroup.h>
#include <trace/hooks/cgroup.h>
#include <trace/hooks/signal.h>
#include <trace/events/sched.h>
#include <linux/kprobes.h>

#include "sched_assist.h"
#include "sa_common.h"
#include "sa_sysfs.h"
#include "sa_exec.h"
#include "sa_fair.h"
#include "sa_oemdata.h"
#include "sa_priority.h"
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
#include "sa_balance.h"
#endif
#include "sa_hmbird.h"

#define HI_MASK		0xFF00000000000000UL
#define HI_FLAG		0xAB00000000000000UL

static void set_ux_to_task(struct task_struct *new) {
	struct pt_regs *childregs = task_pt_regs(new);
	unsigned long newsp;
	unsigned long fn_addr;
	void __user *user_sp_ptr;
	int ux_state;

	if (is_compat_thread(task_thread_info(new)))
		newsp = childregs->compat_sp;
	else
		newsp = childregs->sp;

	if ((void *)newsp == NULL)
		return;

	user_sp_ptr = (void __user *)(uintptr_t)(newsp);
	if (0 != copy_from_user(&fn_addr, user_sp_ptr, sizeof(unsigned long)))
		return;

	if ((fn_addr & HI_MASK) != HI_FLAG)
		return;

	ux_state = oplus_get_static_ux_state(current->group_leader);
	if (ux_state) {
		oplus_set_ux_state_lock(new, ux_state, -1, true);
		return;
	}

	ux_state = oplus_get_inherited_ux_state(current->group_leader);
	if (ux_state) {
		oplus_set_ux_state_lock(new, ux_state, INHERIT_UX_MAX, true);
	}
}

static void android_rvh_wake_up_new_task_handler(void *unused, struct task_struct *new) {
	set_ux_to_task(new);
}

static int register_scheduler_vendor_hooks(void)
{
	int ret;

	/* register vender hook in kernel/sched/fair.c */
	REGISTER_TRACE_RVH(android_rvh_place_entity, android_rvh_place_entity_handler);
#ifdef OPLUS_UX_EEVDF_COMPATIBLE
	REGISTER_TRACE_RVH(android_rvh_update_deadline, android_rvh_update_deadline_handler);
#endif
	REGISTER_TRACE_RVH(android_rvh_can_migrate_task, android_rvh_can_migrate_task_handler);
#ifndef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	/* REGISTER_TRACE_RVH(android_rvh_post_init_entity_util_avg, android_rvh_post_init_entity_util_avg_handler); */
#endif
	/* REGISTER_TRACE_RVH(android_rvh_select_task_rq_fair, android_rvh_select_task_rq_fair_handler); */
	/* REGISTER_TRACE_RVH(android_rvh_find_energy_efficient_cpu, android_rvh_find_energy_efficient_cpu_handler); */

	REGISTER_TRACE_RVH(android_rvh_enqueue_entity, android_rvh_enqueue_entity_handler);
#ifdef CONFIG_LOCKING_PROTECT
	REGISTER_TRACE_RVH(android_rvh_dequeue_entity, android_rvh_dequeue_entity_handler);
#endif

#ifndef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	REGISTER_TRACE_RVH(android_rvh_check_preempt_wakeup, android_rvh_check_preempt_wakeup_handler);
#ifndef CONFIG_MTK_SCHED_VIP_TASK
	REGISTER_TRACE_RVH(android_rvh_replace_next_task_fair, android_rvh_replace_next_task_fair_handler);
#endif
#endif

	/* register vender hook in kernel/sched/topology.c */
	REGISTER_TRACE_VH(android_vh_build_sched_domains, android_vh_build_sched_domains_handler);

	/* register vender hook in  kernel/sched/rt.c */
#ifndef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	/* REGISTER_TRACE_RVH(android_rvh_select_task_rq_rt, android_rvh_select_task_rq_rt_handler); */
	REGISTER_TRACE_RVH(android_rvh_find_lowest_rq, android_rvh_find_lowest_rq_handler);
#endif

	/* register vender hook in kernel/sched/core.c */
	REGISTER_TRACE_RVH(android_rvh_sched_fork, android_rvh_sched_fork_handler);
#ifndef CONFIG_MTK_SCHED_FAST_LOAD_TRACKING
	REGISTER_TRACE_RVH(android_rvh_schedule, android_rvh_schedule_handler);
#endif
	REGISTER_TRACE_RVH(android_vh_scheduler_tick, android_vh_scheduler_tick_handler);
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	REGISTER_TRACE_RVH(android_rvh_after_enqueue_task, android_rvh_after_enqueue_task_handler);
	REGISTER_TRACE_RVH(android_rvh_dequeue_task, android_rvh_dequeue_task_handler);
#endif

	REGISTER_TRACE_RVH(android_rvh_set_cpus_allowed_by_task, android_rvh_set_cpus_allowed_by_task_handler);
	REGISTER_TRACE_RVH(android_rvh_setscheduler, android_rvh_setscheduler_handler);

#ifndef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	/* register vender hook in fs/exec.c */
	REGISTER_TRACE_VH(task_rename, task_rename_handler);
#endif

	/* register vendor hook in kernel/cgroup/cgroup-v1.c */
	/* REGISTER_TRACE_VH(android_vh_cgroup_set_task, android_vh_cgroup_set_task_handler); */

	/* register vendor hook in kernel/signal.c  */
	REGISTER_TRACE_VH(android_vh_exit_signal, android_vh_exit_signal_handler);

	REGISTER_TRACE_VH(sched_stat_runtime, android_vh_sched_stat_runtime_handler);

	REGISTER_TRACE_VH(android_rvh_wake_up_new_task, android_rvh_wake_up_new_task_handler);

#ifdef CONFIG_BLOCKIO_UX_OPT
	sa_blockio_init();
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_BAN_APP_SET_AFFINITY)
	/* register vendor hook in kernel/core.c */
	REGISTER_TRACE_VH(android_vh_sched_setaffinity_early, android_vh_sched_setaffinity_early_handler);
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
	oplus_loadbalance_init();
#endif

	REGISTER_TRACE_VH(android_vh_account_process_tick_gran, android_vh_account_process_tick_gran_handler);
#ifdef CONFIG_OPLUS_FEATURE_TICK_GRAN
	REGISTER_TRACE_VH(sched_switch, sa_sched_switch_handler);
#endif

#ifdef CONFIG_OPLUS_SCHED_GROUP_OPT
	REGISTER_TRACE_VH(android_vh_reweight_entity, android_vh_reweight_entity_handler);
#endif
	return 0;
}

#define OPLUS_OEM_DATA_SIZE_TEST(ostruct, kstruct)		\
	BUILD_BUG_ON(sizeof(ostruct) > (sizeof(u64) *		\
		ARRAY_SIZE(((kstruct *)0)->android_oem_data1)))

typedef int (*profile_event_register_t)(enum profile_type type,
		struct notifier_block *n);
static profile_event_register_t  _profile_event_register;
kallsyms_lookup_name_t _kallsyms_lookup_name;
EXPORT_SYMBOL_GPL(_kallsyms_lookup_name);

int __nocfi detect_symbol(void)
{
	int ret;
	static struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name"
	};

	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_warn("register  failed\n");
		return ret;
	}
	_kallsyms_lookup_name  = (kallsyms_lookup_name_t)kp.addr;
	_profile_event_register = (profile_event_register_t)(*_kallsyms_lookup_name)("profile_event_register");
	pr_info("_profile_event_register:%ps\n", _profile_event_register);
	unregister_kprobe(&kp);

	return 0;
}


static int __init oplus_sched_assist_init(void)
{
	int ret;

	/* compile time checks for vendor data size */
	OPLUS_OEM_DATA_SIZE_TEST(struct oplus_rq, struct rq);

	ret = sa_oemdata_init();
	if (ret != 0)
		return ret;

	global_sched_assist_enabled |= FEATURE_COMMON;
#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
	global_sched_assist_enabled |= FEATURE_SPREAD;
#endif /* CONFIG_OPLUS_FEATURE_SCHED_SPREAD */

	sched_assist_init_oplus_rq();
	update_ux_sched_cputopo();
#ifdef CONFIG_OPLUS_FEATURE_TICK_GRAN
	resched_timer_init();
#endif

	ret = register_scheduler_vendor_hooks();
	if (ret != 0)
		return ret;

	ret = oplus_sched_assist_proc_init();
	if (ret != 0)
		return ret;
	detect_symbol();
	if (_profile_event_register)
		/* register a notifier to monitor task exit */
		(*_profile_event_register)(PROFILE_TASK_EXIT, &process_exit_notifier_block);

	hmbird_sched_ops_init();

	ux_debug("sched assist init succeed!\n");
	return 0;
}

module_init(oplus_sched_assist_init);

MODULE_DESCRIPTION("Oplus Sched Assist Vender Hooks Driver");
MODULE_LICENSE("GPL v2");
