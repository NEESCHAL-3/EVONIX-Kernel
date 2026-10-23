#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <trace/hooks/sched.h>
#include <uapi/linux/sched/types.h>

#include "game_ctrl.h"
#include "heavy_task_boost.h"
#include "boost_proc.h"

#define RENDER_COUNT (2)
#define MAX_HEAVY_TASK_COUNT (16)
#define CHECKING_BOOST_INTERVAL (1000000) /* 1ms */
#define STANDARD_FRAME_INTERVAL (1 * NSEC_PER_SEC / 60)

#define GC_MARKER_PREFIX "GC Marker"

struct render_related_thread {
	pid_t pid;
	struct task_struct *task;
	u32 wake_count;
};

struct heavy_task {
	struct task_struct *task;
	u64 last_wake_time;
	int cluster;
	bool boosting;
};
static int htsks_num = 0;
static struct heavy_task heavy_tasks[MAX_HEAVY_TASK_COUNT];
static int boosted_htsks_num = 0;

static struct task_struct *renders[RENDER_COUNT] = { NULL };
static int policy_num;
static int *policy2cpu = NULL;
static int *clusters_boost_state = NULL;
static unsigned long cluster_bitmap;

static u64 heavy_task_threshold = STANDARD_FRAME_INTERVAL;
static u64 frame_produce_time;

static bool is_boosting = false;
static struct hrtimer checking_boost_hrtimer;
static struct task_struct *thread;
static struct kthread_work htb_work;
static struct kthread_worker htb_worker;
static DEFINE_RAW_SPINLOCK(htb_spinlock);
static DEFINE_MUTEX(htb_mutex);

static cpumask_var_t limit_cpumask;

static int boost_strategy = 0;
static bool htb_enable = false;
static bool init_success = false;

static inline bool render_is_running(void)
{
	return (renders[0] == NULL ? false : task_is_running(renders[0])) ||
	       (renders[1] == NULL ? false : task_is_running(renders[1]));
}

static inline bool is_gc_marker_thread(struct task_struct *p)
{
	return (strncmp(p->comm, GC_MARKER_PREFIX, strlen(GC_MARKER_PREFIX)) ==
		0);
}

static inline bool should_boost(struct heavy_task *htsk, u64 running_time)
{
	return !htsk->boosting &&
	       (!render_is_running() || is_gc_marker_thread(htsk->task)) &&
	       running_time > heavy_task_threshold;
}

static inline int get_task_cpu_cluster(struct task_struct *task)
{
	return cpu_topology[task_cpu(task)].cluster_id;
}

static bool is_render_related_thread(struct task_struct *task,
				     struct render_related_thread *rrt,
				     int rrt_num)
{
	int i;
	for (i = 0; i < rrt_num; i++) {
		if (rrt[i].task == task) {
			return true;
		}
	}
	return false;
}

static bool is_render_thread(struct task_struct *p)
{
	return (strncmp(p->comm, "UnityMain", 9) == 0) ||
	       (strncmp(p->comm, "UnityGfx", 8) == 0);
}

static void update_renders(struct task_struct *p)
{
	if (strncmp(p->comm, "UnityMain", 9) == 0) {
		renders[0] = p;
	} else {
		renders[1] = p;
	}
}

static void cancel_boost(unsigned long flags)
{
	int cpu, cluster;

	mutex_lock(&htb_mutex);
	cpumask_clear(limit_cpumask);
	for (cluster = 0; cluster < policy_num; cluster++) {
		if (flags & (1 << cluster)) {
			cpu = policy2cpu[cluster];
			cpumask_set_cpu(cpu, limit_cpumask);
			htb_systrace_c_printk("boosting_cluster", cluster, "",
					      0);
		}
	}
	ch_freq_boost_request(limit_cpumask, HT_RELEASE_BOOST);
	mutex_unlock(&htb_mutex);
}

static bool try_to_cancel_boost(struct heavy_task *htsk)
{
	if (htsk->boosting) {
		htb_systrace_c_printk("boosting", htsk->task->pid,
				      htsk->task->comm, 0);
		htsk->boosting = false;
		if (--clusters_boost_state[htsk->cluster] <= 0) {
			clusters_boost_state[htsk->cluster] = 0;
			return true;
		}
	}
	return false;
}

static unsigned long try_to_cancel_boost_all(bool reset_wake_time)
{
	int i;
	unsigned long flags = 0;
	for (i = 0; i < htsks_num; i++) {
		if (try_to_cancel_boost(&heavy_tasks[i])) {
			flags |= (1 << heavy_tasks[i].cluster);
		}
		if (reset_wake_time)
			heavy_tasks[i].last_wake_time = 0;
	}
	boosted_htsks_num = 0;
	return flags;
}

static void do_boost(unsigned long flags)
{
	int cpu, cluster;

	mutex_lock(&htb_mutex);
	cpumask_clear(limit_cpumask);
	for (cluster = 0; cluster < policy_num; cluster++) {
		if (flags & (1 << cluster)) {
			cpu = policy2cpu[cluster];
			cpumask_set_cpu(cpu, limit_cpumask);
			htb_systrace_c_printk("boosting_cluster", cluster, "",
					      1);
		}
	}
	ch_freq_boost_request(limit_cpumask, HT_REQUSET_BOOST);
	mutex_unlock(&htb_mutex);
}

static void check_htsks_full(void)
{
	if (htsks_num >= MAX_HEAVY_TASK_COUNT) {
		systrace_c_signed_printk("reset htsk_array", 1);
		systrace_c_signed_printk("reset htsk_array", 0);
		htsks_num = boosted_htsks_num;
	}
}

static void add_new_heavy_task(struct task_struct *p)
{
	check_htsks_full();
	heavy_tasks[htsks_num].task = current;
	heavy_tasks[htsks_num].last_wake_time = 0;
	heavy_tasks[htsks_num].boosting = false;
	heavy_tasks[htsks_num].cluster = get_task_cpu_cluster(current);
	htsks_num++;
}

static void update_heavy_tasks(struct task_struct *task,
			       struct render_related_thread *rrt, int rrt_num)
{
	int i;
	unsigned long flags;
	bool in_heavy_tasks = false;

	raw_spin_lock_irqsave(&htb_spinlock, flags);
	for (i = 0; i < htsks_num; i++) {
		if (heavy_tasks[i].task == task) {
			heavy_tasks[i].last_wake_time = ktime_get_ns();
			heavy_tasks[i].cluster = get_task_cpu_cluster(task);
			goto out;
		} else if (heavy_tasks[i].task == current) {
			in_heavy_tasks = true;
		}
	}

	/*
	 * Add task that may block render to heavy tasks array.
	 * Right now just set last_wake_time to 0.
	 */
	if (!in_heavy_tasks && is_render_thread(task) &&
	    is_render_related_thread(task, rrt, rrt_num) &&
	    !is_render_thread(current)) {
		update_renders(task);
		add_new_heavy_task(current);
	}

out:
	raw_spin_unlock_irqrestore(&htb_spinlock, flags);
}

void heavy_task_boost(struct task_struct *task, void *rrt, int rrt_num)
{
	if (!htb_enable || !init_success) {
		return;
	}

	update_heavy_tasks(task, (struct render_related_thread *)rrt, rrt_num);
}

static void swap_boosted_htsk(int idx)
{
	if (idx >= boosted_htsks_num) {
		struct heavy_task tmp = heavy_tasks[boosted_htsks_num];
		heavy_tasks[boosted_htsks_num] = heavy_tasks[idx];
		heavy_tasks[idx] = tmp;
		if (++boosted_htsks_num >= MAX_HEAVY_TASK_COUNT) {
			boosted_htsks_num = 0;
		}
	}
}

static u64 calculate_running_time(u64 now, u64 last_wake_time, int strategy)
{
	return strategy > 0 ?
		       now - frame_produce_time - (heavy_task_threshold / 10) :
		       now - last_wake_time;
}

static void htb_work_fn(struct kthread_work *work)
{
	int i, cluster;
	bool render_running = false, gc_marker_is_boosting = false;
	unsigned long flags, boost_flags = 0, cancel_flags = 0;
	u64 now, last_wake_time, running_time;
	struct task_struct *p;

	systrace_c_signed_printk("check boost", 1);

	raw_spin_lock_irqsave(&htb_spinlock, flags);
	for (i = 0; i < htsks_num; i++) {
		now = ktime_get_ns();
		p = heavy_tasks[i].task;
		cluster = heavy_tasks[i].cluster;
		last_wake_time = heavy_tasks[i].last_wake_time;

		running_time = 0;
		if (READ_ONCE((p)->__state) == TASK_RUNNING &&
		    last_wake_time != 0 && likely(now > last_wake_time)) {
			running_time = calculate_running_time(
				now, last_wake_time, boost_strategy);
		} else {
			if (try_to_cancel_boost(&heavy_tasks[i])) {
				cancel_flags |= (1 << cluster);
			}
		}

		if (task_is_running(p) && last_wake_time == 0) {
			heavy_tasks[i].last_wake_time = now;
		}

		if (running_time == 0) {
			continue;
		}

		/*
		 * When a boosting task is migrated to another cluster, cancel it and
		 * boost it in the new cluster if possible.
		 */
		if (heavy_tasks[i].boosting &&
		    get_task_cpu_cluster(p) != cluster) {
			if (try_to_cancel_boost(&heavy_tasks[i])) {
				cancel_flags |= (1 << cluster);
			}
		}

		if (is_gc_marker_thread(p) && heavy_tasks[i].boosting) {
			gc_marker_is_boosting = true;
		}

		if (should_boost(&heavy_tasks[i], running_time)) {
			htb_systrace_c_printk("boosting", p->pid, p->comm,
					      (task_rq(p)->cpu + 1));
			heavy_tasks[i].boosting = true;
			cluster = get_task_cpu_cluster(p);
			heavy_tasks[i].cluster = cluster;
			clusters_boost_state[cluster]++;
			swap_boosted_htsk(i);
			if (clusters_boost_state[cluster] == 1) {
				boost_flags |= (1 << cluster);
			}
		} else if (render_is_running()) {
			render_running = true;
			break;
		}
	}

	if (render_running && !gc_marker_is_boosting) {
		/* If the render restores running state, cancel all boost */
		cancel_flags |= try_to_cancel_boost_all(false);
	}

	for (i = 0; i < policy_num; i++) {
		htb_systrace_c_printk("clst stt", i, "",
				      clusters_boost_state[i]);
	}
	raw_spin_unlock_irqrestore(&htb_spinlock, flags);

	if (cancel_flags > 0) {
		cancel_boost(cancel_flags);
	}

	if (boost_flags > 0) {
		is_boosting = true;
		do_boost(boost_flags);
	}
	systrace_c_signed_printk("check boost", 0);
}

static int htb_kthread_create(void)
{
	int ret;
	struct sched_param param = {.sched_priority = MAX_RT_PRIO - 1 };

	kthread_init_work(&htb_work, htb_work_fn);
	kthread_init_worker(&htb_worker);
	thread = kthread_create(kthread_worker_fn, &htb_worker, "g_htb");
	if (IS_ERR(thread)) {
		pr_err("failed to create g_htb thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set g_htb thread SCHED_FIFO\n",
			__func__);
		return ret;
	}

	wake_up_process(thread);

	return 0;
}

static void sched_switch_hook(void *unused, bool preempt,
			      struct task_struct *prev,
			      struct task_struct *next, unsigned int prev_state)
{
	int i;
	unsigned long flags;
	pid_t pid;
	struct heavy_task *htsk;
	u64 now;

	if (!htb_enable || !init_success)
		return;

	raw_spin_lock_irqsave(&htb_spinlock, flags);
	for (i = 0; i < htsks_num; i++) {
		htsk = &heavy_tasks[i];
		pid = htsk->task->pid;
		now = ktime_get_ns();
		if (prev->pid == pid && prev_state > TASK_UNINTERRUPTIBLE) {
			htsk->last_wake_time = 0;
			break;
		}
	}
	if (is_gc_marker_thread(next)) {
		add_new_heavy_task(next);
	}
	raw_spin_unlock_irqrestore(&htb_spinlock, flags);
}

static int cpu_policy_init(void)
{
	int i, cpu;
	struct cpufreq_policy *policy;

	policy_num = 0;
	for_each_possible_cpu (cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			return -1;
		}
		cluster_bitmap |= (1 << policy_num);
		policy_num++;
		cpu = cpumask_last(policy->related_cpus);
		cpufreq_cpu_put(policy);
	}

	if (clusters_boost_state) {
		kfree(clusters_boost_state);
	}
	if (policy2cpu) {
		kfree(policy2cpu);
	}
	clusters_boost_state = kcalloc(policy_num, sizeof(int), GFP_KERNEL);
	policy2cpu = kcalloc(policy_num, sizeof(int), GFP_KERNEL);
	if (clusters_boost_state == NULL || policy2cpu == NULL) {
		return -1;
	}

	i = 0;
	for_each_possible_cpu (cpu) {
		if (i >= policy_num) {
			return -1;
		}
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			return -1;
		}
		policy2cpu[i] = cpu;
		clusters_boost_state[i] = 0;
		cpu = cpumask_last(policy->related_cpus);
		i++;
		cpufreq_cpu_put(policy);
	}

	return 0;
}

static void cpu_policy_exit(void)
{
	kfree(policy2cpu);
	kfree(clusters_boost_state);
}

static enum hrtimer_restart
checking_boost_hrtimer_callback(struct hrtimer *timer)
{
	kthread_queue_work(&htb_worker, &htb_work);
	hrtimer_forward_now(timer, ktime_set(0, CHECKING_BOOST_INTERVAL));

	return HRTIMER_RESTART;
}

static void timer_init(void)
{
	hrtimer_init(&checking_boost_hrtimer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);

	checking_boost_hrtimer.function = checking_boost_hrtimer_callback;
}

int heavy_task_boost_init(void)
{
	if (htb_kthread_create())
		return -1;

	if (!alloc_cpumask_var(&limit_cpumask, GFP_KERNEL))
		return -ENOMEM;

	timer_init();
	boost_proc_init();
	register_trace_sched_switch(sched_switch_hook, NULL);
	if (cpu_policy_init() < 0) {
		return -1;
	}
	init_success = true;

	return 0;
}

void heavy_task_boost_exit(void)
{
	cpu_policy_exit();
	boost_proc_exit();
	unregister_trace_sched_switch(sched_switch_hook, NULL);
}

void htb_notify_boost_strategy_changed(int strategy)
{
	if (strategy >= 0) {
		boost_strategy = strategy;
	}
}

void htb_notify_target_fps_changed(int target_fps)
{
	if (target_fps > 0) {
		heavy_task_threshold = NSEC_PER_SEC / target_fps;
	}
}

void htb_notify_enable(bool enable)
{
	htb_enable = enable;
	if (!enable) {
		htsks_num = 0;
		boosted_htsks_num = 0;
		hrtimer_cancel(&checking_boost_hrtimer);
		kthread_cancel_work_sync(&htb_work);
	}
}

/*
 * Reset boost state reference and cancel boost when next frame produced.
 */
void htb_notify_frame_produce(void)
{
	int i;
	unsigned long flags, cancel_flags = 0;

	if (!htb_enable) {
		return;
	}

	if (!init_success) {
		if (cpu_policy_init() < 0) {
			pr_warn("htb init failed, try to init again next time.");
			return;
		}
		init_success = true;
	}

	systrace_c_signed_printk("frame produce", 1);

	frame_produce_time = ktime_get_ns();
	raw_spin_lock_irqsave(&htb_spinlock, flags);
	for (i = 0; i < policy_num; i++) {
		clusters_boost_state[i] = 0;
	}
	cancel_flags = try_to_cancel_boost_all(true);
	raw_spin_unlock_irqrestore(&htb_spinlock, flags);

	hrtimer_cancel(&checking_boost_hrtimer);
	kthread_cancel_work_sync(&htb_work);

	if (cancel_flags > 0 || is_boosting) {
		cancel_boost((cancel_flags | cluster_bitmap));
		is_boosting = false;
	}

	hrtimer_start(&checking_boost_hrtimer,
		      ktime_set(0, heavy_task_threshold), HRTIMER_MODE_REL);

	systrace_c_signed_printk("frame produce", 0);
}
