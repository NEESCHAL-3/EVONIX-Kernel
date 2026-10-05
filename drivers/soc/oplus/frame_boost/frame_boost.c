// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
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
static int __init oplus_frame_boost_init(void)
{
	int ret = 0;

	fbg_sysctl_init();

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

	/* please register your hooks at the end of init. */
	register_frame_group_vendor_hooks();

	fbg_migrate_task_callback = fbg_skip_migration;
	fbg_android_rvh_schedule_callback = fbg_android_rvh_schedule_handler;

	kp.post_handler = rb_simple_write_handler_post;
	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_warn("FrameBoost: optional rb_simple_write kprobe unavailable: %d\n", ret);
		ret = 0;
	} else {
		fbg_dbg_init();
	}

	ofb_debug("oplus_bsp_frame_boost.ko init succeed!!\n");


out:
	return ret;
}

module_init(oplus_frame_boost_init);
MODULE_DESCRIPTION("Oplus Frame Boost Moduler");
MODULE_LICENSE("GPL v2");
