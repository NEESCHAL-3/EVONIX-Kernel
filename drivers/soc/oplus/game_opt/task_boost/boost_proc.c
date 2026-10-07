#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/proc_fs.h>
#include <trace/events/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/errno.h>

#include "game_ctrl.h"
#include "heavy_task_boost.h"

static DEFINE_MUTEX(proc_mutex);

static int target_fps = 120;

static int htb_strategy = 0;
static int htb_enable = false;

static ssize_t target_fps_proc_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char page[32] = { 0 };
	int ret;
	int fps;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &fps);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&proc_mutex);
	if (fps != target_fps) {
		target_fps = fps;
		htb_notify_target_fps_changed(fps);
	}
	mutex_unlock(&proc_mutex);

	return count;
}

static ssize_t target_fps_proc_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	char page[32] = { 0 };
	int len;

	mutex_lock(&proc_mutex);
	len = sprintf(page, "%d\n", target_fps);
	mutex_unlock(&proc_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops target_fps_proc_ops = {
	.proc_write = target_fps_proc_write,
	.proc_read = target_fps_proc_read,
	.proc_lseek = default_llseek,
};

static ssize_t htb_strategy_proc_write(struct file *file,
				       const char __user *buf, size_t count,
				       loff_t *ppos)
{
	char page[32] = { 0 };
	int ret;
	int strategy;

	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	if (ret <= 0)
		return ret;

	ret = sscanf(page, "%d", &strategy);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&proc_mutex);
	if (strategy != htb_strategy) {
		htb_strategy = strategy;
		htb_notify_boost_strategy_changed(strategy);
	}
	mutex_unlock(&proc_mutex);

	return count;
}

static ssize_t htb_strategy_proc_read(struct file *file, char __user *buf,
				      size_t count, loff_t *ppos)
{
	char page[32] = { 0 };
	int len;

	mutex_lock(&proc_mutex);
	len = sprintf(page, "%d\n", htb_strategy);
	mutex_unlock(&proc_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops htb_strategy_proc_ops = {
	.proc_write = htb_strategy_proc_write,
	.proc_read = htb_strategy_proc_read,
	.proc_lseek = default_llseek,
};

static ssize_t htb_enable_proc_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char page[32] = { 0 };
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

	mutex_lock(&proc_mutex);
	if (htb_enable != enable) {
		htb_enable = enable;
	}
	mutex_unlock(&proc_mutex);

	htb_notify_enable(enable);

	return count;
}

static ssize_t htb_enable_proc_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	char page[32] = { 0 };
	int len;

	mutex_lock(&proc_mutex);
	len = sprintf(page, "%d\n", htb_enable ? 1 : 0);
	mutex_unlock(&proc_mutex);

	return simple_read_from_buffer(buf, count, ppos, page, len);
}

static const struct proc_ops htb_enable_proc_ops = {
	.proc_write = htb_enable_proc_write,
	.proc_read = htb_enable_proc_read,
	.proc_lseek = default_llseek,
};

int boost_proc_init(void)
{
	proc_create_data("target_fps", 0664, critical_heavy_boost_dir, &target_fps_proc_ops,
			 NULL);
	proc_create_data("htb_strategy", 0664, critical_heavy_boost_dir,
			 &htb_strategy_proc_ops, NULL);
	proc_create_data("htb_enable", 0664, critical_heavy_boost_dir, &htb_enable_proc_ops,
			 NULL);

	return 0;
}

void boost_proc_exit(void)
{
}