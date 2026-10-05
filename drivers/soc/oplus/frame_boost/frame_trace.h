/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _FRAME_TRACE_H
#define _FRAME_TRACE_H
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/string.h>

static noinline int _tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

static void f_systrace_c(unsigned long val, char *msg)
{
        char buf[256];

        snprintf(buf, sizeof(buf), "C|99999|%s|%lu\n", msg, val);
        _tracing_mark_write(buf);
}

#endif /* _FRAME_TRACE_H */
