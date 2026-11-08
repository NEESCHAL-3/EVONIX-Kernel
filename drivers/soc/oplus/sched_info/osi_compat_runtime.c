// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compat.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include "osi_base.h"
#include "osi_compat_runtime.h"

struct osi_compat_history {
	raw_spinlock_t lock;
	u64 epoch[JANK_WIN_CNT];
	u64 runtime[JANK_WIN_CNT];
};
static struct osi_compat_history compat_history[CPU_NUMS];

void osi_compat_runtime_init(void)
{
	unsigned int cpu;

	for (cpu = 0; cpu < CPU_NUMS; cpu++)
		raw_spin_lock_init(&compat_history[cpu].lock);
}

/* sched_stat_runtime supplies accounted execution for fair, RT and DL tasks. */
void osi_compat_account_runtime(struct task_struct *task, u64 runtime)
{
	struct osi_compat_history *history;
	u64 now, begin, epoch, boundary, duration;
	unsigned long flags;
	unsigned int cpu, slot;

	if (!runtime || !is_compat_thread(task_thread_info(task)))
		return;
	cpu = task_cpu(task);
	if (cpu >= CPU_NUMS)
		return;
	now = ktime_get_ns();
	runtime = min_t(u64, runtime, (u64)JANK_WIN_CNT * JANK_WIN_SIZE_IN_NS);
	begin = now > runtime ? now - runtime : 0;
	history = &compat_history[cpu];
	raw_spin_lock_irqsave(&history->lock, flags);
	while (begin < now) {
		epoch = begin >> JANK_WIN_SIZE_SHIFT_IN_NS;
		slot = epoch & JANK_WIN_CNT_MASK;
		boundary = (epoch + 1) << JANK_WIN_SIZE_SHIFT_IN_NS;
		duration = min(now, boundary) - begin;
		if (history->epoch[slot] != epoch) {
			history->epoch[slot] = epoch;
			history->runtime[slot] = 0;
		}
		history->runtime[slot] = min_t(u64, JANK_WIN_SIZE_IN_NS,
				history->runtime[slot] + duration);
		begin += duration;
	}
	raw_spin_unlock_irqrestore(&history->lock, flags);
}

u32 osi_compat_cpu_load(u32 windows, const struct cpumask *mask)
{
	struct osi_compat_history *history;
	u64 now, begin, epoch, bucket_begin, bucket_end, overlap, busy = 0;
	u64 duration, denominator;
	unsigned long flags;
	unsigned int cpu, slot, cpus = 0;

	if (!windows || !mask)
		return 0;
	windows = min_t(u32, windows, JANK_WIN_CNT - 1);
	now = ktime_get_ns();
	duration = min_t(u64, now, (u64)windows * JANK_WIN_SIZE_IN_NS);
	if (!duration)
		return 0;
	begin = now - duration;
	for_each_cpu(cpu, mask) {
		if (cpu >= CPU_NUMS || !cpu_possible(cpu))
			continue;
		cpus++;
		history = &compat_history[cpu];
		raw_spin_lock_irqsave(&history->lock, flags);
		for (slot = 0; slot < JANK_WIN_CNT; slot++) {
			epoch = history->epoch[slot];
			bucket_begin = epoch << JANK_WIN_SIZE_SHIFT_IN_NS;
			bucket_end = min(now,
				(epoch + 1) << JANK_WIN_SIZE_SHIFT_IN_NS);
			if (bucket_begin >= now || bucket_end <= begin)
				continue;
			overlap = bucket_end - max(begin, bucket_begin);
			busy += mul_u64_u64_div_u64(history->runtime[slot], overlap,
						   bucket_end - bucket_begin);
		}
		raw_spin_unlock_irqrestore(&history->lock, flags);
	}
	if (!cpus)
		return 0;
	denominator = duration * cpus;
	return min_t(u64, 100, mul_u64_u64_div_u64(busy, 100, denominator));
}
