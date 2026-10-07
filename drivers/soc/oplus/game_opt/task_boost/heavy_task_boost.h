#ifndef __HEAVEY_TASK_BOOST_H__
#define __HEAVEY_TASK_BOOST_H__

void heavy_task_boost(struct task_struct *task, void *rrt, int rrt_num);
void htb_notify_frame_produce(void);
void htb_notify_enable(bool enable);
void htb_notify_boost_strategy_changed(int strategy);
void htb_notify_target_fps_changed(int target_fps);
int heavy_task_boost_init(void);
void heavy_task_boost_exit(void);

#endif