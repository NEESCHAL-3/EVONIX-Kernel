// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX Adaptive Memory Reclaim Controller
 *
 * Proactive background reclaim headroom controller to eliminate direct
 * reclaim UI stalls during foreground allocation bursts while strictly
 * preserving stock watermark_boost_factor buddy compaction.
 *
 * Hardened with:
 * - system_freezable_power_efficient_wq to sleep during suspend.
 * - pm_notifier to cleanly cancel background work before suspend and resume after.
 * - Adaptive polling intervals (3000ms idle, 1000ms active pressure).
 * - kswapd wakeup rate-limiting to prevent ZRAM churn and page-cache thrashing.
 */

#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/sysctl.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <linux/nodemask.h>
#include <linux/suspend.h>
#include <linux/jiffies.h>

static int evonix_adaptive_reclaim = 1;
static int evonix_reclaim_cushion_mb = 64;
static int cushion_min_mb = 16;
static int cushion_max_mb = 512;

static struct delayed_work evonix_reclaim_dwork;
static unsigned long last_kswapd_wakeup_jiffies;

static void evonix_reclaim_work_fn(struct work_struct *work)
{
	int nid;
	bool needed_reclaim = false;
	unsigned long next_delay_ms;

	if (!READ_ONCE(evonix_adaptive_reclaim))
		return;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		int z;

		for (z = 0; z <= ZONE_NORMAL; z++) {
			struct zone *zone = &pgdat->node_zones[z];
			unsigned long free, high, cushion_pages, target;

			if (!managed_zone(zone))
				continue;

			high = zone->_watermark[WMARK_HIGH];
			cushion_pages = (READ_ONCE(evonix_reclaim_cushion_mb) * (1024UL * 1024UL)) / PAGE_SIZE;
			target = high + cushion_pages;
			free = zone_page_state(zone, NR_FREE_PAGES);

			if (free < target) {
				needed_reclaim = true;
				/* Rate limit wakeups to at most once per second unless below low watermark */
				if (time_after(jiffies, last_kswapd_wakeup_jiffies + HZ) ||
				    free < zone->_watermark[WMARK_LOW]) {
					wakeup_kswapd(zone, GFP_KERNEL, 0, zone_idx(zone));
					last_kswapd_wakeup_jiffies = jiffies;
				}
			}
		}
	}

	/* Adaptive interval: 1000ms if memory is under target, 3000ms if headroom is healthy */
	next_delay_ms = needed_reclaim ? 1000 : 3000;

	queue_delayed_work(system_freezable_power_efficient_wq, &evonix_reclaim_dwork,
			   msecs_to_jiffies(next_delay_ms));
}

static int evonix_mm_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		cancel_delayed_work_sync(&evonix_reclaim_dwork);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		if (READ_ONCE(evonix_adaptive_reclaim))
			queue_delayed_work(system_freezable_power_efficient_wq,
					   &evonix_reclaim_dwork,
					   msecs_to_jiffies(1000));
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block evonix_mm_pm_nb = {
	.notifier_call = evonix_mm_pm_notify,
};

static struct ctl_table evonix_mm_table[] = {
	{
		.procname	= "evonix_adaptive_reclaim",
		.data		= &evonix_adaptive_reclaim,
		.maxlen		= sizeof(evonix_adaptive_reclaim),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "evonix_reclaim_cushion_mb",
		.data		= &evonix_reclaim_cushion_mb,
		.maxlen		= sizeof(evonix_reclaim_cushion_mb),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &cushion_min_mb,
		.extra2		= &cushion_max_mb,
	},
	{ }
};

static int __init evonix_mm_init(void)
{
	register_sysctl_init("vm", evonix_mm_table);
	INIT_DELAYED_WORK(&evonix_reclaim_dwork, evonix_reclaim_work_fn);
	register_pm_notifier(&evonix_mm_pm_nb);

	if (evonix_adaptive_reclaim)
		queue_delayed_work(system_freezable_power_efficient_wq, &evonix_reclaim_dwork,
				   msecs_to_jiffies(1000));
	pr_info("EVONIX: Adaptive Memory Reclaim Controller active (cushion=%d MB)\n",
		evonix_reclaim_cushion_mb);
	return 0;
}
late_initcall(evonix_mm_init);
