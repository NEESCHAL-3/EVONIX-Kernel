/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2024 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM frame_boost
#if !defined(_TRACE_FRAME_BOOST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FRAME_BOOST_H
#include <linux/sched.h>
#include <linux/types.h>

#include <linux/tracepoint.h>

TRACE_EVENT(find_frame_boost_cpu,
	TP_PROTO(struct task_struct *p, const struct cpumask *cpumsk,
		const struct cpumask *precpumsk, const struct cpumask *available_cluster,
		char *msg, int target_cpu, int fbg_cpu),
	TP_ARGS(p, cpumsk, precpumsk, available_cluster, msg,
				target_cpu, fbg_cpu),
	TP_STRUCT__entry(__array(char, comm, TASK_COMM_LEN)
			__field(pid_t, pid)
			__field(unsigned long, cpumsk)
			__field(unsigned long, precpumsk)
			__field(unsigned long, available_cluster)
			__array(char, msg, TASK_COMM_LEN)
			__field(int, target_cpu)
			__field(int, fbg_cpu)),
	TP_fast_assign(__entry->pid = p->pid;
			memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
			__entry->cpumsk = cpumask_bits(cpumsk)[0];
			__entry->precpumsk = cpumask_bits(precpumsk)[0];
			__entry->available_cluster = cpumask_bits(available_cluster)[0];
			memcpy(__entry->msg, msg,
				min((size_t) TASK_COMM_LEN, strlen(msg) + 1));
			__entry->target_cpu = target_cpu;
			__entry->fbg_cpu = fbg_cpu;),
	TP_printk
		("comm=%s pid=%d cpumsk=0x%lx precluster=0x%lx availablecluster=0x%lx reason=%s target_cpu=%d, fbg_cpu=%d",
		__entry->comm, __entry->pid, __entry->cpumsk, __entry->precpumsk, __entry->available_cluster,
		__entry->msg, __entry->target_cpu, __entry->fbg_cpu)
);

#endif /*_TRACE_FRAME_BOOST_H */

#undef TRACE_INCLUDE_PATH
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
#define TRACE_INCLUDE_PATH .
#else
#define TRACE_INCLUDE_PATH ../../drivers/soc/oplus/frame_boost
#endif

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE frame_boost_trace
/* This part must be outside protection */
#include <trace/define_trace.h>

