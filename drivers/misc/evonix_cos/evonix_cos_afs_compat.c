// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/cred.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define EVX_AFS_MAX_SCENE 64

/* Wire layouts verified against ColorOS 17 afsConfig.so. */
struct evx_afs_upload {
	__u32 version;
	__u32 max_scene;
	__u64 entries;
};

struct evx_afs_scene {
	__u32 scene;
	__u8 flags[4];
};

struct evx_afs_query {
	__u32 scene;
	struct evx_afs_scene config;
} __packed;

#define EVX_AFS_UPLOAD _IOW('q', 1, struct evx_afs_upload)
#define EVX_AFS_QUERY _IOR('q', 2, struct evx_afs_query)

static DEFINE_MUTEX(evx_afs_lock);
static struct evx_afs_scene *evx_afs_scenes;
static unsigned int evx_afs_max_scene;
static unsigned int evx_afs_version;
static bool evx_afs_enabled = true;
static struct proc_dir_entry *evx_afs_dir;

static int evx_afs_validate_table(const struct evx_afs_scene *scenes,
				unsigned int max_scene)
{
	unsigned int index;

	if (max_scene > EVX_AFS_MAX_SCENE)
		return -E2BIG;
	for (index = 0; index <= max_scene; index++) {
		/* Missing rows are zero-filled by the real userspace uploader. */
		if (!scenes[index].scene && !scenes[index].flags[0] &&
		    !scenes[index].flags[1] && !scenes[index].flags[2] &&
		    !scenes[index].flags[3])
			continue;
		if (scenes[index].scene != index)
			return -EINVAL;
	}
	return 0;
}

static long evx_afs_config_ioctl(struct file *file,
			       unsigned int cmd, unsigned long arg)
{
	void __user *user = (void __user *)arg;
	struct evx_afs_upload upload;
	struct evx_afs_query query;
	struct evx_afs_scene *scenes, *old;
	int ret;

	switch (cmd) {
	case EVX_AFS_UPLOAD:
		if (!(file->f_mode & FMODE_WRITE))
			return -EBADF;
		/* Only the system configuration publisher may replace the table. */
		if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
		    !uid_eq(current_euid(), KUIDT_INIT(1000)))
			return -EPERM;
		if (copy_from_user(&upload, user, sizeof(upload)))
			return -EFAULT;
		if (upload.max_scene > EVX_AFS_MAX_SCENE)
			return -E2BIG;
		scenes = memdup_user(u64_to_user_ptr(upload.entries),
				    array_size(upload.max_scene + 1, sizeof(*scenes)));
		if (IS_ERR(scenes))
			return PTR_ERR(scenes);
		ret = evx_afs_validate_table(scenes, upload.max_scene);
		if (ret) {
			kfree(scenes);
			return ret;
		}
		mutex_lock(&evx_afs_lock);
		old = evx_afs_scenes;
		evx_afs_scenes = scenes;
		evx_afs_max_scene = upload.max_scene;
		evx_afs_version = upload.version;
		mutex_unlock(&evx_afs_lock);
		kfree(old);
		return 0;
	case EVX_AFS_QUERY:
		if (copy_from_user(&query, user, sizeof(query)))
			return -EFAULT;
		mutex_lock(&evx_afs_lock);
		if (!evx_afs_enabled)
			ret = -EAGAIN;
		else if (!evx_afs_scenes)
			ret = -ENODATA;
		else if (query.scene > evx_afs_max_scene ||
			 evx_afs_scenes[query.scene].scene != query.scene)
			ret = -ENOENT;
		else {
			query.config = evx_afs_scenes[query.scene];
			ret = 0;
		}
		mutex_unlock(&evx_afs_lock);
		if (ret)
			return ret;
		return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
	default:
		return -ENOTTY;
	}
}

static int evx_afs_config_show(struct seq_file *m, void *v)
{
	mutex_lock(&evx_afs_lock);
	seq_printf(m, "enabled=%u configured=%u version=%u max_scene=%u\n",
		   evx_afs_enabled, !!evx_afs_scenes,
		   evx_afs_version, evx_afs_max_scene);
	mutex_unlock(&evx_afs_lock);
	return 0;
}

static int evx_afs_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_afs_config_show, NULL);
}

static const struct proc_ops evx_afs_config_fops = {
	.proc_open = evx_afs_config_open,
	.proc_read = seq_read,
	.proc_ioctl = evx_afs_config_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_afs_config_ioctl,
#endif
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int evx_afs_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", READ_ONCE(evx_afs_enabled));
	return 0;
}

static int evx_afs_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_afs_enable_show, NULL);
}

static ssize_t evx_afs_enable_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	unsigned int enabled;
	int ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
	    !uid_eq(current_euid(), KUIDT_INIT(1000)))
		return -EPERM;
	ret = kstrtouint_from_user(buf, count, 10, &enabled);

	if (ret)
		return ret;
	if (enabled > 1)
		return -EINVAL;
	mutex_lock(&evx_afs_lock);
	evx_afs_enabled = enabled;
	mutex_unlock(&evx_afs_lock);
	return count;
}

static const struct proc_ops evx_afs_enable_fops = {
	.proc_open = evx_afs_enable_open,
	.proc_read = seq_read,
	.proc_write = evx_afs_enable_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init evx_cos_afs_compat_init(void)
{
	struct proc_dir_entry *config, *enable;

	static_assert(sizeof(struct evx_afs_upload) == 16);
	static_assert(sizeof(struct evx_afs_scene) == 8);
	static_assert(sizeof(struct evx_afs_query) == 12);

	evx_afs_dir = proc_mkdir("oplus_afs_config", NULL);
	if (!evx_afs_dir)
		return -ENOMEM;
	config = proc_create("afs_config", 0644, evx_afs_dir, &evx_afs_config_fops);
	enable = proc_create("afs_enable", 0644, evx_afs_dir, &evx_afs_enable_fops);
	if (!config || !enable ||
	    !proc_create("afs_debug", 0444, evx_afs_dir, &evx_afs_config_fops)) {
		remove_proc_subtree("oplus_afs_config", NULL);
		return -ENOMEM;
	}
	proc_set_user(config, KUIDT_INIT(1000), KGIDT_INIT(1000));
	proc_set_user(enable, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return 0;
}

module_init(evx_cos_afs_compat_init);
MODULE_DESCRIPTION("ColorOS AFS scene configuration transport");
MODULE_LICENSE("GPL");
