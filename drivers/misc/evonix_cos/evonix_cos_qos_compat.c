// SPDX-License-Identifier: GPL-2.0-only
/*
 * EVONIX ColorOS/OPlus QoS scheduler compatibility backend for rodin.
 *
 * ColorOS qos_sched.so exchanges three page-backed lookup tables with the
 * kernel and then selects entries through /proc/oplus_qos_sched/qos_level.
 * Rodin does not ship OPlus' scheduler extension, but it does provide the
 * upstream Linux mechanisms represented by those entries: per-task uclamp,
 * fair/RT priority and cpuctl group controls.  This driver implements the
 * OPlus userspace ABI and translates task/process entries to those real Linux
 * scheduler operations.  It deliberately does not acknowledge unsupported
 * commands as successful.
 */

#include <linux/compat.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <trace/events/sched.h>
#include <trace/hooks/sched.h>
#include <uapi/linux/sched/types.h>

#define EVX_QOS_NAME                    "evonix_cos_qos"
#define EVX_QOS_DIR                     "oplus_qos_sched"

#define EVX_QOS_MAGIC                   'q'
#define EVX_LUT_MAGIC                   'p'

#define EVX_QOS_MAX_LEVELS              256
#define EVX_QOS_MAX_TIDS                64
#define EVX_QOS_MAX_PROCESS_THREADS     4096
#define EVX_QOS_PID_LEVEL_MASK          0x00ffffff
#define EVX_QOS_PID_ACTIVE_SHIFT        24

#define EVX_QOS_DEFAULT                 (-2)
#define EVX_QOS_RESET                   (-1)

#define EVX_LATENCY_MAGIC               3ULL
#define EVX_UX_LEVEL_MASK               0x0f000000U
#define EVX_UX_LEVEL_MEDIUM             0x03000000U
#define EVX_UX_LEVEL_HIGH               0x06000000U
#define EVX_UX_LEVEL_SUPER              0x08000000U
#define EVX_UX_LEVEL_TOP                0x0a000000U

#define EVX_TASK_STATE_BITS             8
#define EVX_PROCESS_POLICY_BITS         6

enum evx_qos_lut_type {
	EVX_QOS_LUT_TASK = 1,
	EVX_QOS_LUT_PROCESS = 2,
	EVX_QOS_LUT_GROUP = 4,
};

enum evx_qos_level_command {
	EVX_SET_TID_LEVEL = 1,
	EVX_SET_PID_LEVEL,
	EVX_SET_TID_ARRAY_LEVEL,
	EVX_SET_LEVEL_MAX,
};

enum evx_qos_lut_command {
	EVX_UPDATE_LUT_REQUEST = 1,
	EVX_GET_LUT_VERSION,
	EVX_LUT_COMMAND_MAX,
};

/* Exact 64-bit ABI used by ColorOS qos_sched.so. */
struct evx_qos_lut_request {
	s32 lut_size;
	s32 type;
	s64 version;
};

struct evx_qos_lut_item {
	s32 qos_level;
	s32 padding;
	s64 latency;
	s32 share_or_prio;
	s32 uclamp_min;
	s32 uclamp_max;
	s32 stune;
};

struct evx_tid_array {
	s32 count;
	s32 tids[EVX_QOS_MAX_TIDS];
};

struct evx_qos_level_data {
	s32 level;
	union {
		s32 tid;
		s32 pid;
		struct evx_tid_array tarray;
	} info;
};

#define EVX_IOCTL_UPDATE_LUT \
	_IOW(EVX_LUT_MAGIC, EVX_UPDATE_LUT_REQUEST, struct evx_qos_lut_request)
#define EVX_IOCTL_GET_LUT_VERSION \
	_IOR(EVX_LUT_MAGIC, EVX_GET_LUT_VERSION, struct evx_qos_lut_request)
#define EVX_IOCTL_SET_TID_LEVEL \
	_IOW(EVX_QOS_MAGIC, EVX_SET_TID_LEVEL, struct evx_qos_level_data)
#define EVX_IOCTL_SET_PID_LEVEL \
	_IOW(EVX_QOS_MAGIC, EVX_SET_PID_LEVEL, struct evx_qos_level_data)
#define EVX_IOCTL_SET_TID_ARRAY_LEVEL \
	_IOW(EVX_QOS_MAGIC, EVX_SET_TID_ARRAY_LEVEL, struct evx_qos_level_data)

struct evx_qos_lut_bank {
	void *mem;
	size_t data_size;
	size_t alloc_size;
	u32 levels;
	s64 version;
	atomic_t mappings;
};

struct evx_qos_lut_table {
	/* Serializes table replacement against item lookup and diagnostics. */
	struct mutex lock;
	struct evx_qos_lut_bank banks[2];
	u8 active;
	bool valid;
	u64 updates;
};

struct evx_qos_lut_file {
	struct evx_qos_lut_bank *pending_bank;
	s32 pending_type;
};

struct evx_qos_level_file {
	pid_t reader_tid;
};

struct evx_qos_task_state {
	struct hlist_node node;
	struct task_struct *task;
	s32 qos_level;
	bool priority_saved;
	int saved_policy;
	int saved_nice;
	unsigned int saved_rt_priority;
	bool saved_reset_on_fork;
};

struct evx_qos_process_policy {
	struct hlist_node node;
	struct task_struct *leader;
	struct evx_qos_lut_item item;
	s32 level;
};

static struct proc_dir_entry *evx_qos_dir;
static struct evx_qos_lut_table evx_qos_luts[3];

static DEFINE_HASHTABLE(evx_qos_task_states, EVX_TASK_STATE_BITS);
static DEFINE_SPINLOCK(evx_qos_task_states_lock);
static DEFINE_HASHTABLE(evx_qos_process_policies, EVX_PROCESS_POLICY_BITS);
static DEFINE_SPINLOCK(evx_qos_process_policies_lock);

static atomic64_t evx_lut_ioctl_requests;
static atomic64_t evx_lut_mmaps;
static atomic64_t evx_level_ioctl_requests;
static atomic64_t evx_task_apply_successes;
static atomic64_t evx_task_apply_failures;
static atomic64_t evx_latency_fallbacks;
static atomic_t evx_last_error;

static bool evx_fair_policy(int policy)
{
	return policy == SCHED_NORMAL || policy == SCHED_BATCH;
}

static bool evx_rt_policy(int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

static bool evx_deadline_policy(int policy)
{
	return policy == SCHED_DEADLINE;
}

static bool evx_qos_authorized(void)
{
	uid_t uid = __kuid_val(current_euid());

	return uid == 0 || uid == 1000;
}

static int evx_lut_index(s32 type)
{
	switch (type) {
	case EVX_QOS_LUT_TASK:
		return 0;
	case EVX_QOS_LUT_PROCESS:
		return 1;
	case EVX_QOS_LUT_GROUP:
		return 2;
	default:
		return -EINVAL;
	}
}

static const char *evx_lut_name(int index)
{
	static const char * const names[] = { "task", "process", "group" };

	if (index < 0 || index >= ARRAY_SIZE(names))
		return "invalid";
	return names[index];
}

static void evx_record_error(int error)
{
	if (error < 0)
		atomic_set(&evx_last_error, error);
}

static int evx_lut_replace_bank(struct evx_qos_lut_table *table,
				struct evx_qos_lut_file *ctx,
				const struct evx_qos_lut_request *request)
{
	struct evx_qos_lut_bank *bank;
	void *new_mem;
	size_t alloc_size;
	u8 next;

	if (request->lut_size <= 0 ||
	    request->lut_size > EVX_QOS_MAX_LEVELS * sizeof(struct evx_qos_lut_item) ||
	    request->lut_size % sizeof(struct evx_qos_lut_item))
		return -EINVAL;

	alloc_size = PAGE_ALIGN(request->lut_size);
	new_mem = alloc_pages_exact(alloc_size, GFP_KERNEL | __GFP_ZERO);
	if (!new_mem)
		return -ENOMEM;

	/* An all-ones entry is invalid until userspace has copied the real LUT. */
	memset(new_mem, 0xff, alloc_size);

	mutex_lock(&table->lock);
	next = table->valid ? table->active ^ 1 : 0;
	bank = &table->banks[next];
	if (atomic_read(&bank->mappings)) {
		mutex_unlock(&table->lock);
		free_pages_exact(new_mem, alloc_size);
		return -EBUSY;
	}

	if (bank->mem)
		free_pages_exact(bank->mem, bank->alloc_size);

	bank->mem = new_mem;
	bank->data_size = request->lut_size;
	bank->alloc_size = alloc_size;
	bank->levels = request->lut_size / sizeof(struct evx_qos_lut_item);
	bank->version = request->version;
	atomic_set(&bank->mappings, 0);

	table->active = next;
	table->valid = true;
	table->updates++;
	ctx->pending_bank = bank;
	ctx->pending_type = request->type;
	mutex_unlock(&table->lock);

	return 0;
}

static int evx_lut_copy_item(s32 type, s32 level,
			     struct evx_qos_lut_item *item)
{
	struct evx_qos_lut_table *table;
	struct evx_qos_lut_bank *bank;
	int index = evx_lut_index(type);
	int ret = 0;

	if (index < 0 || level < 0)
		return -EINVAL;

	table = &evx_qos_luts[index];
	mutex_lock(&table->lock);
	if (!table->valid) {
		ret = -ENODATA;
		goto out;
	}

	bank = &table->banks[table->active];
	if (!bank->mem || level >= bank->levels) {
		ret = -ERANGE;
		goto out;
	}

	/* Userspace writes the shared page before issuing any level command. */
	smp_rmb();
	memcpy(item, (struct evx_qos_lut_item *)bank->mem + level,
	       sizeof(*item));
	if (item->qos_level != level)
		ret = -ENODATA;
out:
	mutex_unlock(&table->lock);
	return ret;
}

static struct evx_qos_task_state *
evx_find_task_state_locked(struct task_struct *task)
{
	struct evx_qos_task_state *state;

	hash_for_each_possible(evx_qos_task_states, state, node,
			       (unsigned long)task) {
		if (state->task == task)
			return state;
	}
	return NULL;
}

static struct evx_qos_task_state *
evx_get_or_create_task_state(struct task_struct *task, gfp_t gfp)
{
	struct evx_qos_task_state *state, *new_state;

	new_state = kzalloc(sizeof(*new_state), gfp);
	spin_lock(&evx_qos_task_states_lock);
	state = evx_find_task_state_locked(task);
	if (!state && new_state) {
		state = new_state;
		new_state = NULL;
		state->task = task;
		state->qos_level = -1;
		hash_add(evx_qos_task_states, &state->node, (unsigned long)task);
	}
	spin_unlock(&evx_qos_task_states_lock);
	kfree(new_state);

	return state;
}

static int evx_save_priority_once(struct task_struct *task, gfp_t gfp)
{
	struct evx_qos_task_state *state;

	state = evx_get_or_create_task_state(task, gfp);
	if (!state)
		return -ENOMEM;

	spin_lock(&evx_qos_task_states_lock);
	if (!state->priority_saved) {
		state->saved_policy = READ_ONCE(task->policy);
		state->saved_nice = task_nice(task);
		state->saved_rt_priority = READ_ONCE(task->rt_priority);
		/* sched_reset_on_fork is a bit-field and cannot use READ_ONCE(). */
		state->saved_reset_on_fork = task->sched_reset_on_fork;
		state->priority_saved = true;
	}
	spin_unlock(&evx_qos_task_states_lock);

	return 0;
}

static int evx_restore_priority(struct task_struct *task)
{
	struct evx_qos_task_state *state;
	struct sched_attr attr = { };
	int ret;

	spin_lock(&evx_qos_task_states_lock);
	state = evx_find_task_state_locked(task);
	if (!state || !state->priority_saved) {
		spin_unlock(&evx_qos_task_states_lock);
		return 0;
	}

	attr.sched_policy = state->saved_policy;
	attr.sched_nice = state->saved_nice;
	attr.sched_priority = state->saved_rt_priority;
	if (state->saved_reset_on_fork)
		attr.sched_flags = SCHED_FLAG_RESET_ON_FORK;
	spin_unlock(&evx_qos_task_states_lock);

	ret = sched_setattr_nocheck(task, &attr);
	if (!ret) {
		spin_lock(&evx_qos_task_states_lock);
		state = evx_find_task_state_locked(task);
		if (state)
			state->priority_saved = false;
		spin_unlock(&evx_qos_task_states_lock);
	}
	return ret;
}

static int evx_set_task_priority(struct task_struct *task, s32 prio, gfp_t gfp)
{
	struct sched_attr attr = { };
	int ret;

	if (prio == EVX_QOS_DEFAULT)
		return 0;
	if (prio == EVX_QOS_RESET)
		return evx_restore_priority(task);
	if (prio < 0 || prio >= MAX_PRIO)
		return -EINVAL;
	if (evx_deadline_policy(READ_ONCE(task->policy)))
		return -EOPNOTSUPP;

	ret = evx_save_priority_once(task, gfp);
	if (ret)
		return ret;

	if (prio < MAX_RT_PRIO) {
		attr.sched_policy = SCHED_FIFO;
		attr.sched_priority = MAX_RT_PRIO - 1 - prio;
		attr.sched_flags = SCHED_FLAG_RESET_ON_FORK;
		return sched_setattr_nocheck(task, &attr);
	}

	if (evx_fair_policy(READ_ONCE(task->policy))) {
		set_user_nice(task, PRIO_TO_NICE(prio));
		return 0;
	}

	attr.sched_policy = SCHED_NORMAL;
	attr.sched_nice = PRIO_TO_NICE(prio);
	attr.sched_flags = SCHED_FLAG_RESET_ON_FORK;
	return sched_setattr_nocheck(task, &attr);
}

static int evx_latency_to_uclamp_min(s64 latency)
{
	u32 priority;

	if (((u64)latency >> 32) != EVX_LATENCY_MAGIC)
		return EVX_QOS_DEFAULT;

	priority = (u32)latency;
	switch (priority & EVX_UX_LEVEL_MASK) {
	case EVX_UX_LEVEL_MEDIUM:
		return 600;
	case EVX_UX_LEVEL_HIGH:
		return 750;
	case EVX_UX_LEVEL_SUPER:
		return 900;
	case EVX_UX_LEVEL_TOP:
		return 1024;
	default:
		return EVX_QOS_DEFAULT;
	}
}

static int evx_set_task_uclamp(struct task_struct *task, s32 min, s32 max,
			       s64 latency)
{
	struct sched_attr attr = { };
	u64 flags = 0;
	int latency_min;

	latency_min = evx_latency_to_uclamp_min(latency);
	if (min == EVX_QOS_DEFAULT && latency_min >= 0) {
		min = latency_min;
		if (max == EVX_QOS_DEFAULT)
			max = 1024;
		atomic64_inc(&evx_latency_fallbacks);
	}

	if (min >= EVX_QOS_RESET)
		flags |= SCHED_FLAG_UTIL_CLAMP_MIN;
	if (max >= EVX_QOS_RESET)
		flags |= SCHED_FLAG_UTIL_CLAMP_MAX;
	if (!flags)
		return 0;
	if (min < EVX_QOS_RESET || min > SCHED_CAPACITY_SCALE ||
	    max < EVX_QOS_RESET || max > SCHED_CAPACITY_SCALE)
		return -EINVAL;
	if (min != EVX_QOS_RESET && max != EVX_QOS_RESET && min > max)
		return -EINVAL;

	attr.sched_policy = READ_ONCE(task->policy);
	attr.sched_flags = SCHED_FLAG_KEEP_ALL | flags;
	if (min >= EVX_QOS_RESET)
		attr.sched_util_min = min;
	if (max >= EVX_QOS_RESET)
		attr.sched_util_max = max;
	if ((min >= 0 || max >= 0) && min != EVX_QOS_RESET &&
	    max != EVX_QOS_RESET)
		attr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
	if (evx_rt_policy(attr.sched_policy))
		attr.sched_priority = READ_ONCE(task->rt_priority);

	return sched_setattr_nocheck(task, &attr);
}

static int evx_apply_item_to_task(struct task_struct *task,
				  const struct evx_qos_lut_item *item,
				  gfp_t gfp)
{
	struct evx_qos_task_state *state;
	int ret;

	if (!task || !pid_alive(task))
		return -ESRCH;

	ret = evx_set_task_priority(task, item->share_or_prio, gfp);
	if (ret)
		goto failed;

	ret = evx_set_task_uclamp(task, item->uclamp_min, item->uclamp_max,
				  item->latency);
	if (ret)
		goto failed;

	state = evx_get_or_create_task_state(task, gfp);
	if (!state) {
		ret = -ENOMEM;
		goto failed;
	}
	spin_lock(&evx_qos_task_states_lock);
	state->qos_level = item->qos_level;
	spin_unlock(&evx_qos_task_states_lock);
	atomic64_inc(&evx_task_apply_successes);
	return 0;

failed:
	atomic64_inc(&evx_task_apply_failures);
	evx_record_error(ret);
	return ret;
}

static int evx_apply_level_to_tid(pid_t tid,
				  const struct evx_qos_lut_item *item,
				  gfp_t gfp)
{
	struct task_struct *task;
	int ret;

	task = find_get_task_by_vpid(tid);
	if (!task)
		return -ESRCH;
	ret = evx_apply_item_to_task(task, item, gfp);
	put_task_struct(task);
	return ret;
}

static struct evx_qos_process_policy *
evx_find_process_policy_locked(struct task_struct *leader)
{
	struct evx_qos_process_policy *policy;

	hash_for_each_possible(evx_qos_process_policies, policy, node,
			       (unsigned long)leader) {
		if (policy->leader == leader)
			return policy;
	}
	return NULL;
}

static int evx_update_process_policy(struct task_struct *leader,
				     const struct evx_qos_lut_item *item,
				     int level, bool active)
{
	struct evx_qos_process_policy *policy, *new_policy = NULL;
	struct evx_qos_process_policy *old_policy = NULL;

	if (active) {
		new_policy = kzalloc(sizeof(*new_policy), GFP_KERNEL);
		if (!new_policy)
			return -ENOMEM;
	}

	spin_lock(&evx_qos_process_policies_lock);
	policy = evx_find_process_policy_locked(leader);
	if (active) {
		if (!policy) {
			policy = new_policy;
			new_policy = NULL;
			policy->leader = leader;
			hash_add(evx_qos_process_policies, &policy->node,
				 (unsigned long)leader);
		}
		policy->item = *item;
		policy->level = level;
	} else if (policy) {
		hash_del(&policy->node);
		old_policy = policy;
	}
	spin_unlock(&evx_qos_process_policies_lock);
	kfree(new_policy);
	kfree(old_policy);
	return 0;
}

static int evx_apply_level_to_process(pid_t pid,
				      const struct evx_qos_lut_item *item,
				      int level, bool active)
{
	struct task_struct **tasks;
	struct task_struct *target, *leader, *thread;
	int count = 0, i, successes = 0, first_error = 0, policy_ret = 0;
	bool leader_seen = false;

	target = find_get_task_by_vpid(pid);
	if (!target)
		return -ESRCH;
	leader = target->group_leader;
	get_task_struct(leader);
	put_task_struct(target);

	tasks = kvmalloc_array(EVX_QOS_MAX_PROCESS_THREADS,
			       sizeof(*tasks), GFP_KERNEL);
	if (!tasks) {
		put_task_struct(leader);
		return -ENOMEM;
	}

	rcu_read_lock();
	for_each_thread(leader, thread) {
		if (count == EVX_QOS_MAX_PROCESS_THREADS)
			break;
		get_task_struct(thread);
		tasks[count++] = thread;
		if (thread == leader)
			leader_seen = true;
	}
	if (!leader_seen && count < EVX_QOS_MAX_PROCESS_THREADS) {
		get_task_struct(leader);
		tasks[count++] = leader;
	}
	rcu_read_unlock();

	for (i = 0; i < count; i++) {
		int ret = evx_apply_item_to_task(tasks[i], item, GFP_KERNEL);

		if (!ret)
			successes++;
		else if (ret != -ESRCH && !first_error)
			first_error = ret;
		put_task_struct(tasks[i]);
	}

	if (count == EVX_QOS_MAX_PROCESS_THREADS)
		first_error = -E2BIG;
	if (successes && pid_alive(leader))
		policy_ret = evx_update_process_policy(leader, item, level, active);
	else if (active && !first_error)
		first_error = -ESRCH;

	kvfree(tasks);
	put_task_struct(leader);
	if (first_error)
		return first_error;
	if (policy_ret)
		return policy_ret;
	return successes ? 0 : -ESRCH;
}

static void evx_wake_up_new_task(void *unused, struct task_struct *task)
{
	struct evx_qos_process_policy *policy;
	struct evx_qos_lut_item item;
	bool found = false;

	if (thread_group_leader(task))
		return;

	spin_lock(&evx_qos_process_policies_lock);
	policy = evx_find_process_policy_locked(task->group_leader);
	if (policy) {
		item = policy->item;
		found = true;
	}
	spin_unlock(&evx_qos_process_policies_lock);

	if (found)
		evx_apply_item_to_task(task, &item, GFP_ATOMIC);
}

static void evx_sched_process_free(void *unused, struct task_struct *task)
{
	struct evx_qos_task_state *state;
	struct evx_qos_process_policy *policy = NULL;

	spin_lock(&evx_qos_task_states_lock);
	state = evx_find_task_state_locked(task);
	if (state)
		hash_del(&state->node);
	spin_unlock(&evx_qos_task_states_lock);
	kfree(state);

	if (!thread_group_leader(task))
		return;
	spin_lock(&evx_qos_process_policies_lock);
	policy = evx_find_process_policy_locked(task);
	if (policy)
		hash_del(&policy->node);
	spin_unlock(&evx_qos_process_policies_lock);
	kfree(policy);
}

static long evx_qos_level_ioctl(struct file *file, unsigned int cmd,
				unsigned long argument)
{
	struct evx_qos_level_data data;
	struct evx_qos_lut_item item;
	void __user *user = (void __user *)argument;
	int level, ret = 0, i, successes = 0, first_error = 0;
	bool active;

	if (!evx_qos_authorized())
		return -EPERM;
	if (_IOC_TYPE(cmd) != EVX_QOS_MAGIC ||
	    _IOC_NR(cmd) <= 0 || _IOC_NR(cmd) >= EVX_SET_LEVEL_MAX ||
	    _IOC_SIZE(cmd) != sizeof(data))
		return -EINVAL;
	if (copy_from_user(&data, user, sizeof(data)))
		return -EFAULT;

	atomic64_inc(&evx_level_ioctl_requests);
	switch (cmd) {
	case EVX_IOCTL_SET_TID_LEVEL:
		ret = evx_lut_copy_item(EVX_QOS_LUT_TASK, data.level, &item);
		if (!ret)
			ret = evx_apply_level_to_tid(data.info.tid, &item, GFP_KERNEL);
		break;
	case EVX_IOCTL_SET_PID_LEVEL:
		level = data.level & EVX_QOS_PID_LEVEL_MASK;
		active = !!(data.level >> EVX_QOS_PID_ACTIVE_SHIFT);
		ret = evx_lut_copy_item(EVX_QOS_LUT_PROCESS, level, &item);
		if (!ret)
			ret = evx_apply_level_to_process(data.info.pid, &item,
							 level, active);
		break;
	case EVX_IOCTL_SET_TID_ARRAY_LEVEL:
		if (data.info.tarray.count <= 0 ||
		    data.info.tarray.count > EVX_QOS_MAX_TIDS) {
			ret = -EINVAL;
			break;
		}
		ret = evx_lut_copy_item(EVX_QOS_LUT_TASK, data.level, &item);
		if (ret)
			break;
		for (i = 0; i < data.info.tarray.count; i++) {
			int item_ret;
			pid_t tid = data.info.tarray.tids[i];

			item_ret = evx_apply_level_to_tid(tid, &item, GFP_KERNEL);
			if (!item_ret)
				successes++;
			else if (item_ret != -ESRCH && !first_error)
				first_error = item_ret;
		}
		ret = first_error ? first_error : (successes ? 0 : -ESRCH);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	evx_record_error(ret);
	return ret;
}

static long evx_qos_lut_ioctl(struct file *file, unsigned int cmd,
			      unsigned long argument)
{
	struct evx_qos_lut_file *ctx = file->private_data;
	struct evx_qos_lut_request request;
	struct evx_qos_lut_table *table;
	void __user *user = (void __user *)argument;
	int index, ret = 0;

	if (!evx_qos_authorized())
		return -EPERM;
	if (!ctx || _IOC_TYPE(cmd) != EVX_LUT_MAGIC ||
	    _IOC_NR(cmd) <= 0 || _IOC_NR(cmd) >= EVX_LUT_COMMAND_MAX ||
	    _IOC_SIZE(cmd) != sizeof(request))
		return -EINVAL;
	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;

	index = evx_lut_index(request.type);
	if (index < 0)
		return index;
	table = &evx_qos_luts[index];
	atomic64_inc(&evx_lut_ioctl_requests);

	switch (cmd) {
	case EVX_IOCTL_UPDATE_LUT:
		ret = evx_lut_replace_bank(table, ctx, &request);
		break;
	case EVX_IOCTL_GET_LUT_VERSION:
		mutex_lock(&table->lock);
		request.version = table->valid ?
			table->banks[table->active].version : 0;
		mutex_unlock(&table->lock);
		if (copy_to_user(user, &request, sizeof(request)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	evx_record_error(ret);
	return ret;
}

#ifdef CONFIG_COMPAT
static long evx_qos_level_compat_ioctl(struct file *file, unsigned int cmd,
				       unsigned long argument)
{
	return evx_qos_level_ioctl(file, cmd,
				   (unsigned long)compat_ptr(argument));
}

static long evx_qos_lut_compat_ioctl(struct file *file, unsigned int cmd,
				     unsigned long argument)
{
	return evx_qos_lut_ioctl(file, cmd,
				 (unsigned long)compat_ptr(argument));
}
#endif

static void evx_qos_lut_vma_open(struct vm_area_struct *vma)
{
	struct evx_qos_lut_bank *bank = vma->vm_private_data;

	atomic_inc(&bank->mappings);
}

static void evx_qos_lut_vma_close(struct vm_area_struct *vma)
{
	struct evx_qos_lut_bank *bank = vma->vm_private_data;

	atomic_dec(&bank->mappings);
}

static const struct vm_operations_struct evx_qos_lut_vm_ops = {
	.open = evx_qos_lut_vma_open,
	.close = evx_qos_lut_vma_close,
};

static int evx_qos_lut_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct evx_qos_lut_file *ctx = file->private_data;
	struct evx_qos_lut_bank *bank;
	size_t length = vma->vm_end - vma->vm_start;
	int ret;

	if (!evx_qos_authorized())
		return -EPERM;
	if (!ctx || !ctx->pending_bank || vma->vm_pgoff)
		return -EINVAL;
	bank = ctx->pending_bank;
	if (!bank->mem || length != bank->alloc_size)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &evx_qos_lut_vm_ops;
	vma->vm_private_data = bank;
	ret = remap_pfn_range(vma, vma->vm_start, virt_to_pfn(bank->mem),
			      length, vma->vm_page_prot);
	if (ret) {
		evx_record_error(ret);
		return ret;
	}

	/* The initial mapping is not followed by ->open(); forks are. */
	atomic_inc(&bank->mappings);
	atomic64_inc(&evx_lut_mmaps);
	return 0;
}

static int evx_qos_lut_open(struct inode *inode, struct file *file)
{
	struct evx_qos_lut_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	file->private_data = ctx;
	return 0;
}

static int evx_qos_lut_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t evx_qos_lut_read(struct file *file, char __user *buffer,
				size_t count, loff_t *position)
{
	char output[640];
	size_t length = 0;
	int i;

	length += scnprintf(output + length, sizeof(output) - length,
		"abi=real lut_ioctls=%lld mmaps=%lld last_error=%d\n",
		atomic64_read(&evx_lut_ioctl_requests),
		atomic64_read(&evx_lut_mmaps), atomic_read(&evx_last_error));
	for (i = 0; i < ARRAY_SIZE(evx_qos_luts); i++) {
		struct evx_qos_lut_table *table = &evx_qos_luts[i];
		struct evx_qos_lut_bank *bank;

		mutex_lock(&table->lock);
		bank = &table->banks[table->active];
		length += scnprintf(output + length, sizeof(output) - length,
			"%s valid=%d bank=%u levels=%u bytes=%zu version=%lld",
			evx_lut_name(i), table->valid, table->active,
			table->valid ? bank->levels : 0,
			table->valid ? bank->data_size : 0,
			table->valid ? bank->version : 0);
		length += scnprintf(output + length, sizeof(output) - length,
			" updates=%llu mappings=%d\n", table->updates,
			table->valid ? atomic_read(&bank->mappings) : 0);
		mutex_unlock(&table->lock);
	}
	return simple_read_from_buffer(buffer, count, position, output, length);
}

static ssize_t evx_qos_noop_write(struct file *file,
				  const char __user *buffer,
				  size_t count, loff_t *position)
{
	return -EOPNOTSUPP;
}

static int evx_qos_level_open(struct inode *inode, struct file *file)
{
	struct evx_qos_level_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	file->private_data = ctx;
	return 0;
}

static int evx_qos_level_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t evx_qos_level_write(struct file *file,
				   const char __user *buffer,
				   size_t count, loff_t *position)
{
	struct evx_qos_level_file *ctx = file->private_data;
	char input[24];
	int ret;

	if (!ctx || count == 0 || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buffer, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtoint(strstrip(input), 10, &ctx->reader_tid);
	return ret ? ret : count;
}

static ssize_t evx_qos_level_read(struct file *file, char __user *buffer,
				  size_t count, loff_t *position)
{
	struct evx_qos_level_file *ctx = file->private_data;
	struct evx_qos_task_state *state;
	struct task_struct *task = NULL;
	char output[256];
	int level = -1;
	size_t length;

	if (ctx && ctx->reader_tid > 0)
		task = find_get_task_by_vpid(ctx->reader_tid);
	if (task) {
		spin_lock(&evx_qos_task_states_lock);
		state = evx_find_task_state_locked(task);
		if (state)
			level = state->qos_level;
		spin_unlock(&evx_qos_task_states_lock);
		put_task_struct(task);
		length = scnprintf(output, sizeof(output), "pid: %d, level: %d\n",
				   ctx->reader_tid, level);
	} else {
		length = scnprintf(output, sizeof(output),
				   "abi=real level_ioctls=%lld applied=%lld failed=%lld",
			atomic64_read(&evx_level_ioctl_requests),
			atomic64_read(&evx_task_apply_successes),
			atomic64_read(&evx_task_apply_failures));
		length += scnprintf(output + length, sizeof(output) - length,
			" latency_fallbacks=%lld last_error=%d\n",
			atomic64_read(&evx_latency_fallbacks),
			atomic_read(&evx_last_error));
	}
	return simple_read_from_buffer(buffer, count, position, output, length);
}

static const struct proc_ops evx_qos_lut_fops = {
	.proc_open = evx_qos_lut_open,
	.proc_release = evx_qos_lut_release,
	.proc_read = evx_qos_lut_read,
	.proc_write = evx_qos_noop_write,
	.proc_mmap = evx_qos_lut_mmap,
	.proc_ioctl = evx_qos_lut_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_qos_lut_compat_ioctl,
#endif
	.proc_lseek = default_llseek,
};

static const struct proc_ops evx_qos_level_fops = {
	.proc_open = evx_qos_level_open,
	.proc_release = evx_qos_level_release,
	.proc_read = evx_qos_level_read,
	.proc_write = evx_qos_level_write,
	.proc_ioctl = evx_qos_level_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_qos_level_compat_ioctl,
#endif
	.proc_lseek = default_llseek,
};

static int __init evx_cos_qos_compat_init(void)
{
	int i, ret;

	static_assert(sizeof(struct evx_qos_lut_request) == 16);
	static_assert(sizeof(struct evx_qos_lut_item) == 32);
	static_assert(sizeof(struct evx_qos_level_data) == 264);
	static_assert(EVX_IOCTL_UPDATE_LUT == 0x40107001);
	static_assert(EVX_IOCTL_SET_TID_LEVEL == 0x41087101);
	static_assert(EVX_IOCTL_SET_PID_LEVEL == 0x41087102);
	static_assert(EVX_IOCTL_SET_TID_ARRAY_LEVEL == 0x41087103);

	for (i = 0; i < ARRAY_SIZE(evx_qos_luts); i++)
		mutex_init(&evx_qos_luts[i].lock);

	evx_qos_dir = proc_mkdir(EVX_QOS_DIR, NULL);
	if (!evx_qos_dir)
		return -ENOMEM;
	/* init.oplus_perf.rc grants the ColorOS service access after boot. */
	if (!proc_create("qos_lut", 0600, evx_qos_dir, &evx_qos_lut_fops) ||
	    !proc_create("qos_level", 0600, evx_qos_dir,
			 &evx_qos_level_fops)) {
		remove_proc_subtree(EVX_QOS_DIR, NULL);
		return -ENOMEM;
	}

	ret = register_trace_sched_process_free(evx_sched_process_free, NULL);
	if (ret)
		goto remove_proc;
	ret = register_trace_android_rvh_wake_up_new_task(evx_wake_up_new_task, NULL);
	if (ret)
		goto unregister_process_free;

	pr_info(EVX_QOS_NAME ": real ColorOS QoS ABI ready\n");
	return 0;

unregister_process_free:
	unregister_trace_sched_process_free(evx_sched_process_free, NULL);
remove_proc:
	remove_proc_subtree(EVX_QOS_DIR, NULL);
	return ret;
}

module_init(evx_cos_qos_compat_init);
MODULE_DESCRIPTION("EVONIX ColorOS QoS scheduler compatibility backend");
MODULE_LICENSE("GPL v2");
