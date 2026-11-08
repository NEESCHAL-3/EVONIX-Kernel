// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <uapi/linux/sched/types.h>

#include "osi_topology.h"

struct cluster_info cluster[CPU_NUMS];
u32 cluster_num;
struct cpumask all_cpu;
struct cpumask silver_cpu;
struct cpumask gold_cpu;
static struct cpumask policy_masks[CPU_NUMS];
static u32 cpu_policy_index[CPU_NUMS];


void update_cpu_mask(void)
{
	unsigned int cpu, min_capacity = arch_scale_cpu_capacity(0);

	for_each_possible_cpu(cpu) {
		if (arch_scale_cpu_capacity(cpu) <= min_capacity)
			min_capacity = arch_scale_cpu_capacity(cpu);
		cpumask_set_cpu(cpu, &gold_cpu);
	}
	for_each_possible_cpu(cpu) {
		if (arch_scale_cpu_capacity(cpu) == min_capacity)
			cpumask_clear_cpu(cpu, &gold_cpu);
	}
	cpumask_andnot(&silver_cpu, &all_cpu, &gold_cpu);
	pr_info("cpu_highcap_mask:%lx, %lx, %lx\n", cpumask_bits(&all_cpu)[0],
		cpumask_bits(&silver_cpu)[0], cpumask_bits(&gold_cpu)[0]);
}

int cluster_init(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu, index;

	memset(cluster, 0, sizeof(cluster));
	memset(policy_masks, 0, sizeof(policy_masks));
	cpumask_clear(&all_cpu);
	cpumask_clear(&silver_cpu);
	cpumask_clear(&gold_cpu);
	cluster_num = 0;
	for_each_possible_cpu(cpu) {
		if (cpu >= CPU_NUMS)
			return -E2BIG;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return -EPROBE_DEFER;
		for (index = 0; index < cluster_num; index++)
			if (cpumask_test_cpu(cpu, &policy_masks[index]))
				break;
		if (index == cluster_num) {
			if (cluster_num >= CPU_NUMS) {
				cpufreq_cpu_put(policy);
				return -E2BIG;
			}
			cpumask_and(&policy_masks[index], policy->related_cpus,
				    cpu_possible_mask);
			cluster[index].start_cpu = cpumask_first(&policy_masks[index]);
			cluster[index].cpu_nr = cpumask_weight(&policy_masks[index]);
			cluster_num++;
		}
		cpu_policy_index[cpu] = index;
		cpumask_set_cpu(cpu, &all_cpu);
		cpufreq_cpu_put(policy);
	}
	update_cpu_mask();
	return cluster_num ? 0 : -ENODEV;
}

u32 get_start_cpu(u32 cpu)
{
	if (cpu >= CPU_NUMS || !cpumask_test_cpu(cpu, &all_cpu))
		return INVALID_CPUID;
	return cluster[cpu_policy_index[cpu]].start_cpu;
}

u32 get_cluster_id(u32 cpu)
{
	if (cpu >= CPU_NUMS || !cpumask_test_cpu(cpu, &all_cpu))
		return INVALID_CPUID;
	return cpu_policy_index[cpu];
}

bool is_cluster_cpu(u32 cpu)
{
	return get_start_cpu(cpu) == cpu;
}
