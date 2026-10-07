// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/sysctl.h>
#include "frame_sync.h"
#include "game_ctrl.h"
#include "task_boost/heavy_task_boost.h"

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS)
#include <linux/bwmon_geas.h>
#include <drivers/soc/qcom/dcvs/bwmon.h>
#endif

#define SYNC_READ_SHIFT 1 << 6
DEFINE_CTL_TABLE_POLL(framesync_poll);

struct proc_dir_entry *framesync_dir = NULL;
struct gameopt_frame_data* read_pointer = NULL;
struct gameopt_frame_data produce_data;
struct gameopt_frame_data consume_data;
struct gameopt_frame_data tl_pred_data;

#include "critical_task_boost.h"

int epoll_notify(struct ctl_table* ro_table, int write, void* buffer, size_t* lenp, loff_t* ppos) {
    return 0;
}

struct ctl_table framesync_table[] = {
    {
        .procname       = "epoll_notify",
        .mode           = 0664,
        .proc_handler   = epoll_notify,
        .poll           = &framesync_poll,
    },
    { }
};

static int notify_wait_fd(void) {
    atomic_inc(&framesync_poll.event);
    wake_up_interruptible(&framesync_poll.wait);
    return 0;
}

static int reset_wait_event_cnt(void) {
    atomic_set(&framesync_poll.event, 0);
    wake_up_interruptible(&framesync_poll.wait);
    return 0;
}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS)
extern void bwmon_on_frame_event(int cpu, int event);
#endif

static long sync_ctrl_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    struct gameopt_frame_data data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;
    if ((_IOC_TYPE(cmd) != GAMEOPT_EPOLL_MAGIC) || (_IOC_NR(cmd) >= NOTIFY_FRAME_MAX_ID)) {
        return -EINVAL;
    }
    if (copy_from_user(&data, uarg, sizeof(data))) {
        return -EFAULT;
    }
    switch (cmd) {
        case CMD_ID_GAMEOPT_EPOLL_PRODUCE:
            data.mode = NOTIFY_FRAME_PRODUCE;
            produce_data = data;
            read_pointer = &produce_data;
            ret = notify_wait_fd();
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS)
            bwmon_on_frame_event(255, 1);
#endif
			htb_notify_frame_produce();
			notify_frame_produdce();
        break;

        case CMD_ID_GAMEOPT_EPOLL_CONSUME:
            data.mode = NOTIFY_FRAME_CONSUME;
            consume_data = data;
            read_pointer = &consume_data;
            ret = notify_wait_fd();
        break;

        case CMD_ID_GAMEOPT_EPOLL_TLPRED:
            tl_pred_data = data;
            read_pointer = &tl_pred_data;
            ret = notify_wait_fd();
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_GEAS)
            bwmon_on_frame_event(255, 1);
#endif
			htb_notify_frame_produce();
			notify_frame_produdce();
        break;

        default:
            ret = reset_wait_event_cnt();
        break;
    }

    return ret;
}

#if IS_ENABLED(CONFIG_COMPAT)
static long compat_sync_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return sync_ctrl_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif /* CONFIG_COMPAT */

static int sync_ctrl_open(struct inode *inode, struct file *file) {
    return 0;
}

static int sync_ctrl_release(struct inode *inode, struct file *file) {
	return 0;
}

static int produce_read(struct seq_file *m, void *v) {
    if (read_pointer != NULL) {
        seq_printf(m, "%d;%ld\n", read_pointer->bufferN, read_pointer->timeStamp1);
    }
    return 0;
}

static int produce_open(struct inode *inode, struct file *file) {
    return single_open(file, produce_read, inode);
}

static int consume_read(struct seq_file *m, void *v) {
    if (read_pointer != NULL) {
        seq_printf(m, "%d;%ld\n", read_pointer->bufferN, read_pointer->timeStamp1);
    }
    return 0;
}

static int consume_open(struct inode *inode, struct file *file) {
    return single_open(file, consume_read, inode);
}

static int tl_pred_read(struct seq_file *m, void *v) {
    if (read_pointer != NULL) {
        seq_printf(m, "%ld;%ld;%d;%d\n", read_pointer->timeStamp1, read_pointer->timeStamp2, read_pointer->bufferN, read_pointer->mode);
    }
    return 0;
}

static int tl_pred_open(struct inode *inode, struct file *file) {
    return single_open(file, tl_pred_read, inode);
}

static const struct proc_ops sync_ctrl_proc_ops = {
    .proc_ioctl     = sync_ctrl_ioctl,
    .proc_open      = sync_ctrl_open,
    .proc_release   = sync_ctrl_release,
#if IS_ENABLED(CONFIG_COMPAT)
    .proc_compat_ioctl	= compat_sync_ctrl_ioctl,
#endif /* CONFIG_COMPAT */
    .proc_lseek     = default_llseek,
};

static const struct proc_ops produce_proc_ops = {
    .proc_open      = produce_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static const struct proc_ops consume_proc_ops = {
    .proc_open      = consume_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static const struct proc_ops tl_pred_proc_ops = {
    .proc_open      = tl_pred_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

int frame_sync_init(void) {
    if (unlikely(!game_opt_dir)) {
        return -ENOTDIR;
    }

    framesync_dir = proc_mkdir("frame_sync", game_opt_dir);

    proc_create_data("sync_ctrl", 0664, framesync_dir, &sync_ctrl_proc_ops, NULL);
    proc_create_data("produce", 0664, framesync_dir, &produce_proc_ops, NULL);
    proc_create_data("consume", 0664, framesync_dir, &consume_proc_ops, NULL);
    proc_create_data("tl_pred", 0664, framesync_dir, &tl_pred_proc_ops, NULL);

    struct ctl_table_header *hdr;
    hdr = register_sysctl("game_frame_sync", framesync_table);

    return 0;
}
