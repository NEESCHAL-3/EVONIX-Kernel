#ifndef __DEBUG_COMMON_H__
#define __DEBUG_COMMON_H__

#include <linux/kernel.h>

#define DEFAULT_BUFFER_SIZE (1 << 8)

#define DT_PROTO(args...) args
#define DT_DATA(args...) args
#ifndef DECLARE_DEBUG_TRACE
#define DECLARE_DEBUG_TRACE(name, proto, data)			\
	static void __maybe_unused debug_##name(proto) {	\
		name(data);										\
	}
#define UNDEFINE_DECLARE_DEBUG_TRACE
#endif /* DECLARE_DEBUG_TRACE */

static noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

static void trace_pr_val_uint(unsigned long long msg, unsigned long long val)
{
	char buf[DEFAULT_BUFFER_SIZE] = {0};

	snprintf(buf, DEFAULT_BUFFER_SIZE - 1, "C|9001|%llu|%llu", msg, val);
	tracing_mark_write(buf);
}
DECLARE_DEBUG_TRACE(trace_pr_val_uint, DT_PROTO(unsigned long long msg, unsigned long long val), DT_DATA(msg, val));

static void trace_pr_val_str(const char *msg, unsigned long long val)
{
	char buf[DEFAULT_BUFFER_SIZE] = {0};

	snprintf(buf, DEFAULT_BUFFER_SIZE - 1, "C|9001|%s|%llu", msg, val);
	tracing_mark_write(buf);
}
DECLARE_DEBUG_TRACE(trace_pr_val_str, DT_PROTO(const char *msg, unsigned long long val), DT_DATA(msg, val));

static void trace_pr_val_com(const char *msg1, unsigned long long msg2, unsigned long long val)
{
	char buf[DEFAULT_BUFFER_SIZE] = {0};

	snprintf(buf, DEFAULT_BUFFER_SIZE - 1, "C|9001|%s%llu|%llu", msg1, msg2, val);
	tracing_mark_write(buf);
}
DECLARE_DEBUG_TRACE(trace_pr_val_com, DT_PROTO(const char *msg1, unsigned long long msg2, unsigned long long val), DT_DATA(msg1, msg2, val));

static void trace_begin(const char *msg)
{
	char buf[DEFAULT_BUFFER_SIZE] = {0};

	snprintf(buf, DEFAULT_BUFFER_SIZE - 1, "B|%d|%s", current->pid, msg);
	tracing_mark_write(buf);
}
DECLARE_DEBUG_TRACE(trace_begin, DT_PROTO(const char *msg), DT_DATA(msg));

static void trace_end(void)
{
	char buf[DEFAULT_BUFFER_SIZE] = {0};

	snprintf(buf, DEFAULT_BUFFER_SIZE - 1, "E|%d", current->pid);
	tracing_mark_write(buf);
}
DECLARE_DEBUG_TRACE(trace_end, DT_PROTO(void), DT_DATA());

#undef DEFAULT_BUFFER_SIZE
#undef DT_PROTO
#undef DT_DATA
#ifdef UNDEFINE_DECLARE_DEBUG_TRACE
#undef DECLARE_DEBUG_TRACE
#undef UNDEFINE_DECLARE_DEBUG_TRACE
#endif /* UNDEFINE_DECLARE_DEBUG_TRACE */

#endif // __DEBUG_COMMON_H__
