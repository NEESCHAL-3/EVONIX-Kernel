#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include "../sched_assist/sa_common.h"
#include "frame_group.h"
#include "cluster_boost.h"

static DEFINE_RAW_SPINLOCK(preferred_cluster_id_lock);
/*
 * Only interested in these threads their tgid euqal to user_interested_tgid.
 * the purpose of adding this condition is to exclude these threads created
 * befor FBG module init, the preferred_cluster_id of their oplus_task_struct is 0,
 * they will be preferred to run cluster 0 incorrectly.
 */
static atomic_t user_interested_tgid = ATOMIC_INIT(-1);

int __fbg_set_task_preferred_cluster(pid_t tid, int cluster_id)
{
	struct task_struct *task = NULL;
	struct oplus_task_struct *ots = NULL;
	unsigned long flags;

	rcu_read_lock();
	task = find_task_by_vpid(tid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (task) {
		ots = get_oplus_task_struct(task);
		if (IS_ERR_OR_NULL(ots)) {
			put_task_struct(task);
			return 0;
		}
		atomic_set(&user_interested_tgid, task->tgid);
		raw_spin_lock_irqsave(&preferred_cluster_id_lock, flags);
		if ((cluster_id >= 0) && (cluster_id < num_sched_clusters))
			ots->preferred_cluster_id = cluster_id;
		else
			ots->preferred_cluster_id = -1;
		raw_spin_unlock_irqrestore(&preferred_cluster_id_lock, flags);
		put_task_struct(task);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__fbg_set_task_preferred_cluster);

/* The interface is currently offered for use in games. */
struct oplus_sched_cluster *fbg_get_task_preferred_cluster(struct task_struct *p)
{
	unsigned long flags;
	int preferred_cluster_id;
	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (likely(atomic_read(&user_interested_tgid) != p->tgid))
		return NULL;

	raw_spin_lock_irqsave(&preferred_cluster_id_lock, flags);
	preferred_cluster_id = ots->preferred_cluster_id;
	raw_spin_unlock_irqrestore(&preferred_cluster_id_lock, flags);

	if (preferred_cluster_id < 0 || preferred_cluster_id >= num_sched_clusters)
		return NULL;

	return fb_cluster[preferred_cluster_id];
}
