#include "critical_task_boost.h"
#include "game_ctrl.h"

#include <linux/hrtimer.h>
#include <uapi/linux/sched/types.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/atomic.h>

#define CRITICAL_TASK_NUM 2  // 0: unitymain 1: unitygfxdevicew
#define CPU_NUM 8
#define CRITICAL_TASK_STATUS_NUM 2  // 0: not running, 1: running
#define SLIDE_WINDOW_SIZE 10

static bool ct_enable = false;
static int target_fps = 0;
static u64 std_frame_length;

static const char* critical_task[CRITICAL_TASK_NUM] = {"UnityMain", "UnityGfxDevice"};

static struct hrtimer critical_task_long_stage_hrtimer;
static struct hrtimer critical_task_cancel_boost_hrtimer;
static struct hrtimer critical_task_slide_window_hrtimer;

enum CRITICAL_TASK_STATUS {
    CRITICAL_TASK_NOT_RUNNING = 0,
    CRITICAL_TASK_RUNNING = 1,
};

static u64 critical_task_running_time_ns[CRITICAL_TASK_NUM] = {0, 0};
static u64 critical_task_block_time_ns[CRITICAL_TASK_NUM] = {0, 0};
static u64 critical_task_end_time[CRITICAL_TASK_NUM] = {0, 0};
static enum CRITICAL_TASK_STATUS task_status[CRITICAL_TASK_NUM] = {0, 0};
static u64 running_time_percentage[CRITICAL_TASK_NUM] = {0, 0};

static int cpu_core[CRITICAL_TASK_NUM] = {-1, -1};

static u64 unitymain_expire_time_ns = 6000000;
static int unitymain_expire_time_percentage = 75;
static u64 cancel_boost_time_factor = 200;
static u64 expire_next_time_factor = 3;

static u64 critical_task_per_window_running_time_ns[CRITICAL_TASK_NUM][SLIDE_WINDOW_SIZE] = {0};
static u64 last_window_end_time_ns;
static int cur_slide_window_index = -1;
static u64 cur_slide_window_running_time_ns[CRITICAL_TASK_NUM] = {0};
static u64 per_window_time_span_ns = 0;
static int total_slide_window_num = 0;
static u64 total_window_running_time_ns[CRITICAL_TASK_NUM] = {0};

static u64 critical_task_running_time_threshold_percentage[CRITICAL_TASK_STATUS_NUM] = {90, 60};  // not running, running

static cpumask_var_t limit_cpumask;
static cpumask_var_t all_cpumask;

static atomic_t critical_task_running_status[CRITICAL_TASK_NUM] = {ATOMIC_INIT(CRITICAL_TASK_NOT_RUNNING), ATOMIC_INIT(CRITICAL_TASK_NOT_RUNNING)};

extern inline void systrace_c_printk(const char *msg, unsigned long val);
extern int get_critical_task_state(const char* name, pid_t pid);

static DEFINE_MUTEX(chb_mutex);
static DEFINE_RAW_SPINLOCK(chb_lock);

static struct kthread_work ct_work;
static struct kthread_worker ct_worker;
static struct kthread_work cb_work;
static struct kthread_worker cb_worker;
static struct kthread_work sw_work;
static struct kthread_worker sw_worker;

static atomic_t is_boost = ATOMIC_INIT(false);

static void set_all_cpu_mask(void)
{
    cpumask_clear(limit_cpumask);
    for (int i = 0; i < CPU_NUM; i++) {
        cpumask_set_cpu(i, all_cpumask);
    }
}

static void do_boost(void)
{
    if (atomic_read(&is_boost)) {
        return;
    }
    systrace_c_printk("do_boost", 1);
    ch_freq_boost_request(limit_cpumask, CT_REQUSET_BOOST);
    hrtimer_start(&critical_task_cancel_boost_hrtimer,
                    ktime_set(0, unitymain_expire_time_ns * cancel_boost_time_factor / 100),
                    HRTIMER_MODE_REL);
    atomic_set(&is_boost, true);
    systrace_c_printk("do_boost", 0);
}

static void release_boost(void)
{
    if (!atomic_read(&is_boost)) {
        return;
    }
    set_all_cpu_mask();
    systrace_c_printk("release boost", 1);
    systrace_c_printk("release boost", 0);
    ch_freq_boost_request(all_cpumask, CT_RELEASE_BOOST);
    atomic_set(&is_boost, false);
    cpumask_clear(all_cpumask);
}

static void start_hrtimer(void)
{
    if (unitymain_expire_time_ns >= 0) {
        hrtimer_start(&critical_task_long_stage_hrtimer, ktime_set(0, unitymain_expire_time_ns), HRTIMER_MODE_REL);
    }
    if (per_window_time_span_ns >= 0) {
        hrtimer_start(&critical_task_slide_window_hrtimer, ktime_set(0, per_window_time_span_ns), HRTIMER_MODE_REL);
    }
}

static void cancel_hrtime(void)
{
    hrtimer_cancel(&critical_task_long_stage_hrtimer);
    hrtimer_cancel(&critical_task_cancel_boost_hrtimer);
    hrtimer_cancel(&critical_task_slide_window_hrtimer);
}

static void compute_running_percentage(void)
{
    u64 now = 0;
    unsigned long flags;
    now = ktime_get_ns();
    raw_spin_lock_irqsave(&chb_lock, flags);
    for (int i = 0; i < CRITICAL_TASK_NUM; i++) {
        task_status[i] = atomic_read(&critical_task_running_status[i]);
        if (critical_task_end_time[i] != 0 && now >= critical_task_end_time[i]) {
            if (task_status[i] == CRITICAL_TASK_RUNNING) {
                critical_task_running_time_ns[i] += now - critical_task_end_time[i];
                if (now >= last_window_end_time_ns) {
                    cur_slide_window_running_time_ns[i] +=
                            now - max(critical_task_end_time[i], last_window_end_time_ns);
                }
            } else {
                critical_task_block_time_ns[i] += now - critical_task_end_time[i];
            }
        }
        critical_task_end_time[i] = now;
        u64 total_time = critical_task_running_time_ns[i] + critical_task_block_time_ns[i];
        u64 running_time = critical_task_running_time_ns[i];
        if (total_time <= 0) {
            goto unlock;
        }
        running_time_percentage[i] = (running_time * 100) / (total_time);
    }
unlock:
    raw_spin_unlock_irqrestore(&chb_lock, flags);
}

static bool decide_boost_status(void)
{
    cpumask_clear(limit_cpumask);
    for(int i = 0; i < CRITICAL_TASK_NUM; i++) {
        if (running_time_percentage[i] < critical_task_running_time_threshold_percentage[task_status[i]]) {
            continue;
        }
        int other_idx = (i + 1) % CRITICAL_TASK_NUM;  // CRITICAL_TASK_NUM = 2
        unsigned long flags;
        raw_spin_lock_irqsave(&chb_lock, flags);
        if (task_status[other_idx] == CRITICAL_TASK_RUNNING) {
            cpumask_set_cpu(cpu_core[other_idx], limit_cpumask);
        }
        cpumask_set_cpu(cpu_core[i], limit_cpumask);
        raw_spin_unlock_irqrestore(&chb_lock, flags);
        return true;
    }
    return false;
}

static void update_per_window_running_time(void)
{
    unsigned long flags;
    u64 now = ktime_get_ns();
    int i;
    raw_spin_lock_irqsave(&chb_lock, flags);

    for (i = 0; i < CRITICAL_TASK_NUM; i++) {
        task_status[i] = atomic_read(&critical_task_running_status[i]);
        total_window_running_time_ns[i] -= critical_task_per_window_running_time_ns[i][cur_slide_window_index];
        if (critical_task_end_time[i] != 0 && now >= critical_task_end_time[i]) {
            if (task_status[i] == CRITICAL_TASK_RUNNING && now >= last_window_end_time_ns) {
                cur_slide_window_running_time_ns[i] +=
                            now - max(critical_task_end_time[i], last_window_end_time_ns);
            }
            critical_task_per_window_running_time_ns[i][cur_slide_window_index] = cur_slide_window_running_time_ns[i];
            total_window_running_time_ns[i] += critical_task_per_window_running_time_ns[i][cur_slide_window_index];
        }
        cur_slide_window_running_time_ns[i] = 0;
    }
    last_window_end_time_ns = now;
    cur_slide_window_index = (cur_slide_window_index + 1) % SLIDE_WINDOW_SIZE;
    ++total_slide_window_num;
    raw_spin_unlock_irqrestore(&chb_lock, flags);
}

static bool check_slide_window_status(void)
{
    int i;
    u64 total_window_time = 0, expire_time_percentage = 0;
    if (total_slide_window_num < SLIDE_WINDOW_SIZE || per_window_time_span_ns <= 0) {
        return false;
    }
    total_window_time = SLIDE_WINDOW_SIZE * per_window_time_span_ns;
    cpumask_clear(limit_cpumask);
    for (i = 0; i < CRITICAL_TASK_NUM; i++) {
        expire_time_percentage = total_window_running_time_ns[i] * 100 / total_window_time;
        if (expire_time_percentage < critical_task_running_time_threshold_percentage[task_status[i]]) {
            continue;
        }
        int other_idx = (i + 1) % CRITICAL_TASK_NUM;  // CRITICAL_TASK_NUM = 2
        unsigned long flags;
        raw_spin_lock_irqsave(&chb_lock, flags);
        if (task_status[other_idx] == CRITICAL_TASK_RUNNING) {
            cpumask_set_cpu(cpu_core[other_idx], limit_cpumask);
        }
        cpumask_set_cpu(cpu_core[i], limit_cpumask);
        raw_spin_unlock_irqrestore(&chb_lock, flags);
        return true;
    }
    return false;
}

static bool check_long_stage_status(void)
{
    compute_running_percentage();
    return decide_boost_status();
}


static void ct_work_fn(struct kthread_work *work)
{
    if (check_long_stage_status()) {
        do_boost();
    }
}

static void cb_work_fn(struct kthread_work *work)
{
    release_boost();
}

static void sw_work_fn(struct kthread_work *work)
{
    update_per_window_running_time();
    if (check_slide_window_status()) {
        do_boost();
    }
}

static enum hrtimer_restart long_stage_callback_task(struct hrtimer *timer)
{
    if (atomic_read(&is_boost)) {
        return HRTIMER_NORESTART;
    }
    kthread_queue_work(&ct_worker, &ct_work);
    ktime_t kt = ktime_set(0, unitymain_expire_time_ns / expire_next_time_factor);
    hrtimer_forward_now(timer, kt);
    return HRTIMER_RESTART;
}

static enum hrtimer_restart cancel_boost_callback_task(struct hrtimer *timer)
{
    if (!atomic_read(&is_boost)) {
        return HRTIMER_NORESTART;
    }
    kthread_queue_work(&cb_worker, &cb_work);
    return HRTIMER_NORESTART;
}

static enum hrtimer_restart slide_window_callback_task(struct hrtimer *timer)
{
    kthread_queue_work(&sw_worker, &sw_work);
    if (per_window_time_span_ns <= 0) {
        return HRTIMER_NORESTART;
    }
    ktime_t kt = ktime_set(0, per_window_time_span_ns);
    hrtimer_forward_now(timer, kt);
    return HRTIMER_RESTART;
}

static void reset_time(void)
{
    u64 now = 0;
    int i, j;
    now = ktime_get_ns();
    for (i = 0; i < CRITICAL_TASK_NUM; i++) {
        critical_task_running_time_ns[i] = 0;
        critical_task_block_time_ns[i] = 0;
        critical_task_end_time[i] = now;
    }
    for (i = 0; i < CRITICAL_TASK_NUM; i++) {
        for (j = 0; j < SLIDE_WINDOW_SIZE; j++) {
            critical_task_per_window_running_time_ns[i][j] = 0;
        }
        total_window_running_time_ns[i] = 0;
        cur_slide_window_running_time_ns[i] = 0;
    }
    cur_slide_window_index = 0;
    last_window_end_time_ns = now;
    total_slide_window_num = 0;
}

void reset_critical_task_time(void)
{
    mutex_lock(&chb_mutex);
    reset_time();
    mutex_unlock(&chb_mutex);
}

void notify_frame_produdce(void)
{
    if (!ct_enable) {
        return;
    }
    systrace_c_printk("notify_frame_produdce", 1);
    kthread_cancel_work_sync(&ct_work);
    kthread_cancel_work_sync(&cb_work);
    kthread_cancel_work_sync(&sw_work);
    mutex_lock(&chb_mutex);
    release_boost();
    cancel_hrtime();
    reset_time();
    start_hrtimer();
    mutex_unlock(&chb_mutex);
    systrace_c_printk("notify_frame_produdce", 0);
}

static void update_critical_task_time(struct task_struct *task, int i, bool is_prev_task)
{
    if (strncmp(task->comm, critical_task[i], strlen(critical_task[i])) == 0) {
        int state = -1;
        pid_t pid = task->pid;
        state = get_critical_task_state(critical_task[i], pid);
        if (state == -1) {
            return;
        } else {
            u64 now = 0;
            unsigned long flags;
            now = ktime_get_ns();
            raw_spin_lock_irqsave(&chb_lock, flags);
            if (critical_task_end_time[i] != 0 && now >= critical_task_end_time[i]) {
                if (is_prev_task) {
                    critical_task_running_time_ns[i] += now - critical_task_end_time[i];
                    if (now > last_window_end_time_ns) {
                        cur_slide_window_running_time_ns[i] +=
                                    now - max(critical_task_end_time[i], last_window_end_time_ns);
                    }
                    atomic_set(&critical_task_running_status[i], CRITICAL_TASK_NOT_RUNNING);
                } else {
                    critical_task_block_time_ns[i] += now - critical_task_end_time[i];
                    int cpu = task_cpu(task);
                    cpu_core[i] = cpu;
                    atomic_set(&critical_task_running_status[i], CRITICAL_TASK_RUNNING);
                }
            }
            critical_task_end_time[i] = now;
            raw_spin_unlock_irqrestore(&chb_lock, flags);
        }
    }
}


static void sched_switch_hook(void *unused, bool preempt,
        struct task_struct *prev, struct task_struct *next, unsigned int prev_state)
{
    if (!ct_enable) {
        return;
    }
    for (int i = 0; i < CRITICAL_TASK_NUM; i++) {
        update_critical_task_time(prev, i, true);
        update_critical_task_time(next, i, false);
    }
}

static void register_critical_task_vendor_hooks(void)
{
    register_trace_sched_switch(sched_switch_hook, NULL);
}

static ssize_t ct_enable_proc_write(struct file *file,
    const char __user *buf, size_t count, loff_t *ppos)
{
    char page[32] = {0};
    int ret, value;
    bool enable;

    ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
    if (ret <= 0)
        return ret;

    ret = sscanf(page, "%d", &value);
    if (ret != 1)
        return -EINVAL;

    if (value != 0 && value != 1)
        return -EINVAL;

    enable = value == 1;

    mutex_lock(&chb_mutex);
    if (ct_enable != enable) {
        ct_enable = enable;
        if (!ct_enable) {
            kthread_cancel_work_sync(&ct_work);
            release_boost();
            cancel_hrtime();
            reset_time();
        }
    }
    mutex_unlock(&chb_mutex);

    return count;
}

static ssize_t ct_enable_proc_read(struct file *file,
    char __user *buf, size_t count, loff_t *ppos)
{
    char page[32] = {0};
    int len;

    mutex_lock(&chb_mutex);
    len = sprintf(page, "%d\n", ct_enable ? 1 : 0);
    mutex_unlock(&chb_mutex);

    return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops ct_enable_proc_ops = {
    .proc_write        = ct_enable_proc_write,
    .proc_read         = ct_enable_proc_read,
    .proc_lseek        = default_llseek,
};

static ssize_t expire_time_percentage_proc_write(struct file *file,
    const char __user *buf, size_t count, loff_t *ppos)
{
    char page[32] = {0};
    int ret;
    int percentage;
    int fps;

    ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
    if (ret <= 0)
        return ret;

    ret = sscanf(page, "%d %d", &fps, &percentage);
    if (ret != 2) {
        return -EINVAL;
    }

    mutex_lock(&chb_mutex);
    target_fps = fps;
    unitymain_expire_time_percentage = percentage;
    if (target_fps > 0 && unitymain_expire_time_percentage > 0) {
        std_frame_length = NSEC_PER_SEC / target_fps;
        unitymain_expire_time_ns = std_frame_length / 100 * unitymain_expire_time_percentage;
        per_window_time_span_ns = std_frame_length / SLIDE_WINDOW_SIZE;
    }

    mutex_unlock(&chb_mutex);

    return count;
}

static ssize_t expire_time_percentage_proc_read(struct file *file,
    char __user *buf, size_t count, loff_t *ppos)
{
    char page[128] = {0};
    int len;

    mutex_lock(&chb_mutex);
    len = sprintf(page, "target_fps:%d, expire_time_percentage:%d, expire_time_ns:%llu, per_window_time_span_ns:%llu\n",
                    target_fps, unitymain_expire_time_percentage, unitymain_expire_time_ns, per_window_time_span_ns);
    mutex_unlock(&chb_mutex);

    return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops expire_time_percentage_proc_ops = {
    .proc_write        = expire_time_percentage_proc_write,
    .proc_read         = expire_time_percentage_proc_read,
    .proc_lseek        = default_llseek,
};

static int ctb_kthread_create(void)
{
    int ret;
    struct task_struct *ct_thread, *cb_thread, *sw_thread;
    struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

    kthread_init_work(&ct_work, ct_work_fn);
    kthread_init_worker(&ct_worker);
    kthread_init_work(&cb_work, cb_work_fn);
    kthread_init_worker(&cb_worker);
    kthread_init_work(&sw_work, sw_work_fn);
    kthread_init_worker(&sw_worker);
    ct_thread = kthread_create(kthread_worker_fn, &ct_worker, "g_ct");
    cb_thread = kthread_create(kthread_worker_fn, &cb_worker, "g_cb");
    sw_thread = kthread_create(kthread_worker_fn, &sw_worker, "g_sw");
    if (IS_ERR(ct_thread)) {
        pr_err("failed to create g_ct ct_thread: %ld,\n", PTR_ERR(ct_thread));
        return PTR_ERR(ct_thread);
    } else if (IS_ERR(cb_thread)) {
        pr_err("failed to create g_ct cb_thread: %ld,\n", PTR_ERR(cb_thread));
        return PTR_ERR(cb_thread);
    } else if (IS_ERR(sw_thread)) {
        pr_err("failed to create g_ct sw_thread: %ld,\n", PTR_ERR(sw_thread));
        return PTR_ERR(sw_thread);
    }

    ret = sched_setscheduler_nocheck(ct_thread, SCHED_FIFO, &param);
    if (ret) {
        kthread_stop(ct_thread);
        pr_warn("%s: failed to set g_ct ct_thread SCHED_FIFO\n", __func__);
        return ret;
    }
    ret = sched_setscheduler_nocheck(cb_thread, SCHED_FIFO, &param);
    if (ret) {
        kthread_stop(cb_thread);
        pr_warn("%s: failed to set g_ct cb_thread SCHED_FIFO\n", __func__);
        return ret;
    }
    ret = sched_setscheduler_nocheck(sw_thread, SCHED_FIFO, &param);
    if (ret) {
        kthread_stop(sw_thread);
        pr_warn("%s: failed to set g_ct sw_thread SCHED_FIFO\n", __func__);
        return ret;
    }

    wake_up_process(ct_thread);
    wake_up_process(cb_thread);
    wake_up_process(sw_thread);

    return 0;
}

void hrtimer_boost_init(void)
{
    int ret;

    ret = ctb_kthread_create();
    if (ret)
        return;
    if (!alloc_cpumask_var(&limit_cpumask, GFP_KERNEL)) {
        return;
    }

    proc_create_data("ct_enable", 0664, critical_heavy_boost_dir, &ct_enable_proc_ops, NULL);
    proc_create_data("expire_time_percentage", 0664, critical_heavy_boost_dir, &expire_time_percentage_proc_ops, NULL);
    hrtimer_init(&critical_task_long_stage_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    critical_task_long_stage_hrtimer.function = long_stage_callback_task;
    hrtimer_init(&critical_task_cancel_boost_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    critical_task_cancel_boost_hrtimer.function = cancel_boost_callback_task;
    hrtimer_init(&critical_task_slide_window_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    critical_task_slide_window_hrtimer.function = slide_window_callback_task;
    register_critical_task_vendor_hooks();
}

void hrtimer_boost_exit(void)
{
    free_cpumask_var(limit_cpumask);
}
