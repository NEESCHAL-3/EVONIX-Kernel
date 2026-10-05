// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/kprobes.h>

#include "frame_boost.h"
#include "frame_debug.h"
#include "frame_timer.h"

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
#include "cfbt_boost.h"
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
#include "../sched_assist/sa_common.h"
#endif

struct fbg_vendor_hook fbg_hook;


extern struct frame_group *frame_boost_groups[MAX_NUM_FBG_ID];

static struct kprobe kp = {
	.symbol_name    = "rb_simple_write",
};

static inline bool vaild_stune_boost_type(unsigned int type)
{
	return (type >= 0) && (type < BOOST_MAX_TYPE);
}

void fbg_set_stune_boost(int value, int grp_id, unsigned int type)
{
	struct frame_group *grp = NULL;

	if (!vaild_stune_boost_type(type))
		return;
	grp = frame_boost_groups[grp_id];
	if (!grp)
		return;
	grp->stune_boost[type] = value;
}
EXPORT_SYMBOL_GPL(fbg_set_stune_boost);

int fbg_get_stune_boost(int grp_id, unsigned int type)
{
	struct frame_group *grp = NULL;

	if (!vaild_stune_boost_type(type))
		return 0;
	grp = frame_boost_groups[grp_id];
	if (!grp)
		return 0;
	return grp->stune_boost[type];
}
EXPORT_SYMBOL_GPL(fbg_get_stune_boost);

static void __kprobes rb_simple_write_handler_post(struct kprobe *p, struct pt_regs *regs,
	unsigned long flags)
{
	fbg_dbg_reset();
}

/******************
 * moduler function
 *******************/
static atomic_t fbg_deferred_init_started = ATOMIC_INIT(0);

static void fbg_deferred_init_workfn(struct work_struct *work);
static DECLARE_WORK(fbg_deferred_init_work, fbg_deferred_init_workfn);

static bool fbg_cpufreq_policies_ready(void)
{
        struct cpufreq_policy *policy;
        int cpu;

        if (!IS_ENABLED(CONFIG_CPU_FREQ))
                return true;

        for_each_possible_cpu(cpu) {
                policy = cpufreq_cpu_get(cpu);
                if (!policy)
                        return false;

                if (!policy->cpuinfo.max_freq) {
                        cpufreq_cpu_put(policy);
                        return false;
                }

                cpufreq_cpu_put(policy);
        }

        return true;
}

static int oplus_frame_boost_finish_init(void)
{
        int ret = 0;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_CFBT)
        ret = cfbt_frame_group_init();
        if (ret != 0)
                goto out;
#endif /* CONFIG_OPLUS_FEATURE_SCHED_CFBT */

        ret = frame_info_init();
        if (ret != 0)
                goto out;

        ret = frame_group_init();
        if (ret != 0)
                goto out;
        ofb_debug("frame_group init succeed!!\n");

        ret = frame_info_init();
        if (ret != 0)
                goto out;
        ofb_debug("frame_info init succeed!!\n");

        ret = frame_timer_init();
        if (ret != 0)
                goto out;
        ofb_debug("frame_timer init succeed!!\n");

        /* Register hooks only after cluster topology is ready. */
        register_frame_group_vendor_hooks();

        fbg_migrate_task_callback = fbg_skip_migration;
        fbg_android_rvh_schedule_callback = fbg_android_rvh_schedule_handler;

        kp.post_handler = rb_simple_write_handler_post;
        ret = register_kprobe(&kp);
        if (ret < 0) {
                pr_warn("FrameBoost: optional rb_simple_write kprobe unavailable: %d\n",
                        ret);
                ret = 0;
        } else {
                fbg_dbg_init();
        }

        /*
         * Expose userspace controls only after all FrameBoost state,
         * hooks and callbacks are ready.
         */
        fbg_sysctl_init();

        pr_info("FrameBoost: initialized after cpufreq policies became ready\n");
        ofb_debug("oplus_bsp_frame_boost.ko init succeed!!\n");

out:
        return ret;
}

static int fbg_cpufreq_policy_notifier(struct notifier_block *nb,
                                       unsigned long event, void *data)
{
        if (event != CPUFREQ_CREATE_POLICY)
                return NOTIFY_DONE;

        if (!fbg_cpufreq_policies_ready())
                return NOTIFY_DONE;

        if (atomic_cmpxchg(&fbg_deferred_init_started, 0, 1) == 0)
                schedule_work(&fbg_deferred_init_work);

        return NOTIFY_OK;
}

static struct notifier_block fbg_cpufreq_nb = {
        .notifier_call = fbg_cpufreq_policy_notifier,

        /*
         * Run after the generic topology capacity notifier so the final
         * policy can normalize arch_scale_cpu_capacity() first.
         */
        .priority = -100,
};

static void fbg_deferred_init_workfn(struct work_struct *work)
{
        int ret;

        cpufreq_unregister_notifier(&fbg_cpufreq_nb,
                                    CPUFREQ_POLICY_NOTIFIER);

        ret = oplus_frame_boost_finish_init();
        if (ret)
                pr_err("FrameBoost: deferred initialization failed: %d\n", ret);
}

static int __init oplus_frame_boost_init(void)
{
        int ret;

        /*
         * If cpufreq already finished before FrameBoost starts, capacities
         * are already normalized and initialization can proceed immediately.
         */
        if (fbg_cpufreq_policies_ready()) {
                atomic_set(&fbg_deferred_init_started, 1);
                pr_info("FrameBoost: cpufreq policies already ready\n");
                return oplus_frame_boost_finish_init();
        }

        ret = cpufreq_register_notifier(&fbg_cpufreq_nb,
                                        CPUFREQ_POLICY_NOTIFIER);
        if (ret) {
                pr_err("FrameBoost: failed to register cpufreq notifier: %d\n",
                       ret);
                return ret;
        }

        /*
         * Close the race where the final policy completed while the
         * notifier was being registered.
         */
        if (fbg_cpufreq_policies_ready()) {
                if (atomic_cmpxchg(&fbg_deferred_init_started, 0, 1) == 0)
                        schedule_work(&fbg_deferred_init_work);
        } else {
                pr_info("FrameBoost: waiting for cpufreq policies\n");
        }

        return 0;
}

module_init(oplus_frame_boost_init);
MODULE_DESCRIPTION("Oplus Frame Boost Moduler");
MODULE_LICENSE("GPL v2");
