/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_OPLUS_FRAME_BOOST_H
#define _LINUX_OPLUS_FRAME_BOOST_H

struct task_struct;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
void evonix_fbg_flush_task(struct task_struct *p);
void evonix_fbg_sched_fork(struct task_struct *p);
void evonix_fbg_try_to_wake_up(struct task_struct *p);
void evonix_fbg_new_task_stats(struct task_struct *p);
#endif

#endif /* _LINUX_OPLUS_FRAME_BOOST_H */
