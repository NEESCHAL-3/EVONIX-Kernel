// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */
#ifndef __EZRERECLAIM_H
#define __EZRERECLAIM_H
/* ezreclaimd add here */
#include <linux/mm_inline.h>
#include <linux/sched/clock.h>

#include <trace/hooks/vmscan.h>
#include <trace/hooks/mm.h>
#include <trace/events/vmscan.h>

#include "../../mm/internal.h"

#include "../../mm_osvelte/common.h"
#include "../../mm_osvelte/mm-trace.h"

/* above all copy from hybridswap for compatiable */
#define ezr_lru(lruvec) (&(lruvec->lists[0]))
/******************************************************************************
 *                          log utils
 ******************************************************************************/
#define TAG "[EZR]"
#define EZR_LOG_LVL 2

enum {
	EZR_LOG_VERBOSE = 0,
	EZR_LOG_DEBUG,
	EZR_LOG_INFO,
	EZR_LOG_ERR,
};

static inline char ezr_loglvl_to_char(int l)
{
	switch (l) {
	case EZR_LOG_VERBOSE:
		return 'V';
	case EZR_LOG_INFO:
		return 'I';
	case EZR_LOG_DEBUG:
		return 'D';
	case EZR_LOG_ERR:
		return 'E';
	default:
		break;
	}
	return '?';
}

#define ezr_log(l, f, ...) do {						\
	if (l >= EZR_LOG_LVL) 						\
		printk(KERN_ERR "%s %5d %5d %c %-16s: %s:%d "f,	\
		       TAG, current->tgid, current->pid,		\
		       ezr_loglvl_to_char(l), current->comm, __func__,  \
		       __LINE__,  ##__VA_ARGS__);			\
} while (0)

#define ezr_loge(f, ...)						\
	ezr_log(EZR_LOG_ERR, f, ##__VA_ARGS__)
#define ezr_logi(f, ...)						\
	ezr_log(EZR_LOG_INFO, f, ##__VA_ARGS__)
#define ezr_logd(f, ...)						\
	ezr_log(EZR_LOG_DEBUG, f, ##__VA_ARGS__)

/******************************************************************************
 *                          extern symbols
 ******************************************************************************/
struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Scan pressure balancing between anon and file LRUs
	 */
	unsigned long	anon_cost;
	unsigned long	file_cost;

	/* Can active folios be deactivated as part of reclaim? */
#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;
	unsigned int skipped_deactivate:1;

	/* Writepage batching in laptop mode; RECLAIM_WRITE */
	unsigned int may_writepage:1;

	/* Can mapped folios be reclaimed? */
	unsigned int may_unmap:1;

	/* Can folios be swapped as part of reclaim? */
	unsigned int may_swap:1;

	/* Proactive reclaim invoked by userspace through memory.reclaim */
	unsigned int proactive:1;

	/*
	 * Cgroup memory below memory.low is protected as long as we
	 * don't threaten to OOM. If any cgroup is reclaimed at
	 * reduced force or passed over entirely due to its memory.low
	 * setting (memcg_low_skipped), and nothing is reclaimed as a
	 * result, then go back for one more cycle that reclaims the protected
	 * memory (memcg_low_reclaim) to avert OOM.
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* There is easily reclaimable cold cache in the current node */
	unsigned int cache_trim_mode:1;

	/* The file folios on the current node are dangerously low */
	unsigned int file_is_tiny:1;

	/* Always discard instead of demoting to lower tier memory */
	unsigned int no_demotion:1;

	/* Allocation order */
	s8 order;

	/* Scan (total_size >> priority) pages at once */
	s8 priority;

	/* The highest zone to isolate folios for reclaim from */
	s8 reclaim_idx;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	struct {
		unsigned int dirty;
		unsigned int unqueued_dirty;
		unsigned int congested;
		unsigned int writeback;
		unsigned int immediate;
		unsigned int file_taken;
		unsigned int taken;
	} nr;

	/* for recording the reclaimed slab by now */
	struct reclaim_state reclaim_state;

	ANDROID_VENDOR_DATA(1);
};

extern bool isolate_folio(struct lruvec *lruvec, struct folio *folio,
			  struct scan_control *sc);
extern unsigned long reclaim_pages(struct list_head *folio_list,
				   bool ignore_references);
extern unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
						  unsigned long nr_pages,
						  gfp_t gfp_mask,
						  unsigned int reclaim_options);
#endif /* __EZRERECLAIM_H */
