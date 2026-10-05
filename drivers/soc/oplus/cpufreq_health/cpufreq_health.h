/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _CPUFREQ_HEALTH_H
#define _CPUFREQ_HEALTH_H

#include <linux/types.h>

struct cpufreq_health_info {
	u64 ceiling_time;
	u64 ceiling_count;
	u64 floor_time;
	u64 floor_count;
};

static inline void get_cpufreq_health_info(
	int *cnt,
	struct cpufreq_health_info *val)
{
	if (cnt)
		*cnt = 0;
}

#endif /* _CPUFREQ_HEALTH_H */
