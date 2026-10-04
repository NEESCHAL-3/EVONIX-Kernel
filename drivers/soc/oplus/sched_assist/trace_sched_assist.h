/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2024 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sched_assist

#if !defined(_TRACE_SCHED_ASSIST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHED_ASSIST_H

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/tracepoint.h>
#include "sa_common.h"
#include "sa_fair.h"

TRACE_EVENT(set_ux_task_to_prefer_cpu,
	TP_PROTO(struct task_struct *p, char *msg, int target_cpu,
		int ux_cpu, int start_cls, int cls_nr,
		const struct cpumask *cpumask),
	TP_ARGS(p, msg, target_cpu,
						ux_cpu, start_cls,
						cls_nr, cpumask),
	TP_STRUCT__entry(__field(int, pid)
			__array(char, comm, TASK_COMM_LEN)
			__array(char, cpus, 32)
			__field(unsigned long, util)
			__array(char, msg, TASK_COMM_LEN)
			__field(int, target_cpu)
			__field(int, ux_cpu)
			__field(int, start_cls)
			__field(int, cls_nr)
			__field(unsigned long, cpumask)),
	TP_fast_assign(__entry->pid = p->pid;
			memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
			__entry->util = oplus_task_util(p);
			scnprintf(__entry->cpus, sizeof(__entry->cpus),
				"%*pbl", cpumask_pr_args(&p->cpus_mask));
			memcpy(__entry->msg, msg,
				min((size_t) TASK_COMM_LEN, strlen(msg) + 1));
			__entry->target_cpu = target_cpu;
			__entry->ux_cpu = ux_cpu;
			__entry->start_cls = start_cls;
			__entry->cls_nr = cls_nr;
			__entry->cpumask = cpumask_bits(cpumask)[0];),
	TP_printk
		("pid=%d comm=%s util=%lu cpus_allowed=%s reason=%s, target_cpu=%d ux_cpu=%d start_cls=%d cls_nr=%d cpumask=0x%lx",
		__entry->pid, __entry->comm, __entry->util, __entry->cpus,
		__entry->msg, __entry->target_cpu, __entry->ux_cpu,
		__entry->start_cls, __entry->cls_nr, __entry->cpumask)
);

DECLARE_EVENT_CLASS(inherit_ux_template,

	TP_PROTO(struct task_struct *p, int type, int ux_state, s64 inherit_ux, int depth),

	TP_ARGS(p, type, ux_state, inherit_ux, depth),

	TP_STRUCT__entry(
		__field(int,	pid)
		__array(char,	comm, TASK_COMM_LEN)
		__field(int,	type)
		__field(int,	ux_state)
		__field(s64,	inherit_ux)
		__field(int,	depth)),

	TP_fast_assign(
		__entry->pid			= p->pid;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->type			= type;
		__entry->ux_state		= ux_state;
		__entry->inherit_ux		= inherit_ux;
		__entry->depth			= depth;),

	TP_printk("pid=%d comm=%s inherit_type=%d ux_state=%d inherit_ux=%llx ux_depth=%d",
		__entry->pid, __entry->comm, __entry->type, __entry->ux_state,
		__entry->inherit_ux, __entry->depth)
);

DEFINE_EVENT(inherit_ux_template, inherit_ux_set,
	TP_PROTO(struct task_struct *p, int type, int ux_state, s64 inherit_ux, int depth),
	TP_ARGS(p, type, ux_state, inherit_ux, depth));

DEFINE_EVENT(inherit_ux_template, inherit_ux_reset,
	TP_PROTO(struct task_struct *p, int type, int ux_state, s64 inherit_ux, int depth),
	TP_ARGS(p, type, ux_state, inherit_ux, depth));

DEFINE_EVENT(inherit_ux_template, inherit_ux_unset,
	TP_PROTO(struct task_struct *p, int type, int ux_state, s64 inherit_ux, int depth),
	TP_ARGS(p, type, ux_state, inherit_ux, depth));

#endif /*_TRACE_SCHED_ASSIST_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH drivers/soc/oplus/sched_assist

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_sched_assist
/* This part must be outside protection */
#include <trace/define_trace.h>

