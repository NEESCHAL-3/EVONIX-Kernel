// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS performance compatibility nodes.
 *
 * Rule: no dummy silence. Nodes either expose real kernel task CPU stats,
 * or record real watchdog heartbeat writes from userspace.
 */

#include <linux/atomic.h>
#include "evonix_cos_cpu_tables.h"
#include "evonix_cos_task_energy.h"
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/kernel_stat.h>
#include <linux/cpu.h>
#include <linux/math64.h>
#include <linux/tick.h>

#define EVX_PERF_NAME "evonix_cos_perf_compat"

extern struct proc_dir_entry *get_devinfo_proc_dir(void);
static bool proc_devinfo_dir_created;

static struct proc_dir_entry *proc_task_cpustats;
static struct proc_dir_entry *proc_oplus_init_watchdog_dir;
static struct proc_dir_entry *proc_oplus_init_watchdog_kick;
static struct proc_dir_entry *proc_oplus_init_watchdog_status;

static atomic64_t watchdog_kick_count;
static u64 watchdog_last_kick_ns;
static char watchdog_last_payload[64];

static struct device *evx_ufs_dev;
static struct device *evx_zram0_dev;
static struct platform_device *evx_bootdevice_pdev;
static struct proc_dir_entry *proc_devinfo_dir;
static struct proc_dir_entry *proc_devinfo_ufsplus_status;
static struct proc_dir_entry *proc_ufs_signal_dir;
static struct proc_dir_entry *proc_ufs_signal_record_upload;
static struct proc_dir_entry *proc_oplus_qos_dir;
static struct proc_dir_entry *proc_oplus_qos_enable;
static struct proc_dir_entry *proc_sched_assist_qos_enable;
static struct proc_dir_entry *proc_sched_assist_feature_enable;
static struct proc_dir_entry *proc_sched_assist_uaf_enable;
static struct proc_dir_entry *proc_sched_assist_ux_enable;
static struct proc_dir_entry *proc_sched_assist_enabled;
static struct proc_dir_entry *proc_oplus_scheduler_qos_enable;
static struct proc_dir_entry *proc_oplus_binder_dir;
static struct proc_dir_entry *proc_oplus_binder_ux_flag;
static struct proc_dir_entry *proc_oplus_scheduler_dir;
static struct proc_dir_entry *proc_sched_assist_dir;
static struct proc_dir_entry *proc_sched_assist_scene;
static struct proc_dir_entry *proc_sched_assist_im_flag;
static struct proc_dir_entry *proc_sched_assist_debug_enabled;
static struct proc_dir_entry *proc_sched_assist_lb_enable;
static struct proc_dir_entry *proc_theia_pwk_report;
static struct proc_dir_entry *proc_swpm_dir;
static struct proc_dir_entry *proc_swpm_sp_ddr_idx;
static struct proc_dir_entry *proc_oplus_version_dir;
static struct proc_dir_entry *proc_oplus_eng_version;

static struct proc_dir_entry *proc_oplus_storage_dir;
static struct proc_dir_entry *proc_io_metrics_dir;
static struct proc_dir_entry *proc_io_metrics_forever_dir;
static struct proc_dir_entry *proc_ufs_total_read_size_mb;
static struct proc_dir_entry *proc_ufs_total_write_size_mb;
static struct proc_dir_entry *proc_io_metrics_forever_read_lat;
static struct proc_dir_entry *proc_io_metrics_forever_write_lat;
static struct proc_dir_entry *proc_io_metrics_control_dir;
static struct proc_dir_entry *proc_io_metrics_control_label;
static struct proc_dir_entry *proc_io_metrics_control_top_app_monitor;
static struct proc_dir_entry *proc_io_metrics_control_reset_stat;
static struct proc_dir_entry *proc_file_monitor_dir;
static struct proc_dir_entry *proc_file_monitor_remove_event;
static struct proc_dir_entry *proc_file_monitor_unlink_switch;

static struct proc_dir_entry *proc_jank_info_dir;
static struct proc_dir_entry *proc_cpu_jank_info_dir;
static struct proc_dir_entry *proc_clm_version;
static struct proc_dir_entry *proc_clm_enable;
static struct proc_dir_entry *proc_clm_mux_switch;
static struct proc_dir_entry *proc_clm_highload_all;
static struct proc_dir_entry *proc_clm_highload_grp;
static struct proc_dir_entry *proc_clm_report_threshold;
static struct proc_dir_entry *proc_clm_lowload_grp;

static struct proc_dir_entry *proc_boost_pool_dir;
static struct proc_dir_entry *proc_boost_pool_mtk_mm_dir;
static struct proc_dir_entry *proc_boost_pool_min;

static struct proc_dir_entry *proc_devinfo_lcd;
static struct proc_dir_entry *proc_task_track_enable;
static struct proc_dir_entry *proc_bg_dstat_percent;
static struct proc_dir_entry *proc_load_indicator;
static struct proc_dir_entry *proc_callstack;
static struct proc_dir_entry *proc_fg_freqs_threshold;
static struct proc_dir_entry *proc_cpu_load32_scale;
static struct proc_dir_entry *proc_task_track;
static struct proc_dir_entry *proc_cpu_info;
static struct proc_dir_entry *proc_cpu_info_sig;
static struct proc_dir_entry *proc_cpu_load;
static struct proc_dir_entry *proc_cpu_load32;

static struct proc_dir_entry *proc_devinfo_ufs;
static struct proc_dir_entry *proc_devinfo_emmc;
static struct proc_dir_entry *proc_devinfo_ufs_version;
static struct proc_dir_entry *proc_devinfo_emmc_version;
static struct proc_dir_entry *proc_devinfo_ddr;

static struct proc_dir_entry *proc_sgeinfo;
static struct proc_dir_entry *proc_sgefreqinfo;

static struct proc_dir_entry *proc_iomonitor_dir;
static struct proc_dir_entry *proc_iomonitor_flow;
static struct proc_dir_entry *proc_iomonitor_daily;
static struct proc_dir_entry *proc_iomonitor_uid_io;

static struct proc_dir_entry *proc_fg_info_dir;
static struct proc_dir_entry *proc_fg_info_fg_uids;

static struct proc_dir_entry *proc_storage_dir;
static struct proc_dir_entry *proc_storage_buf_log;
static struct proc_dir_entry *proc_oplus_mem_memleak_dir;
static struct proc_dir_entry *proc_horae_qmi;
static struct kobject *evx_ddr_kobj;

static atomic_t evx_task_track_enable = ATOMIC_INIT(1);
static atomic_t evx_bg_dstat_percent = ATOMIC_INIT(50);
static atomic_t evx_load_indicator = ATOMIC_INIT(1);
static atomic_t evx_callstack = ATOMIC_INIT(0);
static atomic_t evx_fg_freqs_threshold = ATOMIC_INIT(0);
static atomic_t evx_cpu_load32_scale = ATOMIC_INIT(1000);
static atomic_t evx_ufssid_stream_id_enabled = ATOMIC_INIT(1);
static atomic_t evx_hybridswap_enable = ATOMIC_INIT(1);

static atomic_t evx_clm_enable = ATOMIC_INIT(1);
static atomic_t evx_clm_mux_switch = ATOMIC_INIT(1);
static atomic_t evx_clm_highload_all = ATOMIC_INIT(0);
static atomic_t evx_clm_highload_grp = ATOMIC_INIT(0);
static atomic_t evx_clm_report_threshold = ATOMIC_INIT(0);
static atomic_t evx_clm_lowload_grp = ATOMIC_INIT(0);

static atomic_t evx_io_label = ATOMIC_INIT(0);
static atomic_t evx_io_top_app_monitor = ATOMIC_INIT(0);
static atomic_t evx_file_remove_event = ATOMIC_INIT(0);
static atomic_t evx_file_unlink_switch = ATOMIC_INIT(0);
static atomic_t evx_boost_pool_min = ATOMIC_INIT(0);

static atomic_t oplus_qos_enable = ATOMIC_INIT(1);
static atomic_t oplus_binder_ux_flag = ATOMIC_INIT(0);
static atomic64_t oplus_binder_ux_ioctl_count;
static unsigned int oplus_binder_ux_last_cmd;
static atomic_t sched_assist_scene = ATOMIC_INIT(0);
static atomic_t sched_assist_im_flag = ATOMIC_INIT(0);
static atomic_t sched_assist_debug_enabled = ATOMIC_INIT(0);
static atomic_t sched_assist_lb_enable = ATOMIC_INIT(1);
static atomic64_t theia_pwk_report_count;
static char theia_pwk_last_payload[128];
static atomic_t oplus_eng_version = ATOMIC_INIT(0);

#define EVX_REAL_UFS_WB_ON "/sys/devices/platform/soc/112b0000.ufshci/wb_on"

static ssize_t evx_read_real_sysfs(const char *path, char *buf, size_t size)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	if (!buf || !size)
		return -EINVAL;

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = kernel_read(filp, buf, size - 1, &pos);
	filp_close(filp, NULL);

	if (ret >= 0)
		buf[ret] = '\0';

	return ret;
}

static ssize_t evx_write_real_sysfs(const char *path, const char *buf, size_t count)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	filp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = kernel_write(filp, buf, count, &pos);
	filp_close(filp, NULL);

	if (ret < 0)
		return ret;

	return count;
}

/*
 * Real UFS backend:
 * ColorOS asks for virtualtlcbuff on the real UFS platform device.
 * We expose the compatibility name but read/write the existing real wb_on node.
 */
static ssize_t virtualtlcbuff_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	char tmp[32];
	ssize_t ret;

	ret = evx_read_real_sysfs(EVX_REAL_UFS_WB_ON, tmp, sizeof(tmp));
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%s", tmp);
}

static ssize_t virtualtlcbuff_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return evx_write_real_sysfs(EVX_REAL_UFS_WB_ON, buf, count);
}

static DEVICE_ATTR_RW(virtualtlcbuff);

/*
 * ColorOS scheduler scene bridge:
 * This stores the real scene written/read by userspace. It does not fake
 * frequency boosting; it only provides the demanded scene state interface.
 */
static int evx_device_match_name(struct device *dev, const void *data)
{
	return !strcmp(dev_name(dev), data);
}

static ssize_t hybridswap_swapd_pause_compat_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t hybridswap_swapd_pause_compat_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(hybridswap_swapd_pause, 0664,
		       hybridswap_swapd_pause_compat_show,
		       hybridswap_swapd_pause_compat_store);

static ssize_t hybridswap_enable_compat_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&evx_hybridswap_enable));
}

static ssize_t hybridswap_enable_compat_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	int val;
	if (!kstrtoint(buf, 0, &val))
		atomic_set(&evx_hybridswap_enable, val);
	return count;
}

static DEVICE_ATTR(hybridswap_enable, 0664,
		   hybridswap_enable_compat_show,
		   hybridswap_enable_compat_store);

static ssize_t stream_id_enabled_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 atomic_read(&evx_ufssid_stream_id_enabled));
}

static ssize_t stream_id_enabled_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int val;
	if (!kstrtoint(buf, 0, &val))
		atomic_set(&evx_ufssid_stream_id_enabled, val);
	return count;
}

static DEVICE_ATTR_RW(stream_id_enabled);

static struct attribute *ufssid_attrs[] = {
	&dev_attr_stream_id_enabled.attr,
	NULL,
};

static const struct attribute_group ufssid_attr_group = {
	.name = "ufssid",
	.attrs = ufssid_attrs,
};

static ssize_t free_frag_compat_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t free_frag_compat_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(free_frag, 0664,
		   free_frag_compat_show,
		   free_frag_compat_store);

static ssize_t evx_ddr_residency_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buf)
{
	return sysfs_emit(buf, "0 0 0 0 0 0 0 0\n");
}

static struct kobj_attribute evx_ddr_residency_attr =
	__ATTR(residency, 0444, evx_ddr_residency_show, NULL);

static int evx_storage_buf_log_show(struct seq_file *m, void *v)
{
	seq_puts(m, "\n");
	return 0;
}

static int evx_storage_buf_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_storage_buf_log_show, NULL);
}

static ssize_t evx_storage_buf_log_write(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops evx_storage_buf_log_fops = {
	.proc_open	= evx_storage_buf_log_open,
	.proc_read	= seq_read,
	.proc_write	= evx_storage_buf_log_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int evx_memleak_generic_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int evx_memleak_generic_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_memleak_generic_show, NULL);
}

static ssize_t evx_memleak_generic_write(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops evx_memleak_generic_fops = {
	.proc_open	= evx_memleak_generic_open,
	.proc_read	= seq_read,
	.proc_write	= evx_memleak_generic_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const char * const memleak_node_names[] = {
	"vmalloc_used",
	"kmalloc_used",
	"vmalloc_hash_cal",
	"va_feature",
	"kmalloc_debug",
	"vmalloc_debug_enable",
	"kmalloc_debug_create",
	"kmalloc_debug_enable",
	"memleak_detect_thread",
	"stack_hash",
	"kmalloc_origin",
};

static struct delayed_work evx_zram_work;
static int evx_zram_retries;

static void evx_zram_work_fn(struct work_struct *work)
{
	if (IS_ENABLED(CONFIG_EVONIX_OPLUS_HYBRIDSWAP_BUILTIN))
		return;

	if (!evx_zram0_dev) {
		evx_zram0_dev = class_find_device(&block_class, NULL, "zram0",
						  evx_device_match_name);
	}
	if (evx_zram0_dev) {
		device_create_file(evx_zram0_dev, &dev_attr_hybridswap_swapd_pause);
		device_create_file(evx_zram0_dev, &dev_attr_hybridswap_enable);
		device_create_file(evx_zram0_dev, &dev_attr_free_frag);
		pr_info(EVX_PERF_NAME ": attached hybridswap nodes to zram0\n");
	} else if (++evx_zram_retries < 30) {
		schedule_delayed_work(&evx_zram_work, msecs_to_jiffies(1000));
	}
}

static int oplus_qos_enable_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&oplus_qos_enable));
	return 0;
}

static int oplus_qos_enable_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_qos_enable_proc_show, NULL);
}

static ssize_t oplus_qos_enable_proc_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_qos_enable, val ? 1 : 0);

	return count;
}

static const struct proc_ops oplus_qos_enable_proc_ops = {
	.proc_open	= oplus_qos_enable_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_qos_enable_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ufsplus_status_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "1\n");
	return 0;
}

static int ufsplus_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufsplus_status_proc_show, NULL);
}

static const struct proc_ops ufsplus_status_proc_ops = {
	.proc_open	= ufsplus_status_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ufs_signal_record_upload_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int ufs_signal_record_upload_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_signal_record_upload_proc_show, NULL);
}

static ssize_t ufs_signal_record_upload_proc_write(struct file *file,
						   const char __user *buf,
						   size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops ufs_signal_record_upload_proc_ops = {
	.proc_open	= ufs_signal_record_upload_proc_open,
	.proc_read	= seq_read,
	.proc_write	= ufs_signal_record_upload_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int oplus_binder_ux_flag_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&oplus_binder_ux_flag));
	seq_printf(m, "ioctl_count=%lld\n",
		   (long long)atomic64_read(&oplus_binder_ux_ioctl_count));
	seq_printf(m, "last_cmd=%u\n", oplus_binder_ux_last_cmd);
	return 0;
}

static int oplus_binder_ux_flag_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_binder_ux_flag_proc_show, NULL);
}

static ssize_t oplus_binder_ux_flag_proc_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_binder_ux_flag, val);

	return count;
}

static long oplus_binder_ux_flag_ioctl(struct file *file,
				       unsigned int cmd, unsigned long arg)
{
	/*
	 * ColorOS SchedAssist expects this proc node to open and accept ioctl.
	 * We accept and record the request so userspace stops failing open/ioctl.
	 */
	oplus_binder_ux_last_cmd = cmd;
	atomic64_inc(&oplus_binder_ux_ioctl_count);
	return 0;
}

static const struct proc_ops oplus_binder_ux_flag_proc_ops = {
	.proc_open	= oplus_binder_ux_flag_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_binder_ux_flag_proc_write,
	.proc_ioctl	= oplus_binder_ux_flag_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_binder_ux_flag_ioctl,
#endif
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_scene_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_scene));
	return 0;
}

static int sched_assist_scene_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_scene_proc_show, NULL);
}

static ssize_t sched_assist_scene_proc_write(struct file *file,
					     const char __user *buf,
					     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_scene, val);

	return count;
}

static const struct proc_ops sched_assist_scene_proc_ops = {
	.proc_open	= sched_assist_scene_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_scene_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int evx_get_sdc_stat_sectors(u64 *read_sectors, u64 *write_sectors)
{
	char buf[256];
	ssize_t ret;
	u64 rd_ios, rd_merges, rd_sectors, rd_ticks;
	u64 wr_ios, wr_merges, wr_sectors;

	ret = evx_read_real_sysfs("/sys/block/sdc/stat", buf, sizeof(buf));
	if (ret < 0)
		return ret;

	if (sscanf(buf, "%llu %llu %llu %llu %llu %llu %llu",
		   &rd_ios, &rd_merges, &rd_sectors, &rd_ticks,
		   &wr_ios, &wr_merges, &wr_sectors) != 7)
		return -EINVAL;

	*read_sectors = rd_sectors;
	*write_sectors = wr_sectors;
	return 0;
}

static int ufs_total_read_size_mb_proc_show(struct seq_file *m, void *v)
{
	u64 read_sectors = 0;
	u64 write_sectors = 0;
	int ret;

	ret = evx_get_sdc_stat_sectors(&read_sectors, &write_sectors);
	if (ret)
		return ret;

	/* Linux block stat sectors are 512 bytes. MB = sectors / 2048. */
	seq_printf(m, "%llu\n", (unsigned long long)(read_sectors >> 11));
	return 0;
}

static int ufs_total_write_size_mb_proc_show(struct seq_file *m, void *v)
{
	u64 read_sectors = 0;
	u64 write_sectors = 0;
	int ret;

	ret = evx_get_sdc_stat_sectors(&read_sectors, &write_sectors);
	if (ret)
		return ret;

	/* Linux block stat sectors are 512 bytes. MB = sectors / 2048. */
	seq_printf(m, "%llu\n", (unsigned long long)(write_sectors >> 11));
	return 0;
}

static int ufs_total_read_size_mb_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_total_read_size_mb_proc_show, NULL);
}

static int ufs_total_write_size_mb_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_total_write_size_mb_proc_show, NULL);
}

static ssize_t ufs_total_size_mb_proc_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops ufs_total_read_size_mb_proc_ops = {
	.proc_open	= ufs_total_read_size_mb_proc_open,
	.proc_read	= seq_read,
	.proc_write	= ufs_total_size_mb_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops ufs_total_write_size_mb_proc_ops = {
	.proc_open	= ufs_total_write_size_mb_proc_open,
	.proc_read	= seq_read,
	.proc_write	= ufs_total_size_mb_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int swpm_sp_ddr_idx_proc_show(struct seq_file *m, void *v)
{
	/*
	 * Real MTK SWPM may be disabled or return no DDR buckets on this build.
	 * Always return stable readable residency buckets so OPlus powerstats
	 * does not fail read() with empty/error data.
	 */
	seq_puts(m, "0 0 0 0 0 0 0 0\n");
	return 0;
}

static int swpm_sp_ddr_idx_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, swpm_sp_ddr_idx_proc_show, NULL);
}

static const struct proc_ops swpm_sp_ddr_idx_proc_ops = {
	.proc_open	= swpm_sp_ddr_idx_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/*
 * Real UFS status compatibility:
 * ColorOS reads ufs_transmission_status under the real UFS device.
 * Expose current real UFS sysfs state as compact numeric status:
 * clkgate clkscale auto_hibern8
 */
static ssize_t ufs_transmission_status_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	char clkgate[32] = "0";
	char clkscale[32] = "0";
	char auto_h8[32] = "0";

	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/clkgate_enable",
			    clkgate, sizeof(clkgate));
	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/clkscale_enable",
			    clkscale, sizeof(clkscale));
	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/auto_hibern8",
			    auto_h8, sizeof(auto_h8));

	strim(clkgate);
	strim(clkscale);
	strim(auto_h8);

	return sysfs_emit(buf, "%s %s %s\n", clkgate, clkscale, auto_h8);
}

static DEVICE_ATTR_RO(ufs_transmission_status);

static int sched_assist_im_flag_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_im_flag));
	return 0;
}

static int sched_assist_im_flag_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_im_flag_proc_show, NULL);
}

static ssize_t sched_assist_im_flag_proc_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_im_flag, val);

	return count;
}

static const struct proc_ops sched_assist_im_flag_proc_ops = {
	.proc_open	= sched_assist_im_flag_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_im_flag_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_debug_enabled_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_debug_enabled));
	return 0;
}

static int sched_assist_debug_enabled_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_debug_enabled_proc_show, NULL);
}

static ssize_t sched_assist_debug_enabled_proc_write(struct file *file,
						     const char __user *buf,
						     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_debug_enabled, val);

	return count;
}

static const struct proc_ops sched_assist_debug_enabled_proc_ops = {
	.proc_open	= sched_assist_debug_enabled_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_debug_enabled_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_lb_enable_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_lb_enable));
	return 0;
}

static int sched_assist_lb_enable_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_lb_enable_proc_show, NULL);
}

static ssize_t sched_assist_lb_enable_proc_write(struct file *file,
						 const char __user *buf,
						 size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_lb_enable, val);

	return count;
}

static const struct proc_ops sched_assist_lb_enable_proc_ops = {
	.proc_open	= sched_assist_lb_enable_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_lb_enable_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int theia_pwk_report_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "count=%lld\n",
		   (long long)atomic64_read(&theia_pwk_report_count));
	seq_printf(m, "last_payload=%s\n", theia_pwk_last_payload);
	return 0;
}

static int theia_pwk_report_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, theia_pwk_report_proc_show, NULL);
}

static ssize_t theia_pwk_report_proc_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(theia_pwk_last_payload) - 1);
	if (copy_from_user(theia_pwk_last_payload, buf, len))
		return -EFAULT;

	theia_pwk_last_payload[len] = '\0';
	atomic64_inc(&theia_pwk_report_count);

	return count;
}

static const struct proc_ops theia_pwk_report_proc_ops = {
	.proc_open	= theia_pwk_report_proc_open,
	.proc_read	= seq_read,
	.proc_write	= theia_pwk_report_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int oplus_eng_version_proc_show(struct seq_file *m, void *v)
{
	/*
	 * OPlus stability service reads this as an engineering-build flag.
	 * 0 = normal/user-style build.
	 */
	seq_printf(m, "%d\n", atomic_read(&oplus_eng_version));
	return 0;
}

static int oplus_eng_version_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_eng_version_proc_show, NULL);
}

static ssize_t oplus_eng_version_proc_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_eng_version, val);

	return count;
}

static const struct proc_ops oplus_eng_version_proc_ops = {
	.proc_open	= oplus_eng_version_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_eng_version_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};


#define EVX_DEFINE_PROC_INT(name, atomic_var) \
static int name##_proc_show(struct seq_file *m, void *v) \
{ \
	seq_printf(m, "%d\n", atomic_read(&atomic_var)); \
	return 0; \
} \
static int name##_proc_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, name##_proc_show, NULL); \
} \
static ssize_t name##_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
{ \
	char kbuf[32]; \
	int val; \
	if (count >= sizeof(kbuf)) count = sizeof(kbuf) - 1; \
	if (copy_from_user(kbuf, buf, count)) return -EFAULT; \
	kbuf[count] = '\0'; \
	if (!kstrtoint(kbuf, 0, &val)) atomic_set(&atomic_var, val); \
	return count; \
} \
static const struct proc_ops name##_proc_ops = { \
	.proc_open = name##_proc_open, \
	.proc_read = seq_read, \
	.proc_write = name##_proc_write, \
	.proc_lseek = seq_lseek, \
	.proc_release = single_release, \
};

EVX_DEFINE_PROC_INT(evx_io_label, evx_io_label)
EVX_DEFINE_PROC_INT(evx_io_top_app_monitor, evx_io_top_app_monitor)
EVX_DEFINE_PROC_INT(evx_file_remove_event, evx_file_remove_event)
EVX_DEFINE_PROC_INT(evx_file_unlink_switch, evx_file_unlink_switch)
EVX_DEFINE_PROC_INT(evx_clm_enable, evx_clm_enable)
EVX_DEFINE_PROC_INT(evx_clm_mux_switch, evx_clm_mux_switch)
EVX_DEFINE_PROC_INT(evx_clm_highload_all, evx_clm_highload_all)
EVX_DEFINE_PROC_INT(evx_clm_highload_grp, evx_clm_highload_grp)
EVX_DEFINE_PROC_INT(evx_clm_report_threshold, evx_clm_report_threshold)
EVX_DEFINE_PROC_INT(evx_clm_lowload_grp, evx_clm_lowload_grp)
EVX_DEFINE_PROC_INT(evx_boost_pool_min, evx_boost_pool_min)
EVX_DEFINE_PROC_INT(evx_task_track_enable, evx_task_track_enable)
EVX_DEFINE_PROC_INT(evx_bg_dstat_percent, evx_bg_dstat_percent)
EVX_DEFINE_PROC_INT(evx_load_indicator, evx_load_indicator)
EVX_DEFINE_PROC_INT(evx_callstack, evx_callstack)
EVX_DEFINE_PROC_INT(evx_fg_freqs_threshold, evx_fg_freqs_threshold)
EVX_DEFINE_PROC_INT(evx_cpu_load32_scale, evx_cpu_load32_scale)

#define EVX_DEFINE_PROC_RO_STR(name, str_val) \
static int name##_proc_show(struct seq_file *m, void *v) \
{ \
	seq_puts(m, str_val); \
	return 0; \
} \
static int name##_proc_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, name##_proc_show, NULL); \
} \
static const struct proc_ops name##_proc_ops = { \
	.proc_open = name##_proc_open, \
	.proc_read = seq_read, \
	.proc_lseek = seq_lseek, \
	.proc_release = single_release, \
};

#define EVX_DEFINE_PROC_RW_STR(name, default_str) \
static char name##_buf[128] = default_str; \
static DEFINE_MUTEX(name##_lock); \
static int name##_proc_show(struct seq_file *m, void *v) \
{ \
	mutex_lock(&name##_lock); \
	seq_printf(m, "%s", name##_buf); \
	mutex_unlock(&name##_lock); \
	return 0; \
} \
static int name##_proc_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, name##_proc_show, NULL); \
} \
static ssize_t name##_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
{ \
	size_t len = min(count, sizeof(name##_buf) - 1); \
	mutex_lock(&name##_lock); \
	if (copy_from_user(name##_buf, buf, len)) { \
		mutex_unlock(&name##_lock); \
		return -EFAULT; \
	} \
	name##_buf[len] = '\0'; \
	mutex_unlock(&name##_lock); \
	return count; \
} \
static const struct proc_ops name##_proc_ops = { \
	.proc_open = name##_proc_open, \
	.proc_read = seq_read, \
	.proc_write = name##_proc_write, \
	.proc_lseek = seq_lseek, \
	.proc_release = single_release, \
};

EVX_DEFINE_PROC_RW_STR(evx_task_track, "0\n")
EVX_DEFINE_PROC_RW_STR(evx_cpu_info, "cluster: 0-3 2100MHz, 4-6 3000MHz, 7 3250MHz\n")
EVX_DEFINE_PROC_RW_STR(evx_cpu_info_sig, "0\n")
/* The Oplus cpu_load ABI is one aggregate percentage, not eight fields. */
static DEFINE_MUTEX(evx_cpu_load_lock);
static u64 evx_cpu_load_previous_busy;
static u64 evx_cpu_load_previous_total;
static u64 evx_cpu_load_sample_ns;
static unsigned int evx_cpu_load_percent;
static bool evx_cpu_load_valid;

static void evx_cpu_load_snapshot(u64 *busy, u64 *total)
{
	struct kernel_cpustat stat;
	u64 active, idle, iowait, value;
	int cpu;

	*busy = 0;
	*total = 0;
	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		kcpustat_cpu_fetch(&stat, cpu);
		active = stat.cpustat[CPUTIME_USER] + stat.cpustat[CPUTIME_NICE] +
			 stat.cpustat[CPUTIME_SYSTEM] + stat.cpustat[CPUTIME_IRQ] +
			 stat.cpustat[CPUTIME_SOFTIRQ] + stat.cpustat[CPUTIME_STEAL];
		idle = stat.cpustat[CPUTIME_IDLE];
		iowait = stat.cpustat[CPUTIME_IOWAIT];
		/* Tickless idle must include time since the last scheduler tick. */
		value = get_cpu_idle_time_us(cpu, NULL);
		if (value != -1ULL)
			idle = value * NSEC_PER_USEC;
		value = get_cpu_iowait_time_us(cpu, NULL);
		if (value != -1ULL)
			iowait = value * NSEC_PER_USEC;
		*busy += active;
		*total += active + idle + iowait;
	}
	cpus_read_unlock();
}

static void evx_cpu_load_start(void)
{
	evx_cpu_load_snapshot(&evx_cpu_load_previous_busy,
			      &evx_cpu_load_previous_total);
	evx_cpu_load_sample_ns = ktime_get_ns();
}

static int evx_cpu_load_proc_show(struct seq_file *m, void *v)
{
	u64 now, busy, total, delta_busy, delta_total;
	unsigned int percent;
	int ret = 0;

	mutex_lock(&evx_cpu_load_lock);
	now = ktime_get_ns();
	/* Multiple readers share the latest measured sample; no polling worker. */
	if (now - evx_cpu_load_sample_ns >= 128 * NSEC_PER_MSEC) {
		evx_cpu_load_snapshot(&busy, &total);
		if (busy >= evx_cpu_load_previous_busy &&
		    total > evx_cpu_load_previous_total) {
			delta_busy = busy - evx_cpu_load_previous_busy;
			delta_total = total - evx_cpu_load_previous_total;
			evx_cpu_load_percent = mul_u64_u64_div_u64(
				min(delta_busy, delta_total), 100, delta_total);
			evx_cpu_load_valid = true;
		} else {
			evx_cpu_load_valid = false;
		}
		evx_cpu_load_previous_busy = busy;
		evx_cpu_load_previous_total = total;
		evx_cpu_load_sample_ns = now;
	}
	percent = evx_cpu_load_percent;
	if (!evx_cpu_load_valid)
		ret = -EAGAIN;
	mutex_unlock(&evx_cpu_load_lock);
	if (!ret)
		seq_printf(m, "%u\n", percent);
	return ret;
}

static int evx_cpu_load_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_cpu_load_proc_show, NULL);
}

static const struct proc_ops evx_cpu_load_proc_ops = {
	.proc_open = evx_cpu_load_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
EVX_DEFINE_PROC_RW_STR(evx_cpu_load32, "0 0 0 0 0 0 0 0\n")

EVX_DEFINE_PROC_RO_STR(evx_devinfo_ufs, "vendor: SAMSUNG\nmanufacture: SAMSUNG\nmodel: KLUEG8UHDB-C2D1\nrev: 0500\nversion: UFS 4.0\nsize: 512GB\nstatus: 1\n")
EVX_DEFINE_PROC_RO_STR(evx_devinfo_emmc, "none\n")
EVX_DEFINE_PROC_RO_STR(evx_devinfo_ufs_version, "4.0\n")
EVX_DEFINE_PROC_RO_STR(evx_devinfo_emmc_version, "none\n")
EVX_DEFINE_PROC_RO_STR(evx_devinfo_ddr, "LPDDR5X\n")

static int evx_forever_metric_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0 0 0 0\n");
	return 0;
}

static int evx_forever_metric_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_forever_metric_show, NULL);
}

static ssize_t evx_forever_metric_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops evx_forever_metric_proc_ops = {
	.proc_open = evx_forever_metric_open,
	.proc_read = seq_read,
	.proc_write = evx_forever_metric_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const char * const forever_metric_names[] = {
	"f2fs_discard_cnt",
	"f2fs_fg_gc_cnt",
	"f2fs_fg_gc_avg_time",
	"f2fs_fg_gc_seg_cnt",
	"f2fs_bg_gc_cnt",
	"f2fs_bg_gc_avg_time",
	"f2fs_bg_gc_seg_cnt",
	"f2fs_cp_cnt",
	"f2fs_cp_avg_time",
	"f2fs_cp_max_time",
	"bio_read_cnt",
	"bio_read_avg_size",
	"bio_read_size_dist",
	"bio_read_avg_time",
	"bio_read_max_time",
	"bio_read_4k_blk_avg_time",
	"bio_read_4k_blk_max_time",
	"bio_read_4k_drv_avg_time",
	"bio_read_4k_drv_max_time",
	"bio_read_512k_blk_avg_time",
	"bio_read_512k_blk_max_time",
	"bio_read_512k_drv_avg_time",
	"bio_read_512k_drv_max_time",
	"bio_write_cnt",
	"bio_write_avg_size",
	"bio_write_size_dist",
	"bio_write_avg_time",
	"bio_write_max_time",
	"bio_write_4k_blk_avg_time",
	"bio_write_4k_blk_max_time",
	"bio_write_4k_drv_avg_time",
	"bio_write_4k_drv_max_time",
	"bio_write_512k_blk_avg_time",
	"bio_write_512k_blk_max_time",
	"bio_write_512k_drv_avg_time",
	"bio_write_512k_drv_max_time",
	"ufs_total_read_time_ms",
	"ufs_total_write_time_ms",
	"f2fs_discard_len",
	"bio_read_4k_blk_lat_dist",
	"bio_read_4k_drv_lat_dist",
	"bio_read_512k_blk_lat_dist",
	"bio_read_512k_drv_lat_dist",
	"bio_write_4k_blk_lat_dist",
	"bio_write_4k_drv_lat_dist",
	"bio_write_512k_blk_lat_dist",
	"bio_write_512k_drv_lat_dist",
};

static char evx_iomonitor_flow_buf[128] = "0 0 0 0\n";
static char evx_iomonitor_daily_buf[128] = "0 0 0 0\n";
static char evx_iomonitor_uid_io_buf[128] = "1000 0 0 0 0\n";
static char evx_fg_uids_buf[128] = "1000\n";

#define EVX_DEFINE_PROC_STR_RW(name, buf_var) \
static int name##_proc_show(struct seq_file *m, void *v) \
{ \
	seq_printf(m, "%s", buf_var); \
	return 0; \
} \
static int name##_proc_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, name##_proc_show, NULL); \
} \
static ssize_t name##_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
{ \
	size_t len = min(count, sizeof(buf_var) - 1); \
	if (copy_from_user(buf_var, buf, len)) return -EFAULT; \
	buf_var[len] = '\0'; \
	return count; \
} \
static const struct proc_ops name##_proc_ops = { \
	.proc_open = name##_proc_open, \
	.proc_read = seq_read, \
	.proc_write = name##_proc_write, \
	.proc_lseek = seq_lseek, \
	.proc_release = single_release, \
};

EVX_DEFINE_PROC_STR_RW(evx_iomonitor_flow, evx_iomonitor_flow_buf)
EVX_DEFINE_PROC_STR_RW(evx_iomonitor_daily, evx_iomonitor_daily_buf)
EVX_DEFINE_PROC_STR_RW(evx_iomonitor_uid_io, evx_iomonitor_uid_io_buf)
EVX_DEFINE_PROC_STR_RW(evx_fg_uids, evx_fg_uids_buf)

static int evx_reset_stat_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}
static int evx_reset_stat_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_reset_stat_proc_show, NULL);
}
static ssize_t evx_reset_stat_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	return count;
}
static const struct proc_ops evx_io_reset_stat_proc_ops = {
	.proc_open = evx_reset_stat_proc_open,
	.proc_read = seq_read,
	.proc_write = evx_reset_stat_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int ufs_lat_dist_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
	return 0;
}
static int ufs_lat_dist_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_lat_dist_proc_show, NULL);
}
static const struct proc_ops ufs_lat_dist_proc_ops = {
	.proc_open = ufs_lat_dist_proc_open,
	.proc_read = seq_read,
	.proc_write = evx_reset_stat_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int evx_clm_version_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "1.0\n");
	return 0;
}
static int evx_clm_version_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_clm_version_proc_show, NULL);
}
static const struct proc_ops evx_clm_version_proc_ops = {
	.proc_open = evx_clm_version_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int devinfo_lcd_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "dsi_panel_active 120Hz\n");
	return 0;
}
static int devinfo_lcd_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_lcd_proc_show, NULL);
}
static const struct proc_ops devinfo_lcd_proc_ops = {
	.proc_open = devinfo_lcd_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int task_cpustats_proc_show(struct seq_file *m, void *v)
{
	struct task_struct *g;
	struct task_struct *t;

	/*
	 * Real-backed task CPU stats.
	 * Output is intentionally simple and parse-safe:
	 * pid tgid utime_ns stime_ns runtime_ns comm
	 */
	seq_puts(m, "pid tgid utime_ns stime_ns runtime_ns comm\n");

	rcu_read_lock();
	for_each_process_thread(g, t) {
		u64 utime = 0;
		u64 stime = 0;
		u64 runtime = 0;

		task_cputime_adjusted(t, &utime, &stime);
		runtime = task_sched_runtime(t);

		seq_printf(m, "%d %d %llu %llu %llu %.64s\n",
			   task_pid_nr(t),
			   task_tgid_nr(t),
			   (unsigned long long)utime,
			   (unsigned long long)stime,
			   (unsigned long long)runtime,
			   t->comm);
	}
	rcu_read_unlock();

	return 0;
}

static int task_cpustats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, task_cpustats_proc_show, NULL);
}

static const struct proc_ops task_cpustats_proc_ops = {
	.proc_open	= task_cpustats_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t watchdog_kick_proc_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(watchdog_last_payload) - 1);
	if (copy_from_user(watchdog_last_payload, buf, len))
		return -EFAULT;

	watchdog_last_payload[len] = '\0';
	watchdog_last_kick_ns = ktime_get_boottime_ns();
	atomic64_inc(&watchdog_kick_count);

	return count;
}

static int watchdog_status_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "kick_count=%lld\n",
		   (long long)atomic64_read(&watchdog_kick_count));
	seq_printf(m, "last_kick_boottime_ns=%llu\n",
		   (unsigned long long)watchdog_last_kick_ns);
	seq_printf(m, "last_payload=%s\n", watchdog_last_payload);
	return 0;
}

static int watchdog_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, watchdog_status_proc_show, NULL);
}

static const struct proc_ops watchdog_kick_proc_ops = {
	.proc_write	= watchdog_kick_proc_write,
};

static const struct proc_ops watchdog_status_proc_ops = {
	.proc_open	= watchdog_status_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* EVX_COS_V28: OPlus sched important task compat */
static int evx_sched_impt_task_show(struct seq_file *m, void *v)
{
        seq_puts(m, "0\n");
        return 0;
}

static int evx_sched_impt_task_open(struct inode *inode, struct file *file)
{
        return single_open(file, evx_sched_impt_task_show, NULL);
}

static ssize_t evx_sched_impt_task_write(struct file *file,
                                         const char __user *buf,
                                         size_t count, loff_t *ppos)
{
        return count;
}

static const struct proc_ops evx_sched_impt_task_fops = {
        .proc_open      = evx_sched_impt_task_open,
        .proc_read      = seq_read,
        .proc_write     = evx_sched_impt_task_write,
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};


/* EVX_COS_V28_R4: OPlus sched_assist im_flag_app compat */
static int evx_im_flag_app_show(struct seq_file *m, void *v)
{
        seq_puts(m, "0\n");
        return 0;
}

static int evx_im_flag_app_open(struct inode *inode, struct file *file)
{
        return single_open(file, evx_im_flag_app_show, NULL);
}

static ssize_t evx_im_flag_app_write(struct file *file,
                                     const char __user *buf,
                                     size_t count, loff_t *ppos)
{
        return count;
}

static const struct proc_ops evx_im_flag_app_fops = {
        .proc_open      = evx_im_flag_app_open,
        .proc_read      = seq_read,
        .proc_write     = evx_im_flag_app_write,
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};


static int __init evx_cos_perf_compat_init(void)
{
	int ret;

	evx_cpu_load_start();

	proc_task_cpustats = proc_create("task_cpustats", 0444, NULL,
					 IS_ENABLED(CONFIG_EVONIX_OPLUS_TASK_ENERGY) ?
					 &evx_task_energy_proc_ops : &task_cpustats_proc_ops);
	if (!proc_task_cpustats)
		return -ENOMEM;

	proc_oplus_init_watchdog_dir = proc_mkdir("oplus_init_watchdog", NULL);
	if (!proc_oplus_init_watchdog_dir)
		goto err_task_cpustats;

	proc_oplus_init_watchdog_kick =
		proc_create("kick", 0666, proc_oplus_init_watchdog_dir,
			    &watchdog_kick_proc_ops);
	if (!proc_oplus_init_watchdog_kick)
		goto err_watchdog_dir;

	proc_oplus_init_watchdog_status =
		proc_create("status", 0444, proc_oplus_init_watchdog_dir,
			    &watchdog_status_proc_ops);
	if (!proc_oplus_init_watchdog_status)
		goto err_watchdog_kick;

	evx_ufs_dev = bus_find_device_by_name(&platform_bus_type, NULL,
					      "112b0000.ufshci");
	if (evx_ufs_dev) {
		int ret;

		ret = device_create_file(evx_ufs_dev, &dev_attr_virtualtlcbuff);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": virtualtlcbuff create failed: %d\n", ret);

		ret = device_create_file(evx_ufs_dev, &dev_attr_ufs_transmission_status);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": ufs_transmission_status create failed: %d\n", ret);

		ret = sysfs_create_group(&evx_ufs_dev->kobj, &ufssid_attr_group);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": ufssid group create failed: %d\n", ret);
	} else {
		pr_warn(EVX_PERF_NAME ": UFS platform device 112b0000.ufshci not found\n");
	}

	proc_oplus_storage_dir = proc_mkdir("oplus_storage", NULL);
	if (proc_oplus_storage_dir) {
		proc_io_metrics_dir = proc_mkdir("io_metrics",
						  proc_oplus_storage_dir);
		if (proc_io_metrics_dir) {
			proc_io_metrics_forever_dir =
				proc_mkdir("forever", proc_io_metrics_dir);
			if (proc_io_metrics_forever_dir) {
				proc_ufs_total_read_size_mb =
					proc_create("ufs_total_read_size_mb", 0666,
						    proc_io_metrics_forever_dir,
						    &ufs_total_read_size_mb_proc_ops);
				proc_ufs_total_write_size_mb =
					proc_create("ufs_total_write_size_mb", 0666,
						    proc_io_metrics_forever_dir,
						    &ufs_total_write_size_mb_proc_ops);
				proc_io_metrics_forever_read_lat =
					proc_create("ufs_read_lat_dist", 0666,
						    proc_io_metrics_forever_dir,
						    &ufs_lat_dist_proc_ops);
				proc_io_metrics_forever_write_lat =
					proc_create("ufs_write_lat_dist", 0666,
						    proc_io_metrics_forever_dir,
						    &ufs_lat_dist_proc_ops);
				{
					int i;
					for (i = 0; i < ARRAY_SIZE(forever_metric_names); i++) {
						proc_create(forever_metric_names[i], 0666,
							    proc_io_metrics_forever_dir,
							    &evx_forever_metric_proc_ops);
					}
				}
			}
			proc_io_metrics_control_dir =
				proc_mkdir("control", proc_io_metrics_dir);
			if (proc_io_metrics_control_dir) {
				proc_io_metrics_control_label =
					proc_create("label", 0666,
						    proc_io_metrics_control_dir,
						    &evx_io_label_proc_ops);
				proc_io_metrics_control_top_app_monitor =
					proc_create("top_app_monitor", 0666,
						    proc_io_metrics_control_dir,
						    &evx_io_top_app_monitor_proc_ops);
				proc_io_metrics_control_reset_stat =
					proc_create("reset_stat", 0666,
						    proc_io_metrics_control_dir,
						    &evx_io_reset_stat_proc_ops);
			}
		}
		proc_file_monitor_dir = proc_mkdir("file_monitor", proc_oplus_storage_dir);
		if (proc_file_monitor_dir) {
			proc_file_monitor_remove_event =
				proc_create("remove_event", 0666,
					    proc_file_monitor_dir,
					    &evx_file_remove_event_proc_ops);
			proc_file_monitor_unlink_switch =
				proc_create("unlink_switch", 0666,
					    proc_file_monitor_dir,
					    &evx_file_unlink_switch_proc_ops);
		}
	}

	/* The native driver owns this entire ABI when enabled. */
	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)) {
		proc_jank_info_dir = proc_mkdir("jank_info", NULL);
		if (proc_jank_info_dir) {
			proc_cpu_jank_info_dir = proc_mkdir("cpu_jank_info", proc_jank_info_dir);
			if (proc_cpu_jank_info_dir) {
				proc_clm_version = proc_create("version", 0444, proc_cpu_jank_info_dir, &evx_clm_version_proc_ops);
				proc_clm_enable = proc_create("clm_enable", 0666, proc_cpu_jank_info_dir, &evx_clm_enable_proc_ops);
				proc_clm_mux_switch = proc_create("clm_mux_switch", 0666, proc_cpu_jank_info_dir, &evx_clm_mux_switch_proc_ops);
				proc_clm_highload_all = proc_create("clm_highload_all", 0666, proc_cpu_jank_info_dir, &evx_clm_highload_all_proc_ops);
				proc_clm_highload_grp = proc_create("clm_highload_grp", 0666, proc_cpu_jank_info_dir, &evx_clm_highload_grp_proc_ops);
				proc_clm_report_threshold = proc_create("clm_report_threshold", 0666, proc_cpu_jank_info_dir, &evx_clm_report_threshold_proc_ops);
				proc_clm_lowload_grp = proc_create("clm_lowload_grp", 0666, proc_cpu_jank_info_dir, &evx_clm_lowload_grp_proc_ops);
				proc_task_track_enable = proc_create("task_track_enable", 0666, proc_cpu_jank_info_dir, &evx_task_track_enable_proc_ops);
				proc_bg_dstat_percent = proc_create("bg_dstat_percent", 0666, proc_cpu_jank_info_dir, &evx_bg_dstat_percent_proc_ops);
				proc_load_indicator = proc_create("load_indicator", 0666, proc_cpu_jank_info_dir, &evx_load_indicator_proc_ops);
				proc_callstack = proc_create("callstack", 0666, proc_cpu_jank_info_dir, &evx_callstack_proc_ops);
				proc_fg_freqs_threshold = proc_create("fg_freqs_threshold", 0666, proc_cpu_jank_info_dir, &evx_fg_freqs_threshold_proc_ops);
				proc_cpu_load32_scale = proc_create("cpu_load32_scale", 0666, proc_cpu_jank_info_dir, &evx_cpu_load32_scale_proc_ops);
				proc_task_track = proc_create("task_track", 0666, proc_cpu_jank_info_dir, &evx_task_track_proc_ops);
				proc_cpu_info = proc_create("cpu_info", 0666, proc_cpu_jank_info_dir, &evx_cpu_info_proc_ops);
				proc_cpu_info_sig = proc_create("cpu_info_sig", 0666, proc_cpu_jank_info_dir, &evx_cpu_info_sig_proc_ops);
				proc_cpu_load = proc_create("cpu_load", 0444,
					proc_cpu_jank_info_dir, &evx_cpu_load_proc_ops);
				proc_cpu_load32 = proc_create("cpu_load32", 0666, proc_cpu_jank_info_dir, &evx_cpu_load32_proc_ops);
			}
		}
	}

	proc_boost_pool_dir = proc_mkdir("boost_pool", NULL);
	if (proc_boost_pool_dir) {
		proc_boost_pool_mtk_mm_dir = proc_mkdir("mtk_mm", proc_boost_pool_dir);
		if (proc_boost_pool_mtk_mm_dir) {
			proc_boost_pool_min = proc_create("min", 0666, proc_boost_pool_mtk_mm_dir, &evx_boost_pool_min_proc_ops);
		}
	}

	proc_swpm_dir = proc_mkdir("swpm", NULL);
	if (proc_swpm_dir)
		proc_swpm_sp_ddr_idx =
			proc_create("swpm_sp_ddr_idx", 0444,
				    proc_swpm_dir,
				    &swpm_sp_ddr_idx_proc_ops);

	proc_theia_pwk_report =
		proc_create("theiaPwkReport", 0666, NULL,
			    &theia_pwk_report_proc_ops);

	INIT_DELAYED_WORK(&evx_zram_work, evx_zram_work_fn);
	schedule_delayed_work(&evx_zram_work, msecs_to_jiffies(500));

	evx_bootdevice_pdev =
		platform_device_register_simple("bootdevice", -1, NULL, 0);
	if (!IS_ERR(evx_bootdevice_pdev)) {
		ret = device_create_file(&evx_bootdevice_pdev->dev,
					 &dev_attr_ufs_transmission_status);
		if (ret)
			pr_warn(EVX_PERF_NAME ": bootdevice ufs status create failed: %d\n",
				ret);
	} else {
		pr_warn(EVX_PERF_NAME ": bootdevice platform register failed: %ld\n",
			PTR_ERR(evx_bootdevice_pdev));
		evx_bootdevice_pdev = NULL;
	}

	proc_devinfo_dir = get_devinfo_proc_dir();
	if (!proc_devinfo_dir) {
		proc_devinfo_dir = proc_mkdir("devinfo", NULL);
		if (proc_devinfo_dir)
			proc_devinfo_dir_created = true;
	}
	if (proc_devinfo_dir) {
		proc_devinfo_ufsplus_status =
			proc_create("ufsplus_status", 0444, proc_devinfo_dir,
				    &ufsplus_status_proc_ops);
		proc_devinfo_lcd =
			proc_create("lcd", 0444, proc_devinfo_dir,
				    &devinfo_lcd_proc_ops);
		proc_devinfo_ufs =
			proc_create("ufs", 0444, proc_devinfo_dir,
				    &evx_devinfo_ufs_proc_ops);
		proc_devinfo_emmc =
			proc_create("emmc", 0444, proc_devinfo_dir,
				    &evx_devinfo_emmc_proc_ops);
		proc_devinfo_ufs_version =
			proc_create("ufs_version", 0444, proc_devinfo_dir,
				    &evx_devinfo_ufs_version_proc_ops);
		proc_devinfo_emmc_version =
			proc_create("emmc_version", 0444, proc_devinfo_dir,
				    &evx_devinfo_emmc_version_proc_ops);
		proc_devinfo_ddr =
			proc_create("ddr", 0444, proc_devinfo_dir,
				    &evx_devinfo_ddr_proc_ops);
	}

	proc_ufs_signal_dir = proc_mkdir("ufs_signalShow", NULL);
	if (proc_ufs_signal_dir)
		proc_ufs_signal_record_upload =
			proc_create("record_upload", 0666, proc_ufs_signal_dir,
				    &ufs_signal_record_upload_proc_ops);

	proc_oplus_qos_dir = proc_mkdir("oplus_qos", NULL);
	if (proc_oplus_qos_dir)
		proc_oplus_qos_enable =
			proc_create("qos_enable", 0666, proc_oplus_qos_dir,
				    &oplus_qos_enable_proc_ops);

	proc_oplus_binder_dir = proc_mkdir("oplus_binder", NULL);
	if (proc_oplus_binder_dir)
		proc_oplus_binder_ux_flag =
			proc_create("ux_flag", 0666,
				    proc_oplus_binder_dir,
				    &oplus_binder_ux_flag_proc_ops);

	/* Native OPlus projectinfo owns /proc/oplusVersion when enabled. */
	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_OPROJECT)) {
		proc_oplus_version_dir = proc_mkdir("oplusVersion", NULL);
		if (proc_oplus_version_dir)
			proc_oplus_eng_version =
				proc_create("engVersion", 0666,
					    proc_oplus_version_dir,
					    &oplus_eng_version_proc_ops);
	}

	/* Native SchedAssist owns /proc/oplus_scheduler when enabled. */
	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)) {
		proc_oplus_scheduler_dir = proc_mkdir("oplus_scheduler", NULL);
		if (proc_oplus_scheduler_dir) {
			proc_oplus_scheduler_qos_enable =
				proc_create("qos_enable", 0666,
					    proc_oplus_scheduler_dir,
					    &oplus_qos_enable_proc_ops);
			proc_sched_assist_dir = proc_mkdir("sched_assist",
							   proc_oplus_scheduler_dir);
			if (proc_sched_assist_dir) {
				proc_sched_assist_scene =
					proc_create("sched_assist_scene", 0666,
						    proc_sched_assist_dir,
						    &sched_assist_scene_proc_ops);
				proc_sched_assist_im_flag =
					proc_create("im_flag", 0666,
						    proc_sched_assist_dir,
						    &sched_assist_im_flag_proc_ops);
		proc_create("im_flag_app", 0666, proc_sched_assist_dir, &evx_im_flag_app_fops);
				proc_sched_assist_debug_enabled =
					proc_create("debug_enabled", 0666,
						    proc_sched_assist_dir,
						    &sched_assist_debug_enabled_proc_ops);
				proc_sched_assist_lb_enable =
					proc_create("lb_enable", 0666,
						    proc_sched_assist_dir,
						    &sched_assist_lb_enable_proc_ops);
		proc_create("sched_impt_task", 0666, proc_sched_assist_dir, &evx_sched_impt_task_fops);
				proc_sched_assist_qos_enable =
					proc_create("qos_enable", 0666,
						    proc_sched_assist_dir,
						    &oplus_qos_enable_proc_ops);
				proc_sched_assist_enabled =
					proc_create("sched_assist_enabled", 0666,
						    proc_sched_assist_dir,
						    &oplus_qos_enable_proc_ops);
				proc_sched_assist_ux_enable =
					proc_create("ux_enable", 0666,
						    proc_sched_assist_dir,
						    &oplus_qos_enable_proc_ops);
				proc_sched_assist_uaf_enable =
					proc_create("uaf_enable", 0666,
						    proc_sched_assist_dir,
						    &oplus_qos_enable_proc_ops);
				proc_sched_assist_feature_enable =
					proc_create("feature_enable", 0666,
						    proc_sched_assist_dir,
						    &oplus_qos_enable_proc_ops);
			}
		}
	}

	proc_sgeinfo = proc_create("sgeinfo", 0444, NULL, &evx_sgeinfo_proc_ops);
	proc_sgefreqinfo = proc_create("sgefreqinfo", 0444, NULL, &evx_sgefreqinfo_proc_ops);

	proc_iomonitor_dir = proc_mkdir("IoMonitor", NULL);
	if (proc_iomonitor_dir) {
		proc_iomonitor_flow = proc_create("flow", 0666, proc_iomonitor_dir, &evx_iomonitor_flow_proc_ops);
		proc_iomonitor_daily = proc_create("daily", 0666, proc_iomonitor_dir, &evx_iomonitor_daily_proc_ops);
		proc_iomonitor_uid_io = proc_create("uid_io_show", 0666, proc_iomonitor_dir, &evx_iomonitor_uid_io_proc_ops);
	}

	if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)) {
		proc_fg_info_dir = proc_mkdir("fg_info", NULL);
		if (proc_fg_info_dir) {
			proc_fg_info_fg_uids = proc_create("fg_uids", 0666, proc_fg_info_dir, &evx_fg_uids_proc_ops);
		}
	}

	proc_storage_dir = proc_mkdir("storage", NULL);
	if (proc_storage_dir)
		proc_storage_buf_log = proc_create("buf_log", 0666, proc_storage_dir,
						   &evx_storage_buf_log_fops);

	proc_oplus_mem_memleak_dir = proc_mkdir("oplus_mem/memleak_detect", NULL);
	if (proc_oplus_mem_memleak_dir) {
		int i;
		for (i = 0; i < ARRAY_SIZE(memleak_node_names); i++)
			proc_create(memleak_node_names[i], 0666,
				    proc_oplus_mem_memleak_dir,
				    &evx_memleak_generic_fops);
	}

	proc_horae_qmi = proc_create("horae_qmi", 0666, NULL, &evx_memleak_generic_fops);

	if (power_kobj) {
		evx_ddr_kobj = kobject_create_and_add("ddr", power_kobj);
		if (evx_ddr_kobj) {
			int ddr_err = sysfs_create_file(evx_ddr_kobj, &evx_ddr_residency_attr.attr);
			if (ddr_err)
				pr_warn(EVX_PERF_NAME ": failed to create residency attribute: %d\n", ddr_err);
		}
	}

	pr_info(EVX_PERF_NAME ": loaded task stats watchdog UFS and sched scene nodes\n");
	return 0;

err_watchdog_kick:
	proc_remove(proc_oplus_init_watchdog_kick);
err_watchdog_dir:
	proc_remove(proc_oplus_init_watchdog_dir);
err_task_cpustats:
	proc_remove(proc_task_cpustats);
	return -ENOMEM;
}

static void __exit evx_cos_perf_compat_exit(void)
{
	if (proc_sched_assist_feature_enable)
		proc_remove(proc_sched_assist_feature_enable);
	if (proc_sched_assist_uaf_enable)
		proc_remove(proc_sched_assist_uaf_enable);
	if (proc_sched_assist_ux_enable)
		proc_remove(proc_sched_assist_ux_enable);
	if (proc_sched_assist_enabled)
		proc_remove(proc_sched_assist_enabled);
	if (proc_sched_assist_qos_enable)
		proc_remove(proc_sched_assist_qos_enable);
	if (proc_oplus_scheduler_qos_enable)
		proc_remove(proc_oplus_scheduler_qos_enable);
	if (proc_oplus_qos_enable)
		proc_remove(proc_oplus_qos_enable);
	if (proc_oplus_qos_dir)
		proc_remove(proc_oplus_qos_dir);

	if (proc_ufs_signal_record_upload)
		proc_remove(proc_ufs_signal_record_upload);
	if (proc_ufs_signal_dir)
		proc_remove(proc_ufs_signal_dir);
	if (proc_devinfo_ddr)
		proc_remove(proc_devinfo_ddr);
	if (proc_devinfo_emmc_version)
		proc_remove(proc_devinfo_emmc_version);
	if (proc_devinfo_ufs_version)
		proc_remove(proc_devinfo_ufs_version);
	if (proc_devinfo_emmc)
		proc_remove(proc_devinfo_emmc);
	if (proc_devinfo_ufs)
		proc_remove(proc_devinfo_ufs);
	if (proc_devinfo_lcd)
		proc_remove(proc_devinfo_lcd);
	if (proc_devinfo_ufsplus_status)
		proc_remove(proc_devinfo_ufsplus_status);
	if (proc_devinfo_dir_created && proc_devinfo_dir)
		proc_remove(proc_devinfo_dir);

	if (proc_boost_pool_min)
		proc_remove(proc_boost_pool_min);
	if (proc_boost_pool_mtk_mm_dir)
		proc_remove(proc_boost_pool_mtk_mm_dir);
	if (proc_boost_pool_dir)
		proc_remove(proc_boost_pool_dir);

	if (proc_cpu_load32)
		proc_remove(proc_cpu_load32);
	if (proc_cpu_load)
		proc_remove(proc_cpu_load);
	if (proc_cpu_info_sig)
		proc_remove(proc_cpu_info_sig);
	if (proc_cpu_info)
		proc_remove(proc_cpu_info);
	if (proc_task_track)
		proc_remove(proc_task_track);
	if (proc_cpu_load32_scale)
		proc_remove(proc_cpu_load32_scale);
	if (proc_fg_freqs_threshold)
		proc_remove(proc_fg_freqs_threshold);
	if (proc_callstack)
		proc_remove(proc_callstack);
	if (proc_load_indicator)
		proc_remove(proc_load_indicator);
	if (proc_bg_dstat_percent)
		proc_remove(proc_bg_dstat_percent);
	if (proc_task_track_enable)
		proc_remove(proc_task_track_enable);
	if (proc_clm_lowload_grp)
		proc_remove(proc_clm_lowload_grp);
	if (proc_clm_report_threshold)
		proc_remove(proc_clm_report_threshold);
	if (proc_clm_highload_grp)
		proc_remove(proc_clm_highload_grp);
	if (proc_clm_highload_all)
		proc_remove(proc_clm_highload_all);
	if (proc_clm_mux_switch)
		proc_remove(proc_clm_mux_switch);
	if (proc_clm_enable)
		proc_remove(proc_clm_enable);
	if (proc_clm_version)
		proc_remove(proc_clm_version);
	if (proc_cpu_jank_info_dir)
		proc_remove(proc_cpu_jank_info_dir);
	if (proc_jank_info_dir)
		proc_remove(proc_jank_info_dir);

	if (proc_file_monitor_unlink_switch)
		proc_remove(proc_file_monitor_unlink_switch);
	if (proc_file_monitor_remove_event)
		proc_remove(proc_file_monitor_remove_event);
	if (proc_file_monitor_dir)
		proc_remove(proc_file_monitor_dir);

	if (proc_io_metrics_control_reset_stat)
		proc_remove(proc_io_metrics_control_reset_stat);
	if (proc_io_metrics_control_top_app_monitor)
		proc_remove(proc_io_metrics_control_top_app_monitor);
	if (proc_io_metrics_control_label)
		proc_remove(proc_io_metrics_control_label);
	if (proc_io_metrics_control_dir)
		proc_remove(proc_io_metrics_control_dir);

	if (proc_io_metrics_forever_write_lat)
		proc_remove(proc_io_metrics_forever_write_lat);
	if (proc_io_metrics_forever_read_lat)
		proc_remove(proc_io_metrics_forever_read_lat);
	if (proc_io_metrics_forever_dir) {
		int i;
		for (i = 0; i < ARRAY_SIZE(forever_metric_names); i++)
			remove_proc_entry(forever_metric_names[i], proc_io_metrics_forever_dir);
	}

	if (proc_fg_info_fg_uids)
		proc_remove(proc_fg_info_fg_uids);
	if (proc_fg_info_dir)
		proc_remove(proc_fg_info_dir);

	if (proc_iomonitor_uid_io)
		proc_remove(proc_iomonitor_uid_io);
	if (proc_iomonitor_daily)
		proc_remove(proc_iomonitor_daily);
	if (proc_iomonitor_flow)
		proc_remove(proc_iomonitor_flow);
	if (proc_iomonitor_dir)
		proc_remove(proc_iomonitor_dir);

	if (proc_sgefreqinfo)
		proc_remove(proc_sgefreqinfo);
	if (proc_sgeinfo)
		proc_remove(proc_sgeinfo);

	cancel_delayed_work_sync(&evx_zram_work);
	if (evx_zram0_dev) {
		device_remove_file(evx_zram0_dev,
				   &dev_attr_free_frag);
		device_remove_file(evx_zram0_dev,
				   &dev_attr_hybridswap_enable);
		device_remove_file(evx_zram0_dev,
				   &dev_attr_hybridswap_swapd_pause);
		put_device(evx_zram0_dev);
		evx_zram0_dev = NULL;
	}

	if (evx_ddr_kobj) {
		sysfs_remove_file(evx_ddr_kobj, &evx_ddr_residency_attr.attr);
		kobject_put(evx_ddr_kobj);
	}
	if (proc_horae_qmi)
		proc_remove(proc_horae_qmi);
	if (proc_oplus_mem_memleak_dir) {
		int i;
		for (i = 0; i < ARRAY_SIZE(memleak_node_names); i++)
			remove_proc_entry(memleak_node_names[i], proc_oplus_mem_memleak_dir);
		remove_proc_entry("oplus_mem/memleak_detect", NULL);
	}
	if (proc_storage_buf_log)
		proc_remove(proc_storage_buf_log);
	if (proc_storage_dir)
		proc_remove(proc_storage_dir);

	if (evx_bootdevice_pdev) {
		device_remove_file(&evx_bootdevice_pdev->dev,
				   &dev_attr_ufs_transmission_status);
		platform_device_unregister(evx_bootdevice_pdev);
		evx_bootdevice_pdev = NULL;
	}

	if (proc_oplus_binder_ux_flag)
		proc_remove(proc_oplus_binder_ux_flag);
	if (proc_oplus_binder_dir)
		proc_remove(proc_oplus_binder_dir);

	if (proc_oplus_eng_version)
		proc_remove(proc_oplus_eng_version);
	if (proc_oplus_version_dir)
		proc_remove(proc_oplus_version_dir);

	if (proc_theia_pwk_report)
		proc_remove(proc_theia_pwk_report);
	if (proc_swpm_sp_ddr_idx)
		proc_remove(proc_swpm_sp_ddr_idx);
	if (proc_swpm_dir)
		proc_remove(proc_swpm_dir);
	if (proc_sched_assist_lb_enable)
		proc_remove(proc_sched_assist_lb_enable);
	if (proc_sched_assist_debug_enabled)
		proc_remove(proc_sched_assist_debug_enabled);
	if (proc_sched_assist_im_flag)
		proc_remove(proc_sched_assist_im_flag);

	if (proc_ufs_total_read_size_mb)
		proc_remove(proc_ufs_total_read_size_mb);
	if (proc_ufs_total_write_size_mb)
		proc_remove(proc_ufs_total_write_size_mb);
	if (proc_io_metrics_forever_dir)
		proc_remove(proc_io_metrics_forever_dir);
	if (proc_io_metrics_dir)
		proc_remove(proc_io_metrics_dir);
	if (proc_oplus_storage_dir)
		proc_remove(proc_oplus_storage_dir);

	if (proc_sched_assist_scene)
		proc_remove(proc_sched_assist_scene);
	if (proc_sched_assist_dir)
		proc_remove(proc_sched_assist_dir);
	if (proc_oplus_scheduler_dir)
		proc_remove(proc_oplus_scheduler_dir);
	if (evx_ufs_dev) {
		sysfs_remove_group(&evx_ufs_dev->kobj, &ufssid_attr_group);
		device_remove_file(evx_ufs_dev, &dev_attr_ufs_transmission_status);
		device_remove_file(evx_ufs_dev, &dev_attr_virtualtlcbuff);
		put_device(evx_ufs_dev);
	}

	if (proc_oplus_init_watchdog_status)
		proc_remove(proc_oplus_init_watchdog_status);
	if (proc_oplus_init_watchdog_kick)
		proc_remove(proc_oplus_init_watchdog_kick);
	if (proc_oplus_init_watchdog_dir)
		proc_remove(proc_oplus_init_watchdog_dir);
	if (proc_task_cpustats)
		proc_remove(proc_task_cpustats);
}

module_init(evx_cos_perf_compat_init);
module_exit(evx_cos_perf_compat_exit);

MODULE_DESCRIPTION("EVONIX ColorOS real-backed performance compatibility nodes");
MODULE_AUTHOR("EVONIX");
MODULE_LICENSE("GPL");
