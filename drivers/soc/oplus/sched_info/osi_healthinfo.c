// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include "osi_healthinfo.h"
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/ratelimit.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/atomic/atomic-long.h>
#include <linux/sched.h>
#include <trace/events/sched.h>
#include <trace/hooks/mm.h>
#include <trace/hooks/sched.h>
#include <trace/hooks/dtask.h>

#include "osi_base.h"
#include "../sched_assist/sa_common.h"
#include "../sched_assist/sa_group.h"

#define BUFFER_SIZE_S 256
#define BUFFER_SIZE_M 512
#define BUFFER_SIZE_L 2048

#define MAX_OHMEVENT_PARAM 4
#define OH_MSG_LEN 256
#include <linux/version.h>

/****  Ctrl init  ****/
#define OHM_LIST_MAGIC          0x5a000000
#define OHM_CTRL_MAX            32
#define OHM_INT_MAX             20
#define OHM_CTRL_IOWAIT         BIT(OHM_SCHED_IOWAIT)
#define OHM_CTRL_SCHEDLATENCY   BIT(OHM_SCHED_SCHEDLATENCY)
#define OHM_CTRL_FSYNC          BIT(OHM_SCHED_FSYNC)
#define OHM_CTRL_EMMCIO         BIT(OHM_SCHED_EMMCIO)
#define OHM_CTRL_DSTATE         BIT(OHM_SCHED_DSTATE)
#define OHM_CTRL_SCHEDTOTAL     (OHM_CTRL_EMMCIO | OHM_CTRL_FSYNC | OHM_CTRL_SCHEDLATENCY | OHM_CTRL_IOWAIT | OHM_CTRL_DSTATE)
#define OHM_CTRL_CPU_CUR        BIT(OHM_CPU_LOAD_CUR)
#define OHM_CTRL_MEMMON         BIT(OHM_MEM_MON)
#define OHM_CTRL_IOPANIC_MON    BIT(OHM_IOPANIC_MON)
#define OHM_CTRL_SVM			BIT(OHM_SVM_MON)
#define OHM_CTRL_RLIMIT			BIT(OHM_RLIMIT_MON)
#define OHM_CTRL_IONMON         BIT(OHM_ION_MON)
/******  Para Update  *****/
#define LOW_THRESH_MS_DEFAULT   100
#define HIGH_THRESH_MS_DEFAULT  500
/* low thresh 10~1000ms*/
#define LOW_THRESH_MS_LOW       10
#define LOW_THRESH_MS_HIGH      1000
/* high thresh 100~5000ms*/
#define HIGH_THRESH_MS_LOW      50
#define HIGH_THRESH_MS_HIGH     5000
/*taskstruct_name length*/
#define TASK_STRUCT_NAME_LEN    50
/*match index: The first four characters may be "com.", so choose the fifth*/
#define CHARACTERS_INDEX        4
#define CHARACTERS_NEXT_INDEX   5

#define LATENCY_STRING_FORMAT(BUF, MODULE, SCHED_STAT) sprintf(BUF, \
	#MODULE"_ctrl: %s\n"#MODULE"_logon: %s\n"#MODULE"_trig: %s\n" \
	#MODULE"_delta_ms: %llu\n"#MODULE"_low_thresh_ms: %d\n"#MODULE"_high_thresh_ms: %d\n" \
	#MODULE"_max_ms: %llu\n"#MODULE"_high_cnt: %llu\n"#MODULE"_low_cnt: %llu\n" \
	#MODULE"_total_ms: %llu\n"#MODULE"_total_cnt: %llu\n" \
	#MODULE"_fg_max_ms: %llu\n"#MODULE"_fg_high_cnt: %llu\n"#MODULE"_fg_low_cnt: %llu\n" \
	#MODULE"_fg_total_ms: %llu\n"#MODULE"_fg_total_cnt: %llu\n" \
	#MODULE"_ux_max_ms: %llu\n"#MODULE"_ux_high_cnt: %llu\n"#MODULE"_ux_low_cnt: %llu\n" \
	#MODULE"_ux_total_ms: %llu\n"#MODULE"_ux_total_cnt: %llu\n" \
	#MODULE"_rt_max_ms: %llu\n"#MODULE"_rt_high_cnt: %llu\n"#MODULE"_rt_low_cnt: %llu\n" \
	#MODULE"_rt_total_ms: %llu\n"#MODULE"_rt_total_cnt: %llu\n" \
	#MODULE"_top_app_max_ms: %llu\n"#MODULE"_top_app_high_cnt: %llu\n"#MODULE"_top_app_low_cnt: %llu\n" \
	#MODULE"_top_app_total_ms: %llu\n"#MODULE"_top_app_total_cnt: %llu\n" \
	#MODULE"_bg_max_ms: %llu\n"#MODULE"_bg_high_cnt: %llu\n"#MODULE"_bg_low_cnt: %llu\n" \
	#MODULE"_bg_total_ms: %llu\n"#MODULE"_bg_total_cnt: %llu\n" \
	#MODULE"_sys_bg_max_ms: %llu\n"#MODULE"_sys_bg_high_cnt: %llu\n"#MODULE"_sys_bg_low_cnt: %llu\n" \
	#MODULE"_sys_bg_total_ms: %llu\n"#MODULE"_sys_bg_total_cnt: %llu\n", \
	SCHED_STAT->ctrl ? "true":"false", \
	SCHED_STAT->logon ? "true":"false", \
	SCHED_STAT->trig ? "true":"false", \
	SCHED_STAT->delta_ms, \
	SCHED_STAT->low_thresh_ms, \
	SCHED_STAT->high_thresh_ms, \
	SCHED_STAT->all.max_ms, \
	SCHED_STAT->all.high_cnt, \
	SCHED_STAT->all.low_cnt, \
	SCHED_STAT->all.total_ms, \
	SCHED_STAT->all.total_cnt, \
	SCHED_STAT->fg.max_ms, \
	SCHED_STAT->fg.high_cnt, \
	SCHED_STAT->fg.low_cnt, \
	SCHED_STAT->fg.total_ms, \
	SCHED_STAT->fg.total_cnt, \
	SCHED_STAT->ux.max_ms, \
	SCHED_STAT->ux.high_cnt, \
	SCHED_STAT->ux.low_cnt, \
	SCHED_STAT->ux.total_ms, \
	SCHED_STAT->ux.total_cnt, \
	SCHED_STAT->rt.max_ms, \
	SCHED_STAT->rt.high_cnt, \
	SCHED_STAT->rt.low_cnt, \
	SCHED_STAT->rt.total_ms, \
	SCHED_STAT->rt.total_cnt, \
	SCHED_STAT->top.max_ms, \
	SCHED_STAT->top.high_cnt, \
	SCHED_STAT->top.low_cnt, \
	SCHED_STAT->top.total_ms, \
	SCHED_STAT->top.total_cnt, \
	SCHED_STAT->bg.max_ms, \
	SCHED_STAT->bg.high_cnt, \
	SCHED_STAT->bg.low_cnt, \
	SCHED_STAT->bg.total_ms, \
	SCHED_STAT->bg.total_cnt, \
	SCHED_STAT->sysbg.max_ms, \
	SCHED_STAT->sysbg.high_cnt, \
	SCHED_STAT->sysbg.low_cnt, \
	SCHED_STAT->sysbg.total_ms, \
	SCHED_STAT->sysbg.total_cnt)


/*
ohm_ctrl_list    = 0x5a0fffff
ohm_logon_list = 0x5a002005
ohm_trig_list    = 0x5a002000
*/

/*Default init*/
static int ohm_ctrl_list = OHM_LIST_MAGIC | OHM_CTRL_CPU_CUR | OHM_CTRL_MEMMON |  OHM_CTRL_IONMON | OHM_CTRL_SCHEDTOTAL;
static int ohm_logon_list = OHM_LIST_MAGIC;
static int ohm_trig_list = OHM_LIST_MAGIC;

bool ohm_cpu_ctrl = true;
bool ohm_cpu_logon;
bool ohm_cpu_trig;

bool ohm_memmon_ctrl = true;
bool ohm_memmon_logon;
bool ohm_memmon_trig;

bool ohm_iopanic_mon_ctrl = true;
bool ohm_iopanic_mon_logon;
bool ohm_iopanic_mon_trig;

bool ohm_ionmon_ctrl = true;
bool ohm_ionmon_logon;
bool ohm_ionmon_trig;

struct sched_stat_para sched_para[OHM_SCHED_TOTAL];
static char *sched_list[OHM_TYPE_TOTAL] = {
	/* SCHED_STATS 0 -11 */
	"iowait",
	"sched_latency",
	"fsync",
	"emmcio",
	"dstate",
	"sched_default_05",
	"sched_default_06",
	"sched_default_07",
	"sched_default_08",
	"sched_default_09",
	"sched_default_10",
	"sched_default_11",
	/* OTHER_TYPE 12 - */
	"cur_cpu_load",
	"memory_monitor",
	"io_panic",
	"svm_monitor",
	"rlimit_monitor",
	"ionwait_monitor"
};

enum task_struct_index {
	all = 0,
	system_server,
	com_tencent_mm,
	com_ss_android_ugc_aweme,
	com_tencent_tmgp_sgame,
	com_ss_android_ugc_aweme_lite,
	com_smile_gifmaker,
	com_tencent_mobileqq,
	com_taobao_taobao,
	com_tencent_qqlive,
	com_kuaishou_nebula,
	com_baidu_searchbox,
	/*add here*/
	task_struct_max
};

/*array_stat*/
static atomic_long_t maj_flt_stat[task_struct_max];

static atomic_long_t min_flt_stat[task_struct_max];

static void init_atomic(void)
{
	int i;
	for (i = 0; i < task_struct_max; ++i) {
		raw_atomic_long_set(&maj_flt_stat[i], 0);
		raw_atomic_long_set(&min_flt_stat[i], 0);
	}
}

static char stat_name[task_struct_max][TASK_STRUCT_NAME_LEN] = {
	"all", "system_server", "com.tencent.mm", "com.ss.android.ugc.aweme", "com.tencent.tmgp.sgame",
	"com.ss.android.ugc.aweme.lite", "com.smile.gifmaker", "com.tencent.mobileqq", "com.taobao.taobao",
	"com.tencent.qqlive", "com.kuaishou.nebula", "com.baidu.searchbox"
};

static char taskstruct_name[task_struct_max][TASK_COMM_LEN] = {
	"all", "system_server", "com.tencent.mm", "droid.ugc.aweme", "cent.tmgp.sgame",
	".ugc.aweme.lite", ".smile.gifmaker", "encent.mobileqq", "m.taobao.taobao",
	".tencent.qqlive", "kuaishou.nebula", "baidu.searchbox"
};



struct thresh_para {
	int l_ms;
	int h_ms;
};

struct thresh_para ohm_thresh_para[OHM_SCHED_TOTAL] = {
	{ LOW_THRESH_MS_DEFAULT, HIGH_THRESH_MS_DEFAULT},
	{ LOW_THRESH_MS_DEFAULT, HIGH_THRESH_MS_DEFAULT},
	{ LOW_THRESH_MS_DEFAULT, HIGH_THRESH_MS_DEFAULT},
	{ 100, 200},
	{ LOW_THRESH_MS_DEFAULT, HIGH_THRESH_MS_DEFAULT},
};

static struct kobject *ohm_kobj;
static struct work_struct ohm_detect_ws;
static char *ohm_detect_env[MAX_OHMEVENT_PARAM] = { "OHMACTION=uevent", NULL };
static bool ohm_action_ctrl;
static char msg_buf[OH_MSG_LEN] = {0};

/*
 * #if  IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
 * todo add ux type
 * #endif
 */

void ohm_trig_init(void)
{
	int i;

	ohm_memmon_trig = (ohm_trig_list & OHM_CTRL_MEMMON) ? true : false;
	ohm_cpu_trig = (ohm_trig_list & OHM_CTRL_CPU_CUR) ? true : false;
	ohm_iopanic_mon_trig = (ohm_trig_list & OHM_CTRL_IOPANIC_MON) ? true : false;
		ohm_ionmon_trig = (ohm_trig_list & OHM_CTRL_IONMON) ? true : false;
	for (i = 0; i < OHM_SCHED_TOTAL; i++) {
		sched_para[i].trig = (ohm_trig_list & BIT(i)) ? true : false;
	}
	return;
}

void ohm_logon_init(void)
{
	int i;

	ohm_cpu_logon = (ohm_logon_list & OHM_CTRL_CPU_CUR) ? true : false;
	ohm_memmon_logon = (ohm_logon_list & OHM_CTRL_MEMMON) ? true : false;
	ohm_iopanic_mon_logon = (ohm_logon_list & OHM_CTRL_IOPANIC_MON) ? true : false;
		ohm_ionmon_logon = (ohm_logon_list & OHM_CTRL_IONMON) ? true : false;
	for (i = 0; i < OHM_SCHED_TOTAL; i++) {
		sched_para[i].logon = (ohm_logon_list & BIT(i)) ? true : false;
	}
	return;
}

void ohm_ctrl_init(void)
{
	int i;

	ohm_cpu_ctrl = (ohm_ctrl_list & OHM_CTRL_CPU_CUR) ? true : false;
	ohm_memmon_ctrl = (ohm_ctrl_list & OHM_CTRL_MEMMON) ? true : false;
	ohm_iopanic_mon_ctrl = (ohm_ctrl_list & OHM_CTRL_IOPANIC_MON) ? true : false;
		ohm_ionmon_ctrl = (ohm_ctrl_list & OHM_CTRL_IONMON) ? true : false;
	for (i = 0; i < OHM_SCHED_TOTAL; i++) {
		sched_para[i].ctrl = (ohm_ctrl_list & BIT(i)) ? true : false;
	}
	return;
}



void ohm_action_trig(int type)
{
	if (!ohm_action_ctrl) {
		osi_err_deferred("ctrl off\n");
		return;
	}
	osi_debug_deferred("%s trig action\n", sched_list[type]);
	if (OHM_MEM_MON == type || OHM_SCHED_FSYNC == type) {
		if (!ohm_kobj) {
			osi_err_deferred("kobj NULL\n");
			return;
		}
		sprintf(ohm_detect_env[1], "OHMTYPE=%s", sched_list[type]);
		ohm_detect_env[MAX_OHMEVENT_PARAM - 2] = NULL;
		ohm_detect_env[MAX_OHMEVENT_PARAM - 1] = NULL;
		schedule_work(&ohm_detect_ws);
	}
}

void ohm_action_trig_with_msg(int type, char *msg)
{
	int len;

	if (!ohm_action_ctrl) {
		osi_err("ctrl off\n");
		return;
	}

	if (!ohm_kobj) {
		osi_err("kobj NULL\n");
		return;
	}
	if (OHM_SVM_MON == type || OHM_RLIMIT_MON == type || OHM_MEM_VMA_ALLOC_ERR == type) {
		sprintf(ohm_detect_env[1], "OHMTYPE=%s", sched_list[type]);
		len = snprintf(msg_buf, OH_MSG_LEN-1, "OHMMSG=%s", msg);
		msg_buf[len] = '\0';
		ohm_detect_env[MAX_OHMEVENT_PARAM - 2] = msg_buf;
		ohm_detect_env[MAX_OHMEVENT_PARAM - 1] = NULL;
		schedule_work(&ohm_detect_ws);
	}
}
EXPORT_SYMBOL_GPL(ohm_action_trig_with_msg);

void ohm_detect_work(struct work_struct *work)
{
	osi_debug("Uevent Para: %s, %s\n", ohm_detect_env[0], ohm_detect_env[1]);
	kobject_uevent_env(ohm_kobj, KOBJ_CHANGE, ohm_detect_env);
	osi_debug("Uevent Done!\n");
}

static inline void ohm_latency_dist_record(struct sched_stat_common *stat_common, u64 delta)
{
	int i = 0;

	if (delta < 1) {
	} else if (++i && delta < 2) {
	} else if (++i && delta < 5) {
	} else if (++i && delta < 10) {
	} else if (++i && delta < 20) {
	} else if (++i && delta < 50) {
	} else if (++i && delta < 100) {
	} else if (++i && delta < 200) {
	} else if (++i && delta < 500) {
	} else if (++i && delta < 1000) {
	} else if (++i && delta < 2000) {
	} else if (++i && delta < 5000) {
	} else {
		++i;
	}

	if (i >= OHM_LATENCY_DIST_MAX)
		return;
	stat_common->latency_dist[i]++;
}

static inline void ohm_sched_stat_record_common(struct sched_stat_para *sched_stat, struct sched_stat_common *stat_common, u64 delta_ms)
{
	stat_common->total_ms += delta_ms;
	stat_common->total_cnt++;
	if (delta_ms > stat_common->max_ms) {
		stat_common->max_ms = delta_ms;
	}
	if (delta_ms >= sched_stat->high_thresh_ms) {
		stat_common->high_cnt++;
	} else if (delta_ms >= sched_stat->low_thresh_ms) {
		stat_common->low_cnt++;
	}
}

static inline void _ohm_para_init(struct sched_stat_para *sched_para)
{
	sched_para->delta_ms = 0;
	memset(&sched_para->all, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->ux, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->rt, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->fg, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->top, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->bg, 0, sizeof(struct sched_stat_common));
	memset(&sched_para->sysbg, 0, sizeof(struct sched_stat_common));
	osi_debug("_ohm_para_init!\n");
}

static inline void clear_health_date(struct task_struct *p, struct sched_stat_para *sched_para)
{
	unsigned long im_flag = IM_FLAG_NONE;

	if (p && p->group_leader)
		im_flag = oplus_get_im_flag(p->group_leader);
	if (test_bit(IM_FLAG_MIDASD, &im_flag))
		 _ohm_para_init(sched_para);
}
void ohm_schedstats_record(int sched_type, struct task_struct *task, u64 delta_ms)
{
	struct sched_stat_para *sched_stat = &sched_para[sched_type];
	static DEFINE_RATELIMIT_STATE(ratelimit, 60*HZ, 1);
	unsigned long flags;

	spin_lock_irqsave(&sched_stat->lock, flags);
	if (unlikely(!sched_stat->ctrl)) {
		spin_unlock_irqrestore(&sched_stat->lock, flags);
		return;
	}
	sched_stat->delta_ms = delta_ms;
	ohm_sched_stat_record_common(sched_stat, &sched_stat->all, delta_ms);

	if (osi_task_in_cpu_group(task, "foreground")) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->fg, delta_ms);
		if (unlikely(delta_ms >= sched_stat->high_thresh_ms)) {
			if (sched_para[sched_type].logon  && __ratelimit(&ratelimit)) {
				osi_debug_deferred("[%s / %s] high_cnt, delay = %llu ms\n",
				sched_list[sched_type], "fg", delta_ms);
			}
			if (sched_para[sched_type].trig)
				ohm_action_trig(sched_type);
		}
	}

#if  IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
/*To do add ux type*/
	if (test_task_ux(task) && !strstr(task->comm, "kveritydX") && !strstr(task->comm, "erofs_worker_ux")) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->ux, delta_ms);
	}
#endif
	if (osi_task_in_cpu_group(task, "top-app")) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->top, delta_ms);
	}
	if (rt_task(task)) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->rt, delta_ms);
	}
	if (osi_task_in_cpu_group(task, "background")) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->bg, delta_ms);
	}
	if (osi_task_in_cpu_group(task, NULL)) {
		ohm_sched_stat_record_common(sched_stat, &sched_stat->sysbg, delta_ms);
	}
	spin_unlock_irqrestore(&sched_stat->lock, flags);
	return;
}

void ohm_para_init(void)
{
	int i;

	for (i = 0; i < OHM_SCHED_TOTAL; i++) {
		_ohm_para_init(&sched_para[i]);
		spin_lock_init(&sched_para[i].lock);
		sched_para[i].low_thresh_ms = 100;
		sched_para[i].high_thresh_ms = 500;
	}
	sched_para[OHM_SCHED_EMMCIO].low_thresh_ms = 100;
	sched_para[OHM_SCHED_EMMCIO].high_thresh_ms = 200;
	ohm_ctrl_init();
	ohm_logon_init();
	ohm_trig_init();
	ohm_debug("origin list: ctrl 0x%08x, logon 0x%08x, trig 0x%08x\n", ohm_ctrl_list, ohm_logon_list, ohm_trig_list);
	return;
}

void ohm_para_update(void)
{
	int i;

	for (i = 0; i < OHM_SCHED_TOTAL; i++) {
		if (ohm_thresh_para[i].l_ms < LOW_THRESH_MS_LOW
			|| ohm_thresh_para[i].l_ms > LOW_THRESH_MS_HIGH
			|| ohm_thresh_para[i].h_ms < HIGH_THRESH_MS_LOW
			|| ohm_thresh_para[i].h_ms > HIGH_THRESH_MS_HIGH) {
/********** Legal Check **********/
			ohm_err("Para illegal: sched_type %s, l_ms %d, h_ms %d\n",
				sched_list[i], ohm_thresh_para[i].l_ms, ohm_thresh_para[i].h_ms);
			ohm_thresh_para[i].l_ms = LOW_THRESH_MS_DEFAULT;
			ohm_thresh_para[i].h_ms = HIGH_THRESH_MS_DEFAULT;
			return;
		}
		sched_para[i].low_thresh_ms = ohm_thresh_para[i].l_ms;
		sched_para[i].high_thresh_ms = ohm_thresh_para[i].h_ms;
	}
	ohm_debug("Success update ohm_para!\n");
}

static inline ssize_t sched_data_to_user(char __user *buff, size_t count, loff_t *off, char *format_str, int len)
{
	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, format_str, (len < count ? len : count))) {
		return -EFAULT;
	}
	*off += len < count ? len : count;

	return (len < count ? len : count);
}

/******  Cur cpuloading  ******/
static ssize_t cpu_load_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[BUFFER_SIZE_S] = {0};
	int len = 0;
	int load = ohm_get_cur_cpuload(ohm_cpu_ctrl);

	if (load < 0) {
		load = 0;
		osi_err_deferred("ohm get cpu load error");
	}
	len = sprintf(page, "cur_cpuloading: %d\n""cur_cpu_ctrl: %s\n""cur_cpu_logon: %s\n""cur_cpu_trig: %s\n",
		load, (ohm_cpu_ctrl ? "true" : "false"), (ohm_cpu_logon ? "true" : "false"), (ohm_cpu_trig ? "true" : "false"));

	return sched_data_to_user(buff, count, off, page, len);
}

static const struct proc_ops proc_cpu_load_fops = {
	.proc_read = cpu_load_read,
	.proc_lseek = default_llseek,
};

/******  Sched latency stat  *****/
static ssize_t sched_latency_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	int len = 0;
	struct sched_stat_para *sched_stat = &sched_para[OHM_SCHED_SCHEDLATENCY];
	char *page = kzalloc(2048, GFP_KERNEL);
	unsigned long flags;

	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&sched_stat->lock, flags);
	len = LATENCY_STRING_FORMAT(page, sched_latency, sched_stat);
	clear_health_date(current, sched_stat);
	spin_unlock_irqrestore(&sched_stat->lock, flags);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, page, (len < count ? len : count))) {
		kfree(page);
		return -EFAULT;
	}
	kfree(page);
	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static const struct proc_ops proc_sched_latency_fops = {
	.proc_read = sched_latency_read,
	.proc_lseek = default_llseek,
};

/****** Sched iowait stat  *****/
static ssize_t iowait_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	int len = 0;
	struct sched_stat_para *sched_stat = &sched_para[OHM_SCHED_IOWAIT];
	char *page = kzalloc(2048, GFP_KERNEL);
	unsigned long flags;

	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&sched_stat->lock, flags);
	len = LATENCY_STRING_FORMAT(page, iowait, sched_stat);
	clear_health_date(current, sched_stat);
	spin_unlock_irqrestore(&sched_stat->lock, flags);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buff, page, (len < count ? len : count))) {
		kfree(page);
	return -EFAULT;
	}
	kfree(page);
	*off += len < count ? len : count;

	return (len < count ? len : count);
}

static const struct proc_ops proc_iowait_fops = {
	.proc_read = iowait_read,
	.proc_lseek = default_llseek,
};

static ssize_t latency_dist_read(struct file *filp, char __user *buff,
			size_t count, loff_t *off)
{
#define LATENCY_BUF_MAX 4096
	char *page;
	int len = 0;
	int i;
	struct sched_stat_para *sched_stat;
	char * const format_str[] = {"1ms", "2ms", "5ms", "10ms", "20ms", "50ms",
		"100ms", "200ms", "500ms", "1000ms", "2000ms", "5000ms", "other"};
	ssize_t ret;

	page = kmalloc(LATENCY_BUF_MAX * sizeof(char), GFP_KERNEL);
	len += sprintf(page + len, "%-16s", "latency");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10s", format_str[i]);
	len += sprintf(page + len, "\n");

	sched_stat = &sched_para[OHM_SCHED_IOWAIT];
	len += sprintf(page + len, "%-16s", "iowait_all:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->all.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "iowait_fg:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->fg.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "iowait_ux:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->ux.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "iowait_rt:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->rt.latency_dist[i]);
	len += sprintf(page + len, "\n");

	sched_stat = &sched_para[OHM_SCHED_FSYNC];
	len += sprintf(page + len, "%-16s", "fsync_all:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->all.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "fsync_fg:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->fg.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "fsync_ux:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->ux.latency_dist[i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "%-16s", "fsync_rt:");
	for (i = 0; i < OHM_LATENCY_DIST_MAX; i++)
		len += sprintf(page + len, "%-10llu", sched_stat->rt.latency_dist[i]);
	len += sprintf(page + len, "\n");

	if (len > LATENCY_BUF_MAX)
		len = LATENCY_BUF_MAX;

	ret = sched_data_to_user(buff, count, off, page, len);

	kfree(page);

	return ret;
}

static const struct proc_ops proc_latency_dist_fops = {
	.proc_read = latency_dist_read,
	.proc_lseek = default_llseek,
};


/****** Sched sync wait stat  ******/
static ssize_t fsync_wait_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	int len = 0;
	struct sched_stat_para *sched_stat = &sched_para[OHM_SCHED_FSYNC];
	char *page = kzalloc(2048, GFP_KERNEL);
	unsigned long flags;

	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&sched_stat->lock, flags);
	len = LATENCY_STRING_FORMAT(page, fsync, sched_stat);
	clear_health_date(current, sched_stat);
	spin_unlock_irqrestore(&sched_stat->lock, flags);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, page, (len < count ? len : count))) {
		kfree(page);
		return -EFAULT;
	}
	kfree(page);
	*off += len < count ? len : count;

	return (len < count ? len : count);
}

static const struct proc_ops proc_fsync_wait_fops = {
	.proc_read = fsync_wait_read,
	.proc_lseek = default_llseek,
};

/****** dstat statistics  ******/
static ssize_t dstate_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	int len = 0;

	struct sched_stat_para *sched_stat = &sched_para[OHM_SCHED_DSTATE];
	char *page = kzalloc(2048, GFP_KERNEL);
	unsigned long flags;
	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&sched_stat->lock, flags);
	len = LATENCY_STRING_FORMAT(page, dstate, sched_stat);
	clear_health_date(current, sched_stat);
	spin_unlock_irqrestore(&sched_stat->lock, flags);
	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, page, (len < count ? len : count))) {
		kfree(page);
		return -EFAULT;
	}
	kfree(page);
	*off += len < count ? len : count;

	return (len < count ? len : count);
}

static const struct proc_ops proc_dstate_fops = {
	.proc_read = dstate_read,
	.proc_lseek = default_llseek,
};

/*******alloc wait ************/

static ssize_t alloc_wait_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[1024] = {0};
	int len = 0;

	len = sprintf(page, "total_alloc_wait_h_cnt: %lld\n""total_alloc_wait_l_cnt: %lld\n"
		"ux_alloc_wait_h_cnt: %lld\n""ux_alloc_wait_l_cnt: %lld\n"
		"ux_alloc_wait_max_ms: %lld\n""ux_alloc_wait_max_order: %lld\n"
		"fg_alloc_wait_h_cnt: %lld\n""fg_alloc_wait_l_cnt: %lld\n"
		"total_alloc_wait_max_ms: %lld\n""total_alloc_wait_max_order: %lld\n"
		"fg_alloc_wait_max_ms: %lld\n""fg_alloc_wait_max_order: %lld\n"
		"alloc_wait_ctrl: %s\n""alloc_wait_logon: %s\n""alloc_wait_trig: %s\n",
		allocwait_para.total_alloc_wait.high_cnt, allocwait_para.total_alloc_wait.low_cnt,
		allocwait_para.ux_alloc_wait.high_cnt, allocwait_para.ux_alloc_wait.low_cnt,
		allocwait_para.ux_alloc_wait.max_ms, allocwait_para.ux_alloc_wait_max_order,
		allocwait_para.fg_alloc_wait.high_cnt, allocwait_para.fg_alloc_wait.low_cnt,
		allocwait_para.total_alloc_wait.max_ms, allocwait_para.total_alloc_wait_max_order,
		allocwait_para.fg_alloc_wait.max_ms, allocwait_para.fg_alloc_wait_max_order,
		ohm_memmon_ctrl ? "true":"false", ohm_memmon_logon ? "true":"false", ohm_memmon_trig ? "true":"false");

	return sched_data_to_user(buff, count, off, page, len);
}

static const struct proc_ops proc_alloc_wait_fops = {
	.proc_read = alloc_wait_read,
	.proc_lseek = default_llseek,
};

/******  Proc para   ******/
static ssize_t ohm_para_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[BUFFER_SIZE_S] = {0};
	int len = 0;

	len = sprintf(page, "action: %s\n""ctrl: 0x%08x\n""logon: 0x%08x\n""trig: 0x%08x\n",
		(ohm_action_ctrl ? "true":"false"), ohm_ctrl_list, ohm_logon_list, ohm_trig_list);

	return sched_data_to_user(buff, count, off, page, len);
}

static ssize_t ohm_para_write(struct file *file, const char __user *buff, size_t len, loff_t *ppos)
{
	char write_data[32] = {0};
	char ctrl_list[32] = {0};
	int action_ctrl;

	if (len > 31 || len == 0)
		return -EFAULT;

	if (copy_from_user(&write_data, buff, len)) {
		osi_err("write error.\n");
		return -EFAULT;
	}
	write_data[len] = '\0';
	if (write_data[len - 1] == '\n') {
		write_data[len - 1] = '\0';
	}
	if (0 == strncmp(write_data, "ohmctrl", 7)) {
		strncpy(ctrl_list, &write_data[7], OHM_INT_MAX);
		ctrl_list[OHM_INT_MAX] = '\0';
		ohm_ctrl_list = (int)simple_strtol(ctrl_list, NULL, 10);
		ohm_ctrl_init();
	} else if (0 == strncmp(write_data, "ohmlogon", 8)) {
		strncpy(ctrl_list, &write_data[8], OHM_INT_MAX);
		ctrl_list[OHM_INT_MAX] = '\0';
		ohm_logon_list = (int)simple_strtol(ctrl_list, NULL, 10);
		ohm_logon_init();
	} else if (0 == strncmp(write_data, "ohmtrig", 7)) {
		strncpy(ctrl_list, &write_data[7], OHM_INT_MAX);
		ctrl_list[OHM_INT_MAX] = '\0';
		ohm_trig_list = (int)simple_strtol(ctrl_list, NULL, 10);
		ohm_trig_init();
	} else if (0 == strncmp(write_data, "ohmparaupdate", 13)) {
		ohm_para_update();
		return len;
	} else if (0 == strncmp(write_data, "ohmacitonctrl", 13)) {
		strncpy(ctrl_list, &write_data[13], OHM_INT_MAX);
		ctrl_list[OHM_INT_MAX] = '\0';
		action_ctrl =  (int)simple_strtol(ctrl_list, NULL, 10);
		ohm_action_ctrl = action_ctrl != 0 ? 1 : 0;
	} else {
		osi_err("input illegal\n");
		return -EFAULT;
	}
	osi_debug("write: %s, set: %s, ctrl: 0x%08x, logon: 0x%08x, trig: 0x%08x\n",
		write_data, ctrl_list, ohm_ctrl_list, ohm_logon_list, ohm_trig_list);
	return len;
}

static const struct proc_ops proc_para_fops = {
	.proc_read = ohm_para_read,
	.proc_write = ohm_para_write,
	.proc_lseek = default_llseek,
};

/******  iowait hung show  ******/
unsigned int  iowait_hung_cnt;
unsigned int  iowait_panic_cnt;
static ssize_t iowait_hung_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[BUFFER_SIZE_M] = {0};
	int len = 0;

	len = sprintf(page, "iowait_hung_cnt: %u\n""iowait_panic_cnt: %u\n"
		"ohm_iopanic_mon_ctrl: %s\n""ohm_iopanic_mon_logon: %s\n""ohm_iopanic_mon_trig: %s\n",
		iowait_hung_cnt, iowait_panic_cnt,
		(ohm_iopanic_mon_ctrl ? "true" : "false"), (ohm_iopanic_mon_logon ? "true"
		 : "false"), (ohm_iopanic_mon_trig ? "true" : "false"));

	return sched_data_to_user(buff, count, off, page, len);
}

static const struct proc_ops proc_iowait_hung_fops = {
	.proc_read = iowait_hung_read,
	.proc_lseek = default_llseek,
};

/******  cpu info show  ******/
extern unsigned int cpufreq_quick_get_max(unsigned int cpu);
static ssize_t cpu_info_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char *page;
	int len = 0;
	unsigned int cpu;
	unsigned long scale_capacity = 0, last_capacity = 0;
	ssize_t ret;

	page = kmalloc(BUFFER_SIZE_L * sizeof(char), GFP_KERNEL);
	for_each_possible_cpu(cpu) {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
	scale_capacity = arch_scale_cpu_capacity(NULL, cpu);
#else
	scale_capacity = arch_scale_cpu_capacity(cpu);
#endif
	if (scale_capacity == last_capacity) {
		continue;
	}
	last_capacity = scale_capacity;
	len += snprintf(page + len, BUFFER_SIZE_L - len, "%u ", cpufreq_quick_get_max(cpu));
	}

	ret = sched_data_to_user(buff, count, off, page, len);
	kfree(page);

	return ret;
}

static const struct proc_ops proc_cpu_info_fops = {
	.proc_read = cpu_info_read,
	.proc_lseek = default_llseek,
};

/* flt_stat open */
static int show_flt_stat(struct seq_file *file, void *v)
{
	int i;
	for (i = 0; i < task_struct_max; i++) {
		seq_printf(file, "%s %ld %ld\n", stat_name[i], atomic_long_read(&maj_flt_stat[i]), atomic_long_read(&min_flt_stat[i]));
	}

	return 0;
}

static int flt_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_flt_stat, NULL);
}

static const struct proc_ops proc_flt_stat_fops = {
	.proc_open = flt_stat_open,
	.proc_read = seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* thresh read/write */
static ssize_t ohm_thresh_read_common(struct file *filp, char __user *buff, size_t count, loff_t *off, int type)
{
	char page[BUFFER_SIZE_S] = {0};
	int len = 0;

	len = sprintf(page, " %s_thresh:\n h_ms = %dms,l_ms = %dms\n",
		sched_list[type], ohm_thresh_para[type].h_ms, ohm_thresh_para[type].l_ms);

	return sched_data_to_user(buff, count, off, page, len);
}

static ssize_t ohm_thresh_read_iowait(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	return ohm_thresh_read_common(filp, buff, count, off, OHM_SCHED_IOWAIT);
}

static ssize_t ohm_thresh_read_sched_latency(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	return ohm_thresh_read_common(filp, buff, count, off, OHM_SCHED_SCHEDLATENCY);
}

static ssize_t ohm_thresh_read_fsync(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	return ohm_thresh_read_common(filp, buff, count, off, OHM_SCHED_FSYNC);
}

static ssize_t ohm_thresh_write_common(struct file *file, const char __user *buff, size_t len, loff_t *ppos, int type)
{
	char write_data[32] = {0};
	char thresh_list[32] = {0};
	int thresh = 0;

	if (len > 31 || len == 0)
		return -EFAULT;

	if (copy_from_user(&write_data, buff, len)) {
		ohm_err("write error.\n");
		return -EFAULT;
	}
	write_data[len] = '\0';
	if (write_data[len - 1] == '\n') {
		write_data[len - 1] = '\0';
	}

	if (0 == strncmp(write_data, "highthresh", 10)) {
		strncpy(thresh_list, &write_data[10], OHM_INT_MAX);
		thresh_list[OHM_INT_MAX] = '\0';
		thresh = (int)simple_strtol(thresh_list, NULL, 10);
		if (thresh < 0 || thresh < ohm_thresh_para[type].l_ms)
			goto input_err;
		ohm_thresh_para[type].h_ms = thresh;
	} else if (0 == strncmp(write_data, "lowthresh", 9)) {
		strncpy(thresh_list, &write_data[9], OHM_INT_MAX);
		thresh_list[OHM_INT_MAX] = '\0';
		thresh = (int)simple_strtol(thresh_list, NULL, 10);
		if (thresh < 0 || thresh > ohm_thresh_para[type].h_ms)
			goto input_err;
		ohm_thresh_para[type].l_ms = thresh;
	} else {
		goto input_err;
	}

	ohm_debug("thresh update success!Now %s h_ms= %dms,l_ms= %dms\n",
		sched_list[type], ohm_thresh_para[type].h_ms, ohm_thresh_para[type].l_ms);
	return len;

input_err:
	ohm_err("input illegal\n");
	return -EFAULT;
}

static ssize_t ohm_thresh_write_iowait(struct file *file, const char __user *buff, size_t len, loff_t *ppos)
{
	return ohm_thresh_write_common(file, buff, len, ppos, OHM_SCHED_IOWAIT);
}

static ssize_t ohm_thresh_write_sched_latency(struct file *file, const char __user *buff, size_t len, loff_t *ppos)
{
	return ohm_thresh_write_common(file, buff, len, ppos, OHM_SCHED_SCHEDLATENCY);
}

static ssize_t ohm_thresh_write_fsync(struct file *file, const char __user *buff, size_t len, loff_t *ppos)
{
	return ohm_thresh_write_common(file, buff, len, ppos, OHM_SCHED_FSYNC);
}

static const struct proc_ops proc_iowait_thresh_fops = {
	.proc_read = ohm_thresh_read_iowait,
	.proc_write = ohm_thresh_write_iowait,
	.proc_lseek = default_llseek,
};

static const struct proc_ops proc_sched_latency_thresh_fops = {
	.proc_read = ohm_thresh_read_sched_latency,
	.proc_write = ohm_thresh_write_sched_latency,
	.proc_lseek = default_llseek,
};

static const struct proc_ops proc_fsync_thresh_fops = {
	.proc_read = ohm_thresh_read_fsync,
	.proc_write = ohm_thresh_write_fsync,
	.proc_lseek = default_llseek,
};


static void probe_sched_latency_handler(void *data, struct task_struct *tsk, u64 delay)
{
	if (!rt_task(tsk))
		ohm_schedstats_record(OHM_SCHED_SCHEDLATENCY, tsk, (delay >> 20));
}

static void probe_sched_stat_iowait_handler(void *data, struct task_struct *tsk, u64 delay)
{
	if (!rt_task(tsk))
		ohm_schedstats_record(OHM_SCHED_IOWAIT, tsk, (delay >> 20));
}

static void probe_sched_stat_blocked(void *data, struct task_struct *tsk, u64 delay)
{
	if (!rt_task(tsk))
		ohm_schedstats_record(OHM_SCHED_DSTATE, tsk, (delay >> 20));
}


static void update_runnable_time_handler(void *data, bool preempt, struct task_struct *prev,
		struct task_struct *next, unsigned int prev_state)
{
	u64 delay;
	struct rq *rq;
	struct oplus_task_struct *ots_next;

	if (unlikely(!next))
		return;
	ots_next = get_oplus_task_struct(next);
	if (!rt_task(next) || !ots_next)
		return;

	rq = task_rq(next);
	update_rq_clock(rq);

	delay = rq->clock - ots_next->enqueue_time;
	ohm_schedstats_record(OHM_SCHED_SCHEDLATENCY, next, (delay >> 20));
}


void update_block_state(struct task_struct *p, int flags)
{
	struct oplus_task_struct *ots;
	u64 delay;

	if (!rt_task(p) || !(flags & ENQUEUE_WAKEUP))
		return;
	ots = get_oplus_task_struct(p);
	if (ots && ots->block_start_time) {
		delay = sched_clock() - ots->block_start_time;
		ots->block_start_time = 0;
		if (p->in_iowait)
			ohm_schedstats_record(OHM_SCHED_IOWAIT, p, (delay >> 20));
		else
			ohm_schedstats_record(OHM_SCHED_DSTATE, p, (delay >> 20));
	}
}

static inline int quick_match(char *comm)
{
	int i, len;
	len = strlen(comm);
	i = -1;
	if (len == TASK_COMM_LEN - 3) {
		return system_server;
	}
	if (len == TASK_COMM_LEN - 2) {
		return com_tencent_mm;
	}
	if (len != TASK_COMM_LEN - 1) {
		return -1;
	}
	switch (comm[CHARACTERS_INDEX]) {
	case 'd':
		return com_ss_android_ugc_aweme;
	case '.':
		/* The fifth character is the same, compare the sixth character*/
		if (comm[CHARACTERS_NEXT_INDEX] == 't') {
			return com_tencent_tmgp_sgame;
		}
		if (comm[CHARACTERS_NEXT_INDEX] == 'a') {
			return com_ss_android_ugc_aweme_lite;
		}
		break;
	case 'l':
		return com_smile_gifmaker;
	case 'n':
		return com_tencent_mobileqq;
	case 'o':
		return com_taobao_taobao;
	case 'c':
		return com_tencent_qqlive;
	case 's':
		return com_kuaishou_nebula;
	case 'u':
		return com_baidu_searchbox;
	default:
		break;
	}

	return i;
}

/*Quick match, first match the length, then match the first and fifth characters, and finally strcmp*/
static inline int comm_match(char *comm)
{
	/*match character, Inaccurate*/
	int i = quick_match(comm);
	if (i == -1) {
		return -1;
	}
	/*do accurate match*/
	if (!strcmp(taskstruct_name[i], comm)) {
		return i;
	}
	return -1;
}

void android_vh_exit_check_pagefault_handler(void *unused, struct task_struct *p)
{
	int i;

	if (p->tgid == p->pid) {
		raw_atomic_long_add((long)p->maj_flt, &maj_flt_stat[0]);
		raw_atomic_long_add((long)p->min_flt, &min_flt_stat[0]);
		/*match task_struct->comm*/
		i = comm_match(p->comm);
		if (i > 0 && i < task_struct_max) {
			raw_atomic_long_add((long)p->maj_flt, &maj_flt_stat[i]);
			raw_atomic_long_add((long)p->min_flt, &min_flt_stat[i]);
		}
	}
	return;
}

static int register_healthinfo_vendor_hooks(void)
{
	int ret;

	ret = register_trace_sched_stat_wait(probe_sched_latency_handler, NULL);
	if (ret)
		goto fail_0;
	ret = register_trace_sched_stat_blocked(probe_sched_stat_blocked, NULL);
	if (ret)
		goto fail_1;
	ret = register_trace_sched_stat_iowait(probe_sched_stat_iowait_handler, NULL);
	if (ret)
		goto fail_2;
	ret = register_trace_sched_switch(update_runnable_time_handler, NULL);
	if (ret)
		goto fail_3;
	ret = register_trace_android_vh_exit_check(android_vh_exit_check_pagefault_handler, NULL);
	if (ret)
		goto fail_4;
	return 0;
fail_4:
	unregister_trace_sched_switch(update_runnable_time_handler, NULL);
fail_3:
	unregister_trace_sched_stat_iowait(probe_sched_stat_iowait_handler, NULL);
fail_2:
	unregister_trace_sched_stat_blocked(probe_sched_stat_blocked, NULL);
fail_1:
	unregister_trace_sched_stat_wait(probe_sched_latency_handler, NULL);
fail_0:
	tracepoint_synchronize_unregister();
	return ret;
}

static int unregister_healthinfo_vendor_hooks(void)
{
	unregister_trace_android_vh_exit_check(android_vh_exit_check_pagefault_handler, NULL);
	unregister_trace_sched_switch(update_runnable_time_handler, NULL);
	unregister_trace_sched_stat_iowait(probe_sched_stat_iowait_handler, NULL);
	unregister_trace_sched_stat_blocked(probe_sched_stat_blocked, NULL);
	unregister_trace_sched_stat_wait(probe_sched_latency_handler, NULL);
	tracepoint_synchronize_unregister();
	return 0;
}

#define FG_RW (S_IWUSR|S_IRUSR|S_IWGRP|S_IRGRP|S_IWOTH|S_IROTH)
#define MAX_ARRAY_LENGTH 256
#define FS_FG_INFO_PATH "fg_info"
#define FS_FG_UIDS "fg_uids"

struct fg_info {
	int fg_num;
	int fg_uids;
};

struct fg_info fginfo = {
	.fg_num = 0,
	.fg_uids = -555,
};

bool is_fg(int uid)
{
	bool ret = false;
	if (uid == fginfo.fg_uids)
		ret = true;
	return ret;
}
EXPORT_SYMBOL_GPL(is_fg);

inline int current_is_fg(void)
{
	int cur_uid;
	cur_uid = current_uid().val;
	if (is_fg(cur_uid))
		return 1;
	return 0;
}

inline int task_is_fg(struct task_struct *tsk)
{
	int cur_uid;
	cur_uid = task_uid(tsk).val;
	if (is_fg(cur_uid))
		return 1;
	return 0;
}

static int fg_uids_show(struct seq_file *m, void *v)
{
	seq_printf(m, "fg_uids: %d\n", fginfo.fg_uids);
	return 0;
}

static int fg_uids_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, fg_uids_show, inode);
}

#if IS_ENABLED(CONFIG_OPLUS_BINDER_TRANS_CTRL)
extern void oblist_dequeue_topapp_change(uid_t topuid);
#endif
static ssize_t fg_uids_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[MAX_ARRAY_LENGTH];
	int err = 0;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user((void *)buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	fginfo.fg_uids = simple_strtol(buffer, NULL, 0);
	fginfo.fg_num = 1;
#if IS_ENABLED(CONFIG_OPLUS_BINDER_TRANS_CTRL)
	if (fginfo.fg_uids > 0)
		oblist_dequeue_topapp_change(fginfo.fg_uids);
#endif
out:
	return err < 0 ? err : count;
}

static const struct proc_ops proc_fg_uids_operations = {
	.proc_open       = fg_uids_open,
	.proc_read       = seq_read,
	.proc_write      = fg_uids_write,
	.proc_lseek     = seq_lseek,
	.proc_release    = single_release,
};

static int uids_proc_fs_init(struct proc_dir_entry *parent)
{
	if (!parent)
		return -EINVAL;
	return proc_create(FS_FG_UIDS, FG_RW, parent,
			   &proc_fg_uids_operations) ? 0 : -ENOMEM;
}


#define HEALTHINFO_PROC_NODE "oplus_healthinfo"
static struct proc_dir_entry *fg_dir;
static struct proc_dir_entry *healthinfo;
static struct proc_dir_entry *sched_thresh;


int oplus_healthinfo_proc_fs_init(void)
{
	int ret = -ENOMEM;
	struct proc_dir_entry *pentry;

	ohm_para_init();
	healthinfo =  proc_mkdir(HEALTHINFO_PROC_NODE, NULL);
	if (!healthinfo) {
		ohm_err("can't create healthinfo proc\n");
		goto ERROR_INIT_VERSION;
	}

	/*init atomic array */
	init_atomic();

/******  ctrl  *****/
	pentry = proc_create("para_update", S_IRUGO | S_IWUGO, healthinfo, &proc_para_fops);
	if (!pentry) {
		ohm_err("create para_update proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

/******  Stat  ******/

	pentry = proc_create("cpu_loading", S_IRUGO, healthinfo, &proc_cpu_load_fops);
	if (!pentry) {
		ohm_err("create cpu_loading proc failed.\n");
		goto ERROR_INIT_VERSION;
	}
	pentry = proc_create("latency_dist", S_IRUGO, healthinfo,
			&proc_latency_dist_fops);
	if (!pentry) {
		ohm_err("create latency dist failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("iowait", S_IRUGO, healthinfo, &proc_iowait_fops);
	if (!pentry) {
		ohm_err("create iowait proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("flt_stat", S_IRUGO, healthinfo, &proc_flt_stat_fops);
	if (!pentry) {
		ohm_err("create flt stat proc failed.\n");
	    goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("sched_latency", S_IRUGO, healthinfo, &proc_sched_latency_fops);
	if (!pentry) {
		ohm_err("create sched_latency proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("dstate", S_IRUGO, healthinfo, &proc_dstate_fops);
	if (!pentry) {
		ohm_err("create dstate proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("iowait_hung", S_IRUGO, healthinfo, &proc_iowait_hung_fops);
	if (!pentry) {
		ohm_err("create iowait_hung proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("alloc_wait", S_IRUGO, healthinfo, &proc_alloc_wait_fops);
	if (!pentry) {
		ohm_err("create alloc_wait proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("cpu_info", S_IRUGO, healthinfo, &proc_cpu_info_fops);
	if (!pentry) {
		ohm_err("create cpu info proc failed.\n");
	goto ERROR_INIT_VERSION;
	}


	/****** thresh update ******/
	sched_thresh =  proc_mkdir("sched_thresh", healthinfo);
	if (!sched_thresh) {
		ohm_err("can't create healthinfo proc\n");
		goto ERROR_INIT_VERSION;
	}

	pentry = proc_create("iowait_thresh", S_IRUGO | S_IWUGO, sched_thresh, &proc_iowait_thresh_fops);
	if (!pentry) {
		ohm_err("create iowait_thresh proc failed.\n");
		goto ERROR_INIT_VERSION;
	}
	pentry = proc_create("sched_latency_thresh", S_IRUGO | S_IWUGO, sched_thresh, &proc_sched_latency_thresh_fops);
	if (!pentry) {
		ohm_err("create sched_latency_thresh proc failed.\n");
		goto ERROR_INIT_VERSION;
	}

	fg_dir = proc_mkdir(FS_FG_INFO_PATH, NULL);
	if (!fg_dir) {
		goto ERROR_INIT_VERSION;
	} else {
		ret = uids_proc_fs_init(fg_dir);
		if (ret)
			goto ERROR_INIT_VERSION;
	}
	ret = register_healthinfo_vendor_hooks();
	if (ret)
		goto ERROR_INIT_VERSION;
	return 0;

ERROR_INIT_VERSION:
	proc_remove(healthinfo);
	proc_remove(fg_dir);
	healthinfo = NULL;
	fg_dir = NULL;
	sched_thresh = NULL;
	return ret ? ret : -ENOMEM;
}

void oplus_healthinfo_proc_fs_exit(void)
{
	unregister_healthinfo_vendor_hooks();
	proc_remove(healthinfo);
	proc_remove(fg_dir);
	healthinfo = NULL;
	fg_dir = NULL;
	sched_thresh = NULL;
}


module_param_named(ohm_action_ctrl, ohm_action_ctrl, bool, S_IRUGO | S_IWUSR);
