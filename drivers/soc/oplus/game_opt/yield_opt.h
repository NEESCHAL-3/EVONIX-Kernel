#ifndef __YIELD_OPT__
#define __YIELD_OPT__

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/irq_work.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <kernel/time/tick-sched.h>
#include <trace/hooks/sched.h>
#include <linux/delay.h>


#define REGISTER_TRACE_VH(vender_hook, handler) \
{ \
	ret = register_trace_##vender_hook(handler, NULL); \
	if (ret) { \
		pr_err("failed to register_trace_"#vender_hook", ret=%d\n", ret); \
	} \
}

#define HMBIRD_CPUFREQ_WINDOW_ROLLOVER	BIT(31)
#define MAX_YIELD_SLEEP		(2000000ULL)
#define MIN_YIELD_SLEEP		(200000ULL)
#define YIELD_DURATION		(5000ULL)
#define DEFAULT_YIELD_SLEEP_TH	(10)

struct sched_yield_state {
	raw_spinlock_t	lock;
	u64				last_yield_time;
	u64				last_update_time;
	u64				sleep_end;
	unsigned long	yield_cnt;
	unsigned long	yield_cnt_after_sleep;
	unsigned long	sleep;
	int sleep_times;
};

struct yield_opt_params {
	int enable;
	int frame_per_sec;
	u64 frame_time_ns;
	int yield_headroom;
};

DECLARE_PER_CPU(struct sched_yield_state, ystate);

int yield_opt_init(void);

#endif // __YIELD_OPT__