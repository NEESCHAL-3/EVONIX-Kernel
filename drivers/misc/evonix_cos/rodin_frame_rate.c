// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysctl.h>
#include "frame_group.h"
#include "frame_info.h"

static DEFINE_MUTEX(rate_lock);
static struct ctl_table_header *rate_header;

static int frame_rate_handler(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int rate, ret;
	struct ctl_table temporary = *table;

	mutex_lock(&rate_lock);
	rate = get_frame_rate(SF_FRAME_GROUP_ID);
	if (rate < MIN_FRAME_RATE || rate > MAX_FRAME_RATE) {
		ret = -ENODEV;
		goto out;
	}
	temporary.data = &rate;
	temporary.maxlen = sizeof(rate);
	ret = proc_dointvec(&temporary, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;
	/* Rodin's real panel modes; this informs scheduling, not panel control. */
	if (rate != 60 && rate != 90 && rate != 120) {
		ret = -EINVAL;
		goto out;
	}
	set_frame_rate(SF_FRAME_GROUP_ID, rate);
	if (get_frame_rate(SF_FRAME_GROUP_ID) != rate)
		ret = -EIO;
out:
	mutex_unlock(&rate_lock);
	return ret;
}

static struct ctl_table rate_table[] = {
	{
		.procname = "frame_rate",
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = frame_rate_handler,
	},
	{ }
};

static int __init rodin_frame_rate_init(void)
{
	/* FrameBoost may initialize after vendor CPUFreq modules load.
	 * The handler reports ENODEV until that real backend is ready.
	 */
	rate_header = register_sysctl("kernel", rate_table);
	return rate_header ? 0 : -ENOMEM;
}

static void __exit rodin_frame_rate_exit(void)
{
	unregister_sysctl_table(rate_header);
}

module_init(rodin_frame_rate_init);
module_exit(rodin_frame_rate_exit);
MODULE_DESCRIPTION("Rodin FrameBoost frame-rate control adapter");
MODULE_LICENSE("GPL");
