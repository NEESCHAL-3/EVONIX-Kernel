/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2024 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cputime

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH drivers/soc/oplus/sched_assist

#if !defined(_TRACE_HOOK_CPUTIME_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_CPUTIME_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_HOOK(android_vh_account_task_time,
	TP_PROTO(struct task_struct *p, struct rq *rq, int user_tick, int ticks),
	TP_ARGS(p, rq, user_tick, ticks));


#endif /* _TRACE_HOOK_CPUTIME_H */
/* This part must be outside protection */
#include <trace/define_trace.h>

