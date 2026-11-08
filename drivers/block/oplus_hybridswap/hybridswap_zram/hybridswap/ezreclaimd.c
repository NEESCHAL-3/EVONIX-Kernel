// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[HYB_ZRAM]" fmt

#include <uapi/linux/sched/types.h>
#include <linux/sched.h>
#include <linux/memory.h>
#include <linux/freezer.h>
#include <linux/swap.h>
#include <linux/cgroup-defs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/file.h>
#include <linux/memcontrol.h>

#include "../zram_drv.h"
#include "../zram_drv_internal.h"
#include "internal.h"
#include "hybridswap.h"

#include "ezreclaimd.h"

struct swapd_param {
	unsigned int min_score;
	unsigned int max_score;
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
};

#define HS_SWAP_ANON_REFAULT_THRESHOLD 22000
#define ANON_REFAULT_SNAPSHOT_MIN_INTERVAL 200
#define EMPTY_ROUND_SKIP_INTERNVAL 20
#define MAX_SKIP_INTERVAL 1000
#define EMPTY_ROUND_CHECK_THRESHOLD 10
#define ZRAM_WM_RATIO 75
#define COMPRESS_RATIO 30
#define SWAPD_MAX_LEVEL_NUM 10
#define SWAPD_DEFAULT_BIND_CPUS "0-3"
#define MAX_RECLAIMIN_SZ (200llu << 20)
#define page_to_kb(nr) (nr << (PAGE_SHIFT - 10))
#define SWAPD_SHRINK_WINDOW (HZ * 10)
#define SWAPD_SHRINK_SIZE_PER_WINDOW 1024
#define PAGES_TO_MB(pages) ((pages) >> 8)
#define PAGES_PER_1MB (1 << 8)

typedef bool (*free_swap_is_low_func)(void);
static free_swap_is_low_func free_swap_is_low_fp;

static atomic64_t zram_wm_ratio = ATOMIC_LONG_INIT(ZRAM_WM_RATIO);
static atomic64_t compress_ratio = ATOMIC_LONG_INIT(COMPRESS_RATIO);
static atomic_t avail_buffers = ATOMIC_INIT(0);
static atomic_t min_avail_buffers = ATOMIC_INIT(0);
static atomic_t high_avail_buffers = ATOMIC_INIT(0);
static atomic_t max_reclaim_size = ATOMIC_INIT(100);
static atomic64_t free_swap_threshold = ATOMIC64_INIT(0);
static atomic64_t zram_crit_thres = ATOMIC_LONG_INIT(0);
static atomic64_t cpuload_threshold = ATOMIC_LONG_INIT(0);
static atomic64_t hs_swap_anon_refault_threshold = ATOMIC_LONG_INIT(HS_SWAP_ANON_REFAULT_THRESHOLD);
static atomic64_t anon_refault_snapshot_min_interval = ATOMIC_LONG_INIT(ANON_REFAULT_SNAPSHOT_MIN_INTERVAL);
static atomic64_t empty_round_skip_interval = ATOMIC_LONG_INIT(EMPTY_ROUND_SKIP_INTERNVAL);
static atomic64_t max_skip_interval = ATOMIC_LONG_INIT(MAX_SKIP_INTERVAL);
static atomic64_t empty_round_check_threshold = ATOMIC_LONG_INIT(EMPTY_ROUND_CHECK_THRESHOLD);
static unsigned long all_totalreserve_pages;
static u64 zram_used_limit_pages;
static int ezr_min_avail_buffer, ezr_high_avail_buffer;

static struct swapd_param zswap_param[SWAPD_MAX_LEVEL_NUM];
static pid_t swapd_pid = -1;
static atomic_long_t fault_out_pause = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_out_pause_cnt = ATOMIC_LONG_INIT(0);

static atomic_t swapd_pause = ATOMIC_INIT(0);
static atomic_t swapd_enabled = ATOMIC_INIT(0);
static unsigned long swapd_nap_jiffies = 1;

static void wake_all_swapd(void);

#ifdef CONFIG_OPLUS_JANK
extern u32 get_cpu_load(u32 win_cnt, struct cpumask *mask);
#endif

static inline u64 get_compress_ratio_value(void)
{
	return atomic64_read(&compress_ratio);
}

static inline unsigned int get_avail_buffers_value(void)
{
	return atomic_read(&avail_buffers);
}

static inline unsigned int get_min_avail_buffers_value(void)
{
	/* workaround here, todo refactor this */
	if (ezr_min_avail_buffer)
		return ezr_min_avail_buffer;
	return atomic_read(&min_avail_buffers);
}

static inline unsigned int get_high_avail_buffers_value(void)
{
	if (ezr_high_avail_buffer)
		return ezr_high_avail_buffer;
	return atomic_read(&high_avail_buffers);
}

static inline u64 get_swapd_max_reclaim_size(void)
{
	return atomic_read(&max_reclaim_size);
}

static inline u64 get_free_swap_threshold_value(void)
{
	return atomic64_read(&free_swap_threshold);
}

static inline unsigned long long get_hs_swap_anon_refault_threshold_value(void)
{
	return atomic64_read(&hs_swap_anon_refault_threshold);
}

static inline unsigned long get_anon_refault_snapshot_min_interval_value(void)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}

static inline unsigned long long get_empty_round_skip_interval_value(void)
{
	return atomic64_read(&empty_round_skip_interval);
}

static inline unsigned long long get_max_skip_interval_value(void)
{
	return atomic64_read(&max_skip_interval);
}

static inline unsigned long long get_empty_round_check_threshold_value(void)
{
	return atomic64_read(&empty_round_check_threshold);
}

static inline u64 get_zram_critical_threshold_value(void)
{
	return atomic64_read(&zram_crit_thres);
}

static inline u64 get_cpuload_threshold_value(void)
{
	return atomic64_read(&cpuload_threshold);
}

static ssize_t avail_buffers_params_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	unsigned int avail_buffers_value;
	unsigned int min_avail_buffers_value;
	unsigned int high_avail_buffers_value;
	u64 free_swap_threshold_value;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u %llu",
				&avail_buffers_value,
				&min_avail_buffers_value,
				&high_avail_buffers_value,
				&free_swap_threshold_value) != 4)
		return -EINVAL;

	atomic_set(&avail_buffers, avail_buffers_value);
	atomic_set(&min_avail_buffers, min_avail_buffers_value);
	atomic_set(&high_avail_buffers, high_avail_buffers_value);
	atomic64_set(&free_swap_threshold,
			(free_swap_threshold_value * (SZ_1M / PAGE_SIZE)));

	return nbytes;
}

static int avail_buffers_params_show(struct seq_file *m, void *v)
{
	seq_printf(m, "avail_buffers: %u\n",
			atomic_read(&avail_buffers));
	seq_printf(m, "min_avail_buffers: %u\n",
			atomic_read(&min_avail_buffers));
	seq_printf(m, "high_avail_buffers: %u\n",
			atomic_read(&high_avail_buffers));
	seq_printf(m, "free_swap_threshold: %llu\n",
			(atomic64_read(&free_swap_threshold) * PAGE_SIZE / SZ_1M));

	return 0;
}

static ssize_t swapd_max_reclaim_size_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	const unsigned int base = 10;
	u32 max_reclaim_size_value;
	int ret;

	buf = strstrip(buf);
	ret = kstrtouint(buf, base, &max_reclaim_size_value);
	if (ret)
		return -EINVAL;

	atomic_set(&max_reclaim_size, max_reclaim_size_value);

	return nbytes;
}

static int swapd_max_reclaim_size_show(struct seq_file *m, void *v)
{
	seq_printf(m, "swapd_max_reclaim_size: %u\n",
			atomic_read(&max_reclaim_size));

	return 0;
}

static int hs_swap_anon_refault_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&hs_swap_anon_refault_threshold, val);

	return 0;
}

static s64 hs_swap_anon_refault_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&hs_swap_anon_refault_threshold);
}

static int empty_round_skip_interval_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_skip_interval, val);

	return 0;
}

static s64 empty_round_skip_interval_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&empty_round_skip_interval);
}

static int max_skip_interval_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&max_skip_interval, val);

	return 0;
}

static s64 max_skip_interval_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&max_skip_interval);
}

static int empty_round_check_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_check_threshold, val);

	return 0;
}

static s64 empty_round_check_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&empty_round_check_threshold);
}


static int anon_refault_snapshot_min_interval_write(
		struct cgroup_subsys_state *css, struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&anon_refault_snapshot_min_interval, val);

	return 0;
}

static s64 anon_refault_snapshot_min_interval_read(
		struct cgroup_subsys_state *css, struct cftype *cft)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}

static int zram_critical_thres_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&zram_crit_thres, val << (20 - PAGE_SHIFT));

	return 0;
}

static s64 zram_critical_thres_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&zram_crit_thres) >> (20 - PAGE_SHIFT);
}

static s64 cpuload_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)

{
	return atomic64_read(&cpuload_threshold);
}

static int cpuload_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&cpuload_threshold, val);

	return 0;
}

static s64 swapd_pid_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return swapd_pid;
}

static void swapd_memcgs_param_parse(int level_num)
{
	struct mem_cgroup *memcg = NULL;
	memcg_hybs_t *hybs = NULL;
	int i;

	while ((memcg = get_next_memcg(memcg))) {
		hybs = MEMCGRP_ITEM_DATA(memcg);

		for (i = 0; i < level_num; ++i) {
			if (atomic64_read(&hybs->app_score) >= zswap_param[i].min_score &&
					atomic64_read(&hybs->app_score) <= zswap_param[i].max_score)
				break;
		}
		atomic_set(&hybs->ub_mem2zram_ratio, zswap_param[i].ub_mem2zram_ratio);
		atomic_set(&hybs->ub_zram2ufs_ratio, zswap_param[i].ub_zram2ufs_ratio);
		atomic_set(&hybs->refault_threshold, zswap_param[i].refault_threshold);
	}
}

static int update_swapd_memcgs_param(char *buf)
{
	static const char delim[] = " ";
	char *token = NULL;
	int level_num;
	int i;

	buf = strstrip(buf);
	token = strsep(&buf, delim);

	if (!token)
		return -EINVAL;

	if (kstrtoint(token, 0, &level_num))
		return -EINVAL;

	if (level_num > SWAPD_MAX_LEVEL_NUM || level_num < 0)
		return -EINVAL;

	log_warn("%s\n", buf);

	mutex_lock(&reclaim_para_lock);
	for (i = 0; i < level_num; ++i) {
		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].min_score) ||
				zswap_param[i].min_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].max_score) ||
				zswap_param[i].max_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_mem2zram_ratio) ||
				zswap_param[i].ub_mem2zram_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_zram2ufs_ratio) ||
				zswap_param[i].ub_zram2ufs_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].refault_threshold))
			goto out;
	}

	swapd_memcgs_param_parse(level_num);
	mutex_unlock(&reclaim_para_lock);
	return 0;

out:
	mutex_unlock(&reclaim_para_lock);
	return -EINVAL;
}

static ssize_t swapd_memcgs_param_write(struct kernfs_open_file *of, char *buf,
		size_t nbytes, loff_t off)
{
	int ret = update_swapd_memcgs_param(buf);

	if (ret)
		return ret;

	return nbytes;
}

static int swapd_memcgs_param_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < SWAPD_MAX_LEVEL_NUM; ++i) {
		seq_printf(m, "level %d min score: %u\n",
				i, zswap_param[i].min_score);
		seq_printf(m, "level %d max score: %u\n",
				i, zswap_param[i].max_score);
		seq_printf(m, "level %d ub_mem2zram_ratio: %u\n",
				i, zswap_param[i].ub_mem2zram_ratio);
		seq_printf(m, "level %d ub_zram2ufs_ratio: %u\n",
				i, zswap_param[i].ub_zram2ufs_ratio);
		seq_printf(m, "memcg %d refault_threshold: %u\n",
				i, zswap_param[i].refault_threshold);
	}

	return 0;
}

static ssize_t swapd_nap_jiffies_write(struct kernfs_open_file *of, char *buf,
		size_t nbytes, loff_t off)
{
	unsigned long nap;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	if (kstrtoul(buf, 0, &nap))
		return -EINVAL;

	swapd_nap_jiffies = nap;
	return nbytes;
}

static int swapd_nap_jiffies_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%lu\n", swapd_nap_jiffies);

	return 0;
}

static ssize_t swapd_single_memcg_param_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u", &ub_mem2zram_ratio, &ub_zram2ufs_ratio,
				&refault_threshold) != 3)
		return -EINVAL;

	if (ub_mem2zram_ratio > MAX_RATIO || ub_zram2ufs_ratio > MAX_RATIO)
		return -EINVAL;

	log_warn("%u %u %u\n",
	     ub_mem2zram_ratio, ub_zram2ufs_ratio, refault_threshold);

	atomic_set(&MEMCGRP_ITEM(memcg, ub_mem2zram_ratio), ub_mem2zram_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, ub_zram2ufs_ratio), ub_zram2ufs_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, refault_threshold), refault_threshold);

	return nbytes;
}


static int swapd_single_memcg_param_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	seq_printf(m, "memcg score: %lld\n",
			atomic64_read(&hybs->app_score));
	seq_printf(m, "memcg ub_mem2zram_ratio: %u\n",
			atomic_read(&hybs->ub_mem2zram_ratio));
	seq_printf(m, "memcg ub_zram2ufs_ratio: %u\n",
			atomic_read(&hybs->ub_zram2ufs_ratio));
	seq_printf(m, "memcg refault_threshold: %u\n",
			atomic_read(&hybs->refault_threshold));

	return 0;
}

static int mem_cgroup_zram_wm_ratio_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&zram_wm_ratio, val);

	return 0;
}

static s64 mem_cgroup_zram_wm_ratio_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&zram_wm_ratio);
}

static int mem_cgroup_compress_ratio_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&compress_ratio, val);

	return 0;
}

static s64 mem_cgroup_compress_ratio_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&compress_ratio);
}

static int memcg_active_app_info_list_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long anon_size;
	unsigned long zram_size;
	unsigned long eswap_size;

	while ((memcg = get_next_memcg(memcg))) {
		u64 score;

		if (!MEMCGRP_ITEM_DATA(memcg))
			continue;

		score = atomic64_read(&MEMCGRP_ITEM(memcg, app_score));
		anon_size = memcg_anon_pages(memcg);
		eswap_size = hybridswap_read_memcg_stats(memcg,
				MCG_DISK_STORED_PG_SZ);
		zram_size = hybridswap_read_memcg_stats(memcg,
				MCG_ZRAM_STORED_PG_SZ);

		if (anon_size + zram_size + eswap_size == 0)
			continue;

		if (!strlen(MEMCGRP_ITEM(memcg, name)))
			continue;

		anon_size *= PAGE_SIZE / SZ_1K;
		zram_size *= PAGE_SIZE / SZ_1K;
		eswap_size *= PAGE_SIZE / SZ_1K;

		seq_printf(m, "%s %llu %lu %lu %lu %llu\n",
				MEMCGRP_ITEM(memcg, name), score,
				anon_size, zram_size, eswap_size,
				MEMCGRP_ITEM(memcg, reclaimed_pagefault));
	}
	return 0;
}

static unsigned long get_totalreserve_pages(void)
{
	int nid;
	unsigned long val = 0;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (pgdat)
			val += pgdat->totalreserve_pages;
	}

	return val;
}

static unsigned int system_cur_avail_buffers(void)
{
	return si_mem_available() >> 8;
}

static bool min_buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_min_avail_buffers_value())
		return true;

	return false;
}

static bool high_buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_high_avail_buffers_value())
		return true;

	return false;
}

static int zram_used_limit_mb_write(struct cgroup_subsys_state *css,
				    struct cftype *cft, s64 val)
{
	zram_used_limit_pages = (val << 20) >> PAGE_SHIFT;
	return 0;
}

static s64 zram_used_limit_mb_read(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return (zram_used_limit_pages << PAGE_SHIFT) >> 20;
}

static ssize_t ezr_avail_buffer_write(struct kernfs_open_file *of, char *buf,
				       size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);
	int min, high;

	if (!hybs)
		return -EINVAL;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u", &min, &high) != 2)
		return -EINVAL;
	if (high - min < 64)
		return -EINVAL;
	ezr_min_avail_buffer = min;
	ezr_high_avail_buffer = high;
	return nbytes;
}

static int ezr_avail_buffer_read(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;
	seq_printf(m, "[%d-%d]\n", ezr_min_avail_buffer, ezr_high_avail_buffer);
	return 0;
}

static struct cftype mem_cgroup_swapd_legacy_files[] = {
	{
		.name = "active_app_info_list",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_active_app_info_list_show,
	},
	{
		.name = "zram_wm_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_zram_wm_ratio_write,
		.read_s64 = mem_cgroup_zram_wm_ratio_read,
	},
	{
		.name = "compress_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_compress_ratio_write,
		.read_s64 = mem_cgroup_compress_ratio_read,
	},
	{
		.name = "swapd_pid",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_s64 = swapd_pid_read,
	},
	{
		.name = "avail_buffers",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = avail_buffers_params_write,
		.seq_show = avail_buffers_params_show,
	},
	{
		.name = "swapd_max_reclaim_size",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_max_reclaim_size_write,
		.seq_show = swapd_max_reclaim_size_show,
	},
	{
		.name = "area_anon_refault_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = hs_swap_anon_refault_threshold_write,
		.read_s64 = hs_swap_anon_refault_threshold_read,
	},
	{
		.name = "empty_round_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_skip_interval_write,
		.read_s64 = empty_round_skip_interval_read,
	},
	{
		.name = "max_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = max_skip_interval_write,
		.read_s64 = max_skip_interval_read,
	},
	{
		.name = "empty_round_check_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_check_threshold_write,
		.read_s64 = empty_round_check_threshold_read,
	},
	{
		.name = "anon_refault_snapshot_min_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = anon_refault_snapshot_min_interval_write,
		.read_s64 = anon_refault_snapshot_min_interval_read,
	},
	{
		.name = "swapd_memcgs_param",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_memcgs_param_write,
		.seq_show = swapd_memcgs_param_show,
	},
	{
		.name = "swapd_single_memcg_param",
		.write = swapd_single_memcg_param_write,
		.seq_show = swapd_single_memcg_param_show,
	},
	{
		.name = "zram_critical_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_critical_thres_write,
		.read_s64 = zram_critical_thres_read,
	},
	{
		.name = "cpuload_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = cpuload_threshold_write,
		.read_s64 = cpuload_threshold_read,
	},
	{
		.name = "swapd_nap_jiffies",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_nap_jiffies_write,
		.seq_show = swapd_nap_jiffies_show,
	},
	{
		.name = "zram_used_limit_mb",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_used_limit_mb_write,
		.read_s64 = zram_used_limit_mb_read,
	},
	{
		.name = "ezr_avail_buffer",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = ezr_avail_buffer_write,
		.seq_show = ezr_avail_buffer_read,
	},
	{ }, /* terminate */
};

#define INC_EXTRA_ZRAM_RATIO (2)
static unsigned long get_nr_zram_increase(void)
{
	if (unlikely(!swapd_zram))
		return 0;

	return swapd_zram->increase_nr_pages;
}

static unsigned long zram_used_pages(void)
{
	if (unlikely(!swapd_zram))
		return 0;

	return (u64)atomic64_read(&swapd_zram->stats.pages_stored);
}

/*
 * add extra zram_increase / INC_EXTRA_ZRAM_RATIO to zram, same
 * pages do not occupy physical memory
 * */
static unsigned long get_nr_zram_total(void)
{
	unsigned long nr_zram = 1;

	if (!swapd_zram)
		return nr_zram;

	if (zram_used_limit_pages)
		return zram_used_limit_pages;

	nr_zram = swapd_zram->disksize >> PAGE_SHIFT;
#if (defined CONFIG_ZRAM_WRITEBACK) || (defined CONFIG_HYBRIDSWAP_CORE)
	nr_zram -= (get_nr_zram_increase() / INC_EXTRA_ZRAM_RATIO);
#endif
	return nr_zram ?: 1;
}

static bool zram_watermark_ok(void)
{
	long long diff_buffers;
	long long wm = 0;
	long long cur_ratio = 0;
	unsigned long zram_used = zram_used_pages();
	const unsigned int percent_constant = 100;

	diff_buffers = get_high_avail_buffers_value() -
		system_cur_avail_buffers();
	diff_buffers *= SZ_1M / PAGE_SIZE;
	diff_buffers *= get_compress_ratio_value() / 10;
	diff_buffers = diff_buffers * percent_constant / get_nr_zram_total();

	cur_ratio = zram_used * percent_constant / get_nr_zram_total();
	wm  = min(get_zram_wm_ratio_value(), get_zram_wm_ratio_value() - diff_buffers);

	return cur_ratio > wm;
}

static inline bool zram_is_full(void)
{
	return zram_used_pages() >= get_nr_zram_total();
}

static void wake_all_swapd(void) { }

static bool free_swap_is_low(void)
{
	struct sysinfo info;

	si_swapinfo(&info);

	return (info.freeswap < get_free_swap_threshold_value());
}

static bool hybridswap_swapd_enabled(void)
{
	return !!atomic_read(&swapd_enabled);
}

static int ezreclaimable_init(void);
static int swapd_pre_init(void)
{
	int ret;
	free_swap_is_low_fp = free_swap_is_low;
	all_totalreserve_pages = get_totalreserve_pages();

	if (hybridswap_swapd_enabled())
		return -EINVAL;

	ret = ezreclaimable_init();
	/* this used by zram meminfo */
	if (!ret)
		atomic_set(&swapd_enabled, 1);
	return ret;
}

static void swapd_pre_deinit(void) {}

static void vh_alloc_pages_slowpath(void *data, gfp_t gfp_flags,
				    unsigned int order, unsigned long delta)
{
}

static void vh_tune_scan_type(void *data, enum scan_balance *s_balance)
{
}

static int swapd_init(struct zram **zram)
{
	/*
	 * register_panel_event_notifier should be called after boot completed
	 */
	register_panel_event_notifier();
	return 0;
}

static void swapd_exit(void)
{
	ezr_loge("unsupport for ezr\n");
}

void ezr_ops_init(struct hybridswapd_operations *ops)
{
	ops->fault_out_pause = &fault_out_pause;
	ops->fault_out_pause_cnt = &fault_out_pause_cnt;
	ops->swapd_pause = &swapd_pause;

	ops->memcg_legacy_files = mem_cgroup_swapd_legacy_files;
	ops->update_memcg_param = update_swapd_memcg_param;

	ops->pre_init = swapd_pre_init;
	ops->pre_deinit = swapd_pre_deinit;

	ops->init = swapd_init;
	ops->deinit = swapd_exit;
	ops->enabled = hybridswap_swapd_enabled;

	ops->free_zram_is_ok = free_zram_is_ok;
	ops->zram_watermark_ok = zram_watermark_ok;
	ops->zram_total_pages = get_nr_zram_total;
	ops->wakeup_kthreads = wake_all_swapd;

	ops->vh_alloc_pages_slowpath = vh_alloc_pages_slowpath;
	ops->vh_tune_scan_type = vh_tune_scan_type;
	ezr_logi("+\n");
}

/******************************************************************************
 *                          ezreclaimd module
 ******************************************************************************/
#define DEFAULT_MIN_BATCH_MB (8)
#define LOOP_BATCH_MB (64)
#define DEFAULT_THRASHING_LIMIT_PCT (80)

#if PAGE_SHIFT < 20
#define M2P(mb)	((mb) << (20 - PAGE_SHIFT))
#else
#define M2P(mb)	((mb) >> (PAGE_SHIFT - 20))
#endif

enum ezr_stat_item {
	EZR_SLEEP_DISPLAY_OFF,
	EZR_SLEEP_CAMERA,
	/* available */
	EZR_STOP_SWAPD_PAUSE,
	EZR_STOP_ANON_THRASHING,
	EZR_STOP_LOW_ANON,
	EZR_STOP_LOW_SWAP,
	EZR_STOP_AVAILABLE_OK,
	EZR_STOP_MIN_BUFFER_OK,
	/* demote */
	EZR_STOP_FILE_THRASHING,
	EZR_STOP_LOW_FILE,
	EZR_STOP_DEMOTE_OK,
	/* reclaim */
	EZR_RC_DIRECT,
	EZR_RC_KSWAPD,
	EZR_RC_SLOW,
	EZR_RC_FAILED0,
	EZR_RC_FAILED_ORDER,
	/* misc */
	EZR_SHRINK_IGNORE_FOLIOS,
	NR_MAX_EZR_STAT,
	/* do not use */
	EZR_STOP_UNKNOWN,
};

static const char *ezr_stat_item_txt[NR_MAX_EZR_STAT] = {
	"sl_display_off",
	"sl_camera",
	/* available */
	"st_pasue",
	"sl_anon_thrashing",
	"st_low_anon",
	"st_low_swap",
	"st_avail_ok",
	"st_min_ok",
	/* demote */
	"sl_file_thrashing",
	"st_low_file",
	"st_demote_ok",
	/* reclaim */
	"rc_direct",
	"rc_kswapd",
	"rc_slow",
	"rcf_order0",
	"rcf_order",
	/* misc */
	"shrink_ignore",
};

enum wakeup_event_item {
	WAKEUP_AVAILABLE = 1,
	WAKEUP_DEMOTE
};

struct ezr_struct {
	unsigned int min_batch;

	/* thrashing control */
	unsigned long last_demote_cnt;
	int wake_flags;

	unsigned long window_sz_hz;
	unsigned int thrashing_limit_pct;
	unsigned long jiffies_file_thrashing;
	unsigned long jiffies_anon_thrashing;
	unsigned int thrashing_file_pct, thrashing_anon_pct;
	unsigned long last_file_lru, last_anon_lru;
	unsigned long last_ws_refault_file, last_ws_refault_anon;

	struct timer_list refault_timer;
	struct wait_queue_head ezreclaimd_wait;
	struct task_struct *ezreclaimd_task;

	atomic_t stat_items[NR_MAX_EZR_STAT];
};

enum {
	EZR_WM_DIRECT_RECLAIM,
	EZR_WM_MIN, /* reserved for camera */
	NR_EZR_WMARKS
};

enum {
	EZR_RECLAIM_KSWAPD,
	EZR_RECLAIM_DIRECT_RECLAIM,
	EZR_RECLAIM_ALL,
};

/*
 * Precision of these counts is not a concern, so atomic operations
 * are not used
 */
static unsigned long ezreclaimable_promote_cnt;
static unsigned long ezreclaimable_demote_cnt;
static unsigned long ezreclaimable_reclaim_cnt;

static unsigned long ezr_wmarks[NR_EZR_WMARKS];
static unsigned int ezr_reclaim_timeout = HZ / 2;

atomic_t ezreclaimable_nr = ATOMIC_INIT(0);

static void wakeup_ezreclaimd(void);

/******************************************************************************
 *                          kprobe
 ******************************************************************************/
typedef struct mem_cgroup *(*mem_cgroup_iter_t)(struct mem_cgroup *root,
						struct mem_cgroup *prev,
						struct mem_cgroup_reclaim_cookie *reclaim);
static mem_cgroup_iter_t mem_cgroup_iter_dup;
typedef void (*mem_cgroup_iter_break_t)(struct mem_cgroup *root, struct mem_cgroup *prev);
static mem_cgroup_iter_break_t mem_cgroup_iter_break_dup;

/******************************************************************************
 *                          struct initialize
 ******************************************************************************/
static struct ezr_struct g_ezr = {
	.thrashing_limit_pct	= DEFAULT_THRASHING_LIMIT_PCT,
	.min_batch		= (DEFAULT_MIN_BATCH_MB * SZ_1M) / PAGE_SIZE,
	.ezreclaimd_wait	= __WAIT_QUEUE_HEAD_INITIALIZER(g_ezr.ezreclaimd_wait),
};

/******************************************************************************
 *                          stats counter
 ******************************************************************************/
static void inc_ezr_event(enum ezr_stat_item i)
{
	atomic_add(1, g_ezr.stat_items + i);
}

/******************************************************************************
 *                          ezreclaim folio flag
 ******************************************************************************/
static inline bool folio_test_ezreclaimable(struct folio *folio)
{
	return test_bit(PG_ezreclaimable, folio_flags(folio, 0));
}

static __always_inline void folio_mark_ezreclaimable(struct folio *folio)
{
	set_bit(PG_ezreclaimable, folio_flags(folio, 0));
}

static __always_inline void folio_clear_ezreclaimable(struct folio *folio)
{
	clear_bit(PG_ezreclaimable, folio_flags(folio, 0));
}

/******************************************************************************
 *                          demote
 *      mark_demote_current
 *		try_to_free_pages (filter folio)
 *			evict_folios
 *			__remove_mapping (keep_reclaimed_folio)
 *				list_for_each_entry_safe_reverse
 *					move_folios_to_lru (lru_gen_add_folio)
 *
 ******************************************************************************/
static inline bool current_is_demote_files(void)
{
	struct ezr_struct *ezrs = &g_ezr;

	return current == ezrs->ezreclaimd_task;
}

static void ezr_vh_shrink_folio_list(void *data, struct folio *folio, bool dirty,
				     bool writeback, bool *activate, bool *keep)
{
	if (!current_is_demote_files())
		return;

	if (writeback || dirty || folio_nr_pages(folio) != 1)
		goto keep_folio;

	if (folio_zonenum(folio) != ZONE_NORMAL ||
		get_pageblock_migratetype(&folio->page) == MIGRATE_CMA)
		goto keep_folio;

	return;
keep_folio:
	/* only one task can do this. safe now. */
	*keep = true;
	inc_ezr_event(EZR_SHRINK_IGNORE_FOLIOS);
}

static void ezr_vh_keep_reclaimed_folio(void *data, struct folio *folio, int refcount, bool *keep)
{
	if (current_is_demote_files() && !folio_test_anon(folio) &&
	    !folio_test_swapbacked(folio) && !folio_test_ezreclaimable(folio)) {
		folio_mark_ezreclaimable(folio);
		folio_ref_unfreeze(folio, refcount);
		*keep = true;
	}
}

/* list_for_each_entry_safe_reverse */
static void ezr_vh_evict_folios_bypass(void *data, struct folio *folio, bool *bypass)
{
	if (folio_test_ezreclaimable(folio))
		*bypass = true;
}

static inline bool ezreclaimable_add_folio(struct lruvec *lruvec, struct folio *folio)
{
	/* we abuse active/inactive lists as ezreclaimable list */
	struct list_head *head = &lruvec->lists[0];

	if (!folio_test_ezreclaimable(folio))
		return false;
	if (folio_test_unevictable(folio) || folio_test_active(folio)) {
		folio_clear_ezreclaimable(folio);
		return false;
	}

	ezreclaimable_demote_cnt++;
	list_add(&folio->lru, head);
	atomic_add(folio_nr_pages(folio), &ezreclaimable_nr);
	return true;
}

static void ezr_vh_lru_gen_add_folio_skip(void *data, struct lruvec *lruvec,
					  struct folio *folio, bool *skip)
{
	if (ezreclaimable_add_folio(lruvec, folio))
		*skip = true;
}

/******************************************************************************
 *                          promote
 *      __filemap_get_folio
 *      filemap_map_pages (fault_around)
 ******************************************************************************/

/* not export by kernel, just copy from memcontrol.c without lruvec_memcg_debug */
struct lruvec *folio_lruvec_lock_irqsave_dup(struct folio *folio,
		unsigned long *flags)
{
	struct lruvec *lruvec = folio_lruvec(folio);

	spin_lock_irqsave(&lruvec->lru_lock, *flags);
	return lruvec;
}

static inline void unlock_page_lruvec_irqrestore_dup(struct lruvec *lruvec,
		unsigned long flags)
{
	spin_unlock_irqrestore(&lruvec->lru_lock, flags);
}

static inline bool folio_ezreclaimable_promote(struct folio *folio)
{
	struct lruvec *locked_lruvec;
	unsigned long flags;

	if (!folio_test_ezreclaimable(folio))
		return false;

	locked_lruvec = folio_lruvec_lock_irqsave_dup(folio, &flags);
	if (folio_test_lru(folio)) {
		lruvec_del_folio(locked_lruvec, folio);
		folio_clear_ezreclaimable(folio);
		lruvec_add_folio(locked_lruvec, folio);
	} else {
		folio_clear_ezreclaimable(folio);
	}
	unlock_page_lruvec_irqrestore_dup(locked_lruvec, flags);

	ezreclaimable_promote_cnt++;
	return true;
}

static void ezr_vh_filemap_get_folio(void *data, struct address_space *mapping,
				     pgoff_t index, int fgp_flags, gfp_t gfp_mask,
				     struct folio *folio)
{
	if (!folio)
		return;

	folio_ezreclaimable_promote(folio);
}

static void ezr_vh_filemap_pages(void *data, struct folio *folio)
{
	folio_ezreclaimable_promote(folio);
}

/******************************************************************************
 *                          migrate
 *      set newfolio ezr flag
 ******************************************************************************/
static void ezr_vh_look_around_migrate_folio(void *data, struct folio *folio,
					     struct folio *newfolio)
{
	if (folio_test_ezreclaimable(folio))
		folio_mark_ezreclaimable(newfolio);
}

/******************************************************************************
 *                          ezreclaimd
 *	tune swappiness
 *      refault timer & mglru workingset refault (get_type_to_scan_dup)
 *
 ******************************************************************************/
static inline bool inactive_file_is_low(void)
{
	return global_node_page_state(NR_INACTIVE_FILE) < totalram_pages() / 16;
}

static inline bool inactive_anon_is_low(void)
{
	return global_node_page_state(NR_INACTIVE_ANON) < totalram_pages() / 16;
}

static bool ezr_need_refill(void)
{
	long delta;
	struct ezr_struct *ezrs = &g_ezr;

	delta = ezr_wmarks[EZR_WM_DIRECT_RECLAIM] - atomic_read(&ezreclaimable_nr);
	return delta > ezrs->min_batch;
}

static void ezr_vh_tune_swappiness(void *data, int *swappiness)
{
	struct ezr_struct *ezrs = &g_ezr;

	if (!current_is_demote_files())
		return;

	if (ezrs->wake_flags == WAKEUP_AVAILABLE) {
		if (ezr_need_refill() && !inactive_file_is_low()) {
			*swappiness = 180;
			return;
		}
		*swappiness = 200;
		return;
	}
	*swappiness = 2;
}

static void refault_poll_timer_fn(struct timer_list *unused)
{
	struct ezr_struct *data = &g_ezr;
	unsigned long lru, ws;

	/* calc thrash file */
	lru = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);
	ws = global_node_page_state_pages(WORKINGSET_REFAULT_FILE);
	data->thrashing_file_pct = (ws - data->last_ws_refault_file) * 100 /
		(data->last_file_lru + 1);
	data->last_ws_refault_file = ws;
	data->last_file_lru = lru;

	/* calc thrash anon */
	lru = global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_ANON);
	ws = global_node_page_state_pages(WORKINGSET_REFAULT_ANON);
	data->thrashing_anon_pct = (ws - data->last_ws_refault_anon) * 100 /
		(data->last_anon_lru + 1);
	data->last_ws_refault_anon = ws;
	data->last_anon_lru = lru;

	if (data->thrashing_file_pct > 10 || data->thrashing_anon_pct > 10)
		ezr_logi("thrashing_file_pct: %d thrashing_anon_pct: %d\n",
			 data->thrashing_file_pct, data->thrashing_anon_pct);

	if (data->thrashing_file_pct > data->thrashing_limit_pct)
		data->jiffies_file_thrashing = jiffies + 2 * data->window_sz_hz;
	if (data->thrashing_anon_pct > data->thrashing_limit_pct)
		data->jiffies_anon_thrashing = jiffies + 2 * data->window_sz_hz;
	wakeup_ezreclaimd();
	mod_timer(&data->refault_timer, jiffies + data->window_sz_hz);
}

static void ezr_refault_timer_init(unsigned long window_sz_hz)
{
	struct ezr_struct *data = &g_ezr;

	timer_setup(&data->refault_timer, refault_poll_timer_fn, 0);
	mod_timer(&data->refault_timer, jiffies + data->window_sz_hz);
	ezr_logi("timer enable, window_size: %lu\n", data->window_sz_hz);
}

struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static bool positive_ctrl_err_dup(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Return true if the PV has a limited number of refaults or a lower
	 * refaulted/total than the SP.
	 */
	return pv->refaulted < MIN_LRU_BATCH ||
	       pv->refaulted * (sp->total + MIN_LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}

static void read_ctrl_pos_dup(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->refaulted = lrugen->avg_refaulted[type][tier] +
			 atomic_long_read(&lrugen->refaulted[hist][type][tier]);
	pos->total = lrugen->avg_total[type][tier] +
		     atomic_long_read(&lrugen->evicted[hist][type][tier]);
	if (tier)
		pos->total += lrugen->protected[hist][type][tier - 1];
	pos->gain = gain;
}

static int get_type_to_scan_dup(struct lruvec *lruvec, int swappiness)
{
	struct ctrl_pos sp, pv;
	int gain[ANON_AND_FILE] = { swappiness, 200 - swappiness };

	/*
	 * Compare the first tier of anon with that of file to determine which
	 * type to scan. Also need to compare other tiers of the selected type
	 * with the first tier of the other type to determine the last tier (of
	 * the selected type) to evict.
	 */
	read_ctrl_pos_dup(lruvec, LRU_GEN_ANON, 0, gain[LRU_GEN_ANON], &sp);
	read_ctrl_pos_dup(lruvec, LRU_GEN_FILE, 0, gain[LRU_GEN_FILE], &pv);
	return positive_ctrl_err_dup(&sp, &pv);
}

static bool ezreclaimd_should_sleep(void)
{
	if (atomic_read(&display_off)) {
		inc_ezr_event(EZR_SLEEP_DISPLAY_OFF);
		return true;
	}

	if (osvelte_test_scene(MM_SCENE_CAMERA)) {
		inc_ezr_event(EZR_SLEEP_CAMERA);
		return true;
	}
	return false;
}

static int read_system_state(int flags, bool ttwu)
{
	struct ezr_struct *ezrs = &g_ezr;

	if (flags == WAKEUP_AVAILABLE) {
		if (atomic_read(&swapd_pause))
			return EZR_STOP_SWAPD_PAUSE;
		if (time_before(jiffies, ezrs->jiffies_anon_thrashing))
			return EZR_STOP_ANON_THRASHING;
		if (inactive_anon_is_low())
			return EZR_STOP_LOW_ANON;
		if (!free_zram_is_ok())
			return EZR_STOP_LOW_SWAP;
		if (high_buffer_is_suitable())
			return EZR_STOP_AVAILABLE_OK;
		/* TTWU check min_buffer_is_suitable */
		if (!ttwu)
			return 0;
		if (ttwu && !min_buffer_is_suitable())
			return 0;
		return EZR_STOP_UNKNOWN;
	}

	if (flags == WAKEUP_DEMOTE) {
		if (time_before(jiffies, ezrs->jiffies_file_thrashing))
			return EZR_STOP_FILE_THRASHING;
		if (inactive_file_is_low())
			return EZR_STOP_LOW_FILE;
		if (!ezr_need_refill())
			return EZR_STOP_DEMOTE_OK;
		return 0;
	}
	return EZR_STOP_UNKNOWN;
}

static void wakeup_ezreclaimd(void)
{
	struct ezr_struct *ezrs = &g_ezr;
	int flags;
	int ret;

	if (!ezrs->ezreclaimd_task)
		return;

	/* if ezreclaimd running, return */
	if (!waitqueue_active(&ezrs->ezreclaimd_wait))
		return;

	if (ezreclaimd_should_sleep())
		return;

	/* first check mem_availiable  */
	for (flags = WAKEUP_AVAILABLE; flags <= WAKEUP_DEMOTE; flags++) {
		ret = read_system_state(flags, true);
		if (!ret)
			break;
		if (ret != EZR_STOP_UNKNOWN)
			inc_ezr_event(ret);
	}
	/* system state not good, just return */
	if (ret)
		return;
	/* wakeup */
	ezrs->wake_flags = flags;
	wake_up_interruptible(&ezrs->ezreclaimd_wait);
}

static int shrink_ezr_folios(struct scan_control *sc);
static int ezreclaimd(void *p)
{
	struct task_struct *tsk = current;
	struct ezr_struct *ezrs = &g_ezr;
	int retries = 0, max_retries, flag, min_batch;
	int ret;
	unsigned long nr;
	unsigned long start_demote, reclaimed;
	unsigned long start_js, reclaim_jiffies;

	tsk->flags |= PF_MEMALLOC;
	set_freezable();

	while (true) {
		wait_event_freezable(ezrs->ezreclaimd_wait, ezrs->wake_flags);

		/* initlialize */
		flag = ezrs->wake_flags;
		retries = 0;
		reclaimed = 0;
		start_js = jiffies;
		start_demote = ezreclaimable_demote_cnt;
		min_batch = ezrs->min_batch;
		ret = -1;
		if (flag == WAKEUP_AVAILABLE)
			max_retries = M2P(get_high_avail_buffers_value() -
					  get_min_avail_buffers_value()) / min_batch;
		else
			max_retries = M2P(LOOP_BATCH_MB) / min_batch;

		mm_trace_fmt_begin("%d,%d,%d", flag, max_retries,
				   atomic_read(&ezreclaimable_nr));
start_over:
		if (ezreclaimd_should_sleep())
			goto out;

		ret = read_system_state(flag, false);
		if (ret) {
			if (ret != EZR_STOP_UNKNOWN)
				inc_ezr_event(ret);
			goto out;
		}

		/* used by abort scan */
		ezrs->last_demote_cnt = ezreclaimable_demote_cnt;
		nr = try_to_free_mem_cgroup_pages(root_mem_cgroup, min_batch, GFP_KERNEL,
						  MEMCG_RECLAIM_MAY_SWAP | MEMCG_RECLAIM_PROACTIVE);
		reclaimed += nr;

		if (time_after_eq(jiffies, start_js + ezr_reclaim_timeout)) {
			inc_ezr_event(EZR_RC_SLOW);
			goto out;
		}

		if (retries++ < max_retries)
			goto start_over;
out:
		mm_trace_fmt_end();
		reclaim_jiffies = jiffies - start_js;
		if (flag == WAKEUP_AVAILABLE)
			ezr_logi("wake_flag:1 retries:[%d-%d] avail:%d wm:[%d-%d] ret:%d demote:%lu reclaimed:%lu ezr:%d dur:%u\n",
				 retries, max_retries,
				 system_cur_avail_buffers(),
				 get_min_avail_buffers_value(),
				 get_high_avail_buffers_value(), ret,
				 ezreclaimable_demote_cnt - start_demote,
				 reclaimed, atomic_read(&ezreclaimable_nr),
				 jiffies_to_msecs(reclaim_jiffies));
		else
			ezr_logi("wake_flag:2 retries:[%d-%d] ret:%d demote:%lu reclaimed:%lu ezr:%d dur:%u\n",
				 retries, max_retries, ret,
				 ezreclaimable_demote_cnt - start_demote,
				 reclaimed, atomic_read(&ezreclaimable_nr),
				 jiffies_to_msecs(reclaim_jiffies));

		/* recalim : sleep = 1 : 1 */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(reclaim_jiffies);
		ezrs->wake_flags = 0;
	}
	return 0;
}

/******************************************************************************
 *                          ezr memory reclaim
 *      lru_gen_del_folio
 *		clear_reclaimed_folio
 *			isolate_ezr_folios & reclaim_pages
 *				iter_memcgs
 *					shrink_ezr_folios
 ******************************************************************************/
static inline bool ezreclaimable_del_folio(struct lruvec *lruvec, struct folio *folio)
{
	if (!folio_test_ezreclaimable(folio))
		return false;

	list_del(&folio->lru);
	atomic_add(-folio_nr_pages(folio), &ezreclaimable_nr);
	return true;
}

static void ezr_vh_lru_gen_del_folio_skip(void *data, struct lruvec *lruvec,
					    struct folio *folio, bool *skip)
{
	if (ezreclaimable_del_folio(lruvec, folio))
		*skip = true;
}

static void ezr_vh_clear_reclaimed_folio(void *data, struct folio *folio, bool reclaimed)
{
	if (reclaimed && folio_test_ezreclaimable(folio)) {
		ezreclaimable_reclaim_cnt++;
		folio_clear_ezreclaimable(folio);
	}
}

static bool sort_ezreclaimable_folio(struct lruvec *lruvec, struct folio *folio)
{
	int delta = folio_nr_pages(folio);
	bool success;

	if (folio_evictable(folio))
		return false;

	folio_clear_ezreclaimable(folio);
	success = lru_gen_del_folio(lruvec, folio, true);
	VM_WARN_ON_ONCE_FOLIO(!success, folio);
	folio_set_unevictable(folio);
	lruvec_add_folio(lruvec, folio);
	__count_vm_events(UNEVICTABLE_PGCULLED, delta);
	return true;
}

static void isolate_ezr_folios(struct scan_control *sc,
			       struct lruvec *lruvec,
			       struct list_head *list)
{
	int skipped = 0;
	int isolated = 0;
	int sorted = 0;
	int remaining = sc->nr_to_reclaim;
	struct list_head *head = ezr_lru(lruvec);
	int scanned = 0;
	LIST_HEAD(moved);

	while (!list_empty(head)) {
		struct folio *folio = lru_to_folio(head);
		int delta = folio_nr_pages(folio);

		VM_WARN_ON_ONCE_FOLIO(folio_test_unevictable(folio), folio);
		VM_WARN_ON_ONCE_FOLIO(folio_test_active(folio), folio);

		scanned += delta;

		if (sort_ezreclaimable_folio(lruvec, folio))
			sorted += delta;
		else if (isolate_folio(lruvec, folio, sc)) {
			list_add(&folio->lru, list);
			isolated += delta;
		} else {
			list_move(&folio->lru, &moved);
			skipped += delta;
		}

		/* whether we need remaining here? */
		if (!--remaining || isolated >= sc->nr_to_reclaim)
			break;
	}
	sc->nr_scanned += scanned;

	if (skipped)
		list_splice(&moved, head);
}

static int iter_memcg_callback(struct mem_cgroup *memcg,
			       struct lruvec *lruvec, void *private)
{
	struct scan_control *sc = (struct scan_control *)private;
	int mode = sc->android_vendor_data1;
	LIST_HEAD(list);

	if (mode == EZR_RECLAIM_KSWAPD) {
		/* do nothing */
	} else if (mode == EZR_RECLAIM_DIRECT_RECLAIM) {
		if (get_type_to_scan_dup(lruvec, 7) == LRU_GEN_ANON)
			return 0;
	}

	spin_lock_irq(&lruvec->lru_lock);
	isolate_ezr_folios(sc, lruvec, &list);
	spin_unlock_irq(&lruvec->lru_lock);

	sc->nr_reclaimed += reclaim_pages(&list, true);
	/* abort reclaim */
	if (sc->nr_reclaimed >= sc->nr_to_reclaim)
		return 1;
	return 0;
}

void do_iter_mem_cgroups_lruvec(int (*cb)(struct mem_cgroup *memcg,
					  struct lruvec *lruvec, void *private),
				void *private)
{
	struct mem_cgroup *memcg;
	int nid, ret;

	/* fixme if more node in device */
	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);

		memcg = mem_cgroup_iter_dup(NULL, NULL, NULL);
		do {
			struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

			ret = cb(memcg, lruvec, private);
			if (ret) {
				mem_cgroup_iter_break_dup(NULL, memcg);
				break;
			}
			memcg = mem_cgroup_iter_dup(NULL, memcg, NULL);
		} while (memcg);
	}
}

static int shrink_ezr_folios(struct scan_control *sc)
{
	do_iter_mem_cgroups_lruvec(iter_memcg_callback, sc);
	return sc->nr_reclaimed;
}

/******************************************************************************
 *                          ezr memory reclaim
 *	ezr_memory_reclaim_all
 *	ezr_memory_reclaim
 ******************************************************************************/
static inline void shrink_ezr_folios_all(struct scan_control *sc)
{
	unsigned int nr_retries = MAX_RECLAIM_RETRIES / 4;
	unsigned long long start_nsecs;
	unsigned long nr_to_reclaim, nr_reclaimed;

	start_nsecs = sched_clock();
	nr_to_reclaim = atomic_read(&ezreclaimable_nr);
	nr_reclaimed = 0;
	while (nr_reclaimed < nr_to_reclaim) {
		unsigned long batch_size = (nr_to_reclaim - nr_reclaimed) / 4;
		unsigned long reclaimed;

		sc->nr_to_reclaim = batch_size;
		/* shrink_ezr_folios assign reclaimed to nr_reclaimed, so reset here. */
		sc->nr_reclaimed = 0;

		reclaimed = shrink_ezr_folios(sc);
		if (!reclaimed && !nr_retries--)
			goto out;

		nr_reclaimed += reclaimed;
	}
	sc->nr_reclaimed = nr_reclaimed;
out:
	ezr_logi("EZRECLAIM_MIN: nr_to_reclaim: %lu nr_reclaimed: %lu use(ms): %llu\n",
		nr_to_reclaim, sc->nr_reclaimed, (sched_clock() - start_nsecs) / NSEC_PER_MSEC);
}

/******************************************************************************
 *                          memory reclaim boost
 *	direct_reclaim
 *	kswapd
 ******************************************************************************/
static void ezr_rvh_perform_reclaim(void *data, int order, gfp_t gfp_mask,
				    nodemask_t *nodemask,
				    unsigned long *nr_reclaimed,
				    bool *skip)
{
	struct scan_control sc = {
		.gfp_mask = current_gfp_context(gfp_mask),
		.android_vendor_data1 = EZR_RECLAIM_DIRECT_RECLAIM,
	};
	long wmark, delta;

	if (order) {
		inc_ezr_event(EZR_RC_FAILED_ORDER);
		return;
	}

	if (unlikely(osvelte_test_scene(MM_SCENE_CAMERA)))
		wmark = 0;
	else
		wmark = ezr_wmarks[EZR_WM_MIN];
	delta = atomic_read(&ezreclaimable_nr) - wmark;
	if (delta < MIN_LRU_BATCH)
		return;

	sc.nr_to_reclaim = MIN_LRU_BATCH;
	shrink_ezr_folios(&sc);
	if (sc.nr_reclaimed < SWAP_CLUSTER_MAX) {
		inc_ezr_event(EZR_RC_FAILED0);
		return;
	}

	*skip = true;
	*nr_reclaimed = sc.nr_reclaimed;
	inc_ezr_event(EZR_RC_DIRECT);
	wakeup_ezreclaimd();
}

static void ezr_rvh_kswapd_shrink_node(void *data, unsigned long *nr_to_reclaim)
{
	struct ezr_struct *ezrs = &g_ezr;
	struct scan_control sc = {
		.android_vendor_data1 = EZR_RECLAIM_KSWAPD,
	};
	struct scan_control *kswapd_sc;
	long wmark, delta;

	kswapd_sc = container_of(nr_to_reclaim, struct scan_control, nr_to_reclaim);
	if (unlikely(osvelte_test_scene(MM_SCENE_CAMERA)))
		wmark = 0;
	else
		wmark = ezr_wmarks[EZR_WM_DIRECT_RECLAIM];
	delta = atomic_read(&ezreclaimable_nr) - wmark;
	if (delta < ezrs->min_batch)
		return;

	sc.nr_to_reclaim = min_t(unsigned long, *nr_to_reclaim,
				 (unsigned long)delta);
	shrink_ezr_folios(&sc);
	kswapd_sc->nr_reclaimed += sc.nr_reclaimed;
	inc_ezr_event(EZR_RC_KSWAPD);
	wakeup_ezreclaimd();
}

/* reclaim vh */
static inline void show_val_meminfo(struct seq_file *m,
				    const char *str, long size)
{
	char name[17];
	int len = strlen(str);

	if (len <= 15) {
		snprintf(name, sizeof(name), "%s:", str);
	} else {
		strscpy(name, str, 15);
		name[15] = ':';
		name[16] = '\0';
	}

	seq_printf(m, "%-16s%8ld kB\n", name, size);
}

/******************************************************************************
 *                          meminfo & mem_availiable adjust
 ******************************************************************************/
static void ezr_vh_meminfo_proc_show(void *data, struct seq_file *m)
{
	show_val_meminfo(m, "Ezr",
			 atomic_read(&ezreclaimable_nr) << (PAGE_SHIFT - 10));
}

static void vh_mglru_should_abort_scan(void *data, u64 *ext, bool *bypass)
{
	struct scan_control *sc;
	struct ezr_struct *ezrs = &g_ezr;

	if (!current_is_demote_files())
		return;

	sc = container_of(ext, struct scan_control, android_vendor_data1);
	sc->nr_reclaimed += ezreclaimable_demote_cnt - g_ezr.last_demote_cnt;
	ezrs->last_demote_cnt = ezreclaimable_demote_cnt;
}

static void ezr_vh_available_adjust(void *data, unsigned long *available)
{
	long delta;

	delta = atomic_read(&ezreclaimable_nr) -
		(int)ezr_wmarks[EZR_WM_MIN];
	if (delta < 0)
		delta = 0;
	*available += delta;
}

/******************************************************************************
 *                          sysfs knobs
 *      stats
 *      reclaim_test
 *      wmarks
 *      thrashing limit pct
 ******************************************************************************/
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int size = 0, i;
	struct ezr_struct *ezrs = &g_ezr;

	size += sysfs_emit_at(buf, size, "nr_pages             %u\n",
			      atomic_read(&ezreclaimable_nr));

	size += sysfs_emit_at(buf, size, "promote_cnt          %lu\n",
			      ezreclaimable_promote_cnt);
	size += sysfs_emit_at(buf, size, "demote_cnt           %lu\n",
			      ezreclaimable_demote_cnt);
	size += sysfs_emit_at(buf, size, "reclaim_cnt          %lu\n",
			      ezreclaimable_reclaim_cnt);

	size += sysfs_emit_at(buf, size, "wm_direct            %lu\n",
			      ezr_wmarks[EZR_WM_DIRECT_RECLAIM]);
	size += sysfs_emit_at(buf, size, "wm_min               %lu\n",
			      ezr_wmarks[EZR_WM_MIN]);

	size += sysfs_emit_at(buf, size, "thrashing_limit_pct  %u\n",
			      ezrs->thrashing_limit_pct);
	size += sysfs_emit_at(buf, size, "display_off          %d\n",
			      atomic_read(&display_off));

	for (i = EZR_SLEEP_DISPLAY_OFF; i < NR_MAX_EZR_STAT; i++)
		size += sysfs_emit_at(buf, size, "%-20s %u\n",
				      ezr_stat_item_txt[i],
				      atomic_read(ezrs->stat_items + i));
	return size;
}

static ssize_t reclaim_test_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long value;
	struct scan_control sc = {
		.android_vendor_data1 = EZR_RECLAIM_ALL,
	};

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;
	if (value != 1)
		return -EINVAL;

	shrink_ezr_folios_all(&sc);
	return count;
}

static ssize_t wmarks_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long wmarks[NR_EZR_WMARKS];
	int ret, i;

	ret = sscanf(buf, "%lu %lu", &wmarks[EZR_WM_DIRECT_RECLAIM], &wmarks[EZR_WM_MIN]);
	if (ret != 2)
		return -EINVAL;

	for (i = 0; i < 2; i++)
		ezr_wmarks[i] = wmarks[i];
	return count;
}

static ssize_t thrashing_limit_pct_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned long value;

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;

	if (value <= 0)
		return -EINVAL;
	g_ezr.thrashing_limit_pct = value;
	return count;
}

static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute reclaim_test_attr = __ATTR_WO(reclaim_test);
static struct kobj_attribute wmarks_attr = __ATTR_WO(wmarks);
static struct kobj_attribute thrashing_limit_pct_attr = __ATTR_WO(thrashing_limit_pct);

static struct attribute *attrs[] = {
	&stats_attr.attr,
	&reclaim_test_attr.attr,
	&wmarks_attr.attr,
	&thrashing_limit_pct_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int attach_kprobe(void)
{
	struct kprobe kp_mem_cgroup_iter = {
		.symbol_name = "mem_cgroup_iter",
	};
	struct kprobe kp_mem_cgroup_iter_break = {
		.symbol_name = "mem_cgroup_iter_break",
	};
	int ret;

	ret = register_kprobe(&kp_mem_cgroup_iter);
	if (ret) {
		ezr_loge("kprobe mem_cgroup_iter error\n");
		return ret;
	}
	mem_cgroup_iter_dup = (mem_cgroup_iter_t)kp_mem_cgroup_iter.addr;
	unregister_kprobe(&kp_mem_cgroup_iter);

	ret = register_kprobe(&kp_mem_cgroup_iter_break);
	if (ret) {
		ezr_loge("kprobe mem_cgroup_iter_break error\n");
		return ret;
	}
	mem_cgroup_iter_break_dup = (mem_cgroup_iter_break_t)kp_mem_cgroup_iter_break.addr;
	unregister_kprobe(&kp_mem_cgroup_iter_break);
	return 0;
}

static int ezreclaimable_init(void)
{
	int ret;
	struct ezr_struct *ezrs = &g_ezr;
	unsigned long total_ram;
	struct kobject *ezr_sysfs_kobj;
	struct kobject *oplus_mm_kobj = NULL;

	oplus_mm_kobj = (struct kobject *)osvelte_read_symbol(OPLUS_MM_KOBJ, true);
	if (!oplus_mm_kobj) {
		ezr_logi("create oplus_mm_kobj failed\n");
		return -EINVAL;
	}

	ezr_sysfs_kobj = kobject_create_and_add("ezr", oplus_mm_kobj);
	if (!ezr_sysfs_kobj) {
		ezr_loge("failed to create sysfs kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(ezr_sysfs_kobj, &attr_group);
	if (ret) {
		ezr_loge("failed to create syfs attr group\n");
		kobject_put(oplus_mm_kobj);
	}
	ezr_logi("stage1: create sysfs group\n");

	if (attach_kprobe()) {
		ezr_loge("failed to attach kprobe\n");
		return -EINVAL;
	}

	/* copy from init.oplus.nandswap.sh */
	total_ram = totalram_pages();
	if (total_ram <= (SZ_1G / PAGE_SIZE * 6)) {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 128);
		ezr_wmarks[EZR_WM_MIN] = 0;
	} else if (total_ram <= (SZ_8G / PAGE_SIZE)) {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 320);
		ezr_wmarks[EZR_WM_MIN] = (SZ_1M / PAGE_SIZE * 128);
		ezr_min_avail_buffer = 2400;
		ezr_high_avail_buffer = 2500;
	} else {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 640);
		ezr_wmarks[EZR_WM_MIN] = (SZ_1M / PAGE_SIZE * 512);
	}

	ret = register_trace_android_vh_si_mem_available_adjust(ezr_vh_available_adjust, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_shrink_folio_list(ezr_vh_shrink_folio_list, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_tune_swappiness(ezr_vh_tune_swappiness, NULL);
	if (ret)
		return ret;

	/* mglru hook */
	ret = register_trace_android_vh_lru_gen_add_folio_skip(ezr_vh_lru_gen_add_folio_skip, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_lru_gen_del_folio_skip(ezr_vh_lru_gen_del_folio_skip, NULL);
	if (ret)
		return ret;

	/* reclaim hook */
	ret = register_trace_android_rvh_perform_reclaim(ezr_rvh_perform_reclaim, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_rvh_kswapd_shrink_node(ezr_rvh_kswapd_shrink_node, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_look_around_migrate_folio(ezr_vh_look_around_migrate_folio, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_clear_reclaimed_folio(ezr_vh_clear_reclaimed_folio, NULL);
	if (ret)
		return ret;
	/* demote hook */
	ret = register_trace_android_vh_meminfo_proc_show(ezr_vh_meminfo_proc_show, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_keep_reclaimed_folio(ezr_vh_keep_reclaimed_folio, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_mglru_should_abort_scan(vh_mglru_should_abort_scan, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_evict_folios_bypass(ezr_vh_evict_folios_bypass, NULL);
	if (ret)
		return ret;

	/* filemap hook */
	ret = register_trace_android_vh_filemap_get_folio(ezr_vh_filemap_get_folio, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_filemap_pages(ezr_vh_filemap_pages, NULL);
	if (ret)
		return ret;
	ezr_logi("stage2: resgister vendor hook\n");

	ezrs->ezreclaimd_task = kthread_run(ezreclaimd, ezrs, "ezreclaimd");
	ezrs->window_sz_hz = msecs_to_jiffies(1000);
	swapd_pid = ezrs->ezreclaimd_task->tgid;
	ezr_refault_timer_init(ezrs->window_sz_hz);
	osvelte_register_symbol(OPLUS_TASK_EZRECLAIMD, ezrs->ezreclaimd_task);
	ezr_logi("stage3: init done, window_sz_hz: %lu\n", ezrs->window_sz_hz);
	return 0;
}
