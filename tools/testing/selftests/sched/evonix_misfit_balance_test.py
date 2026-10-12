#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host-extracted migration/balance regressions; not a kernel or device test."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from evonix_policy_test import function

ROOT = Path(__file__).resolve().parents[4]
BASELINE = "11225b427565272299fad3222985f001585ac20b"


def source(baseline=False):
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{BASELINE}:kernel/sched/fair.c"],
            cwd=ROOT, text=True)
    return (ROOT / "kernel/sched/fair.c").read_text()


def definition(text, name, signature):
    body = function(re.sub(r"/\*.*?\*/", "", text, flags=re.S), name)
    return signature + " {\n" + body + "\n}\n"


COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define CPU_NOT_IDLE 0
#define CPU_IDLE 1
#define CPU_NEWLY_IDLE 2
enum cpu_idle_type { not_idle = 0, idle = 1, newly_idle = 2 };
enum migration_type { migrate_load, migrate_util, migrate_task, migrate_misfit };
#define LBF_DST_PINNED 1
#define LBF_ACTIVE_LB 2
#define LBF_SOME_PINNED 4
#define LBF_ALL_PINNED 8
#define LBF_NEED_BREAK 16
#define SD_ASYM_CPUCAPACITY 1
#define SCHED_NR_MIGRATE_BREAK 32
#define MAX_PINNED_INTERVAL 512
#define min(a, b) ((a) < (b) ? (a) : (b))
#define likely(x) (x)
#define unlikely(x) (x)
#define schedstat_inc(x) ((x)++)
#define schedstat_add(x, y) ((x) += (y))
#define lockdep_assert_rq_held(rq) assert((rq)->locked)
struct cpumask { unsigned int bits; };
struct task_struct {
    struct cpumask *cpus_ptr;
    bool migration_disabled, throttled, pcpu, running, cookie;
    int locality, hot;
    unsigned int fits;
    struct {
        int nr_failed_migrations_affine, nr_failed_migrations_running;
        int nr_forced_migrations, nr_failed_migrations_hot;
    } stats;
};
struct rq {
    int cpu, nr_running, ttwu_pending, active_balance, push_cpu;
    bool locked, idle;
    unsigned long capacity;
    struct task_struct *curr;
    int active_balance_work;
};
struct sched_group {
    struct cpumask mask;
    struct { int imbalance; } storage, *sgc;
};
struct sched_domain {
    struct sched_domain *parent;
    struct sched_group *groups;
    unsigned int flags;
    int nr_balance_failed, cache_nice_tries;
    unsigned int balance_interval, min_interval, max_interval;
    unsigned int lb_count[3], lb_nobusyg[3], lb_nobusyq[3], lb_imbalance[3];
    unsigned int lb_failed[3], lb_balanced[3], lb_hot_gained[3];
};
struct rq_flags { unsigned long flags; };
struct list_head { int unused; };
struct lb_env {
    struct rq *src_rq, *dst_rq;
    struct sched_domain *sd;
    int src_cpu, dst_cpu, new_dst_cpu;
    enum cpu_idle_type idle;
    unsigned int flags, loop, loop_break, loop_max;
    struct cpumask *dst_grpmask, *cpus;
    enum migration_type migration_type;
    long imbalance;
    int fbq_type;
    struct list_head tasks;
    struct rq_flags *src_rq_rf;
};
static struct rq queues[8];
#define cpu_rq(cpu) (&queues[cpu])
#define cpu_of(rq) ((rq)->cpu)
#define capacity_of(cpu) (queues[cpu].capacity)
#define capacity_greater(a, b) ((a) * 1024 > (b) * 1078)
static bool cpumask_test_cpu(int cpu, const struct cpumask *mask)
{
    return mask->bits & (1U << cpu);
}
"""

MIGRATION = r"""
#define for_each_cpu_and(i, a, b) \
    for ((i) = 0; (i) < 8; (i)++) if (((a)->bits & (b)->bits) & (1U << (i)))
static int vendor_veto, task_fit_queries;
static unsigned int sysctl_sched_migration_cost;
#define trace_android_rvh_can_migrate_task(p, cpu, answer) \
    ((void)(p), (void)(cpu), *(answer) = !vendor_veto)
#define task_group(p) (p)
#define throttled_lb_pair(p, src, dst) ((void)(src), (void)(dst), (p)->throttled)
#define kthread_is_per_cpu(p) ((p)->pcpu)
#define task_on_cpu(rq, p) ((void)(rq), (p)->running)
#define is_migration_disabled(p) ((p)->migration_disabled)
#define migrate_degrades_locality(p, env) ((void)(env), (p)->locality)
#define task_hot(p, env) ((void)(env), (p)->hot)
#define sched_core_cookie_match(rq, p) ((void)(rq), (p)->cookie)
static bool available_idle_cpu(int cpu)
{
    return queues[cpu].idle && !queues[cpu].nr_running && !queues[cpu].ttwu_pending;
}
static bool task_fits_cpu(struct task_struct *p, int cpu)
{
    task_fit_queries++;
    return p->fits & (1U << cpu);
}
/* EXTRACTED */
int main(void)
{
    struct cpumask group = {.bits = 0xff}, cpus = {.bits = 0xff};
    /* Synthetic capacity groups, not measured Rodin power data. */
    const unsigned long capacities[8] = {550, 550, 550, 550, 750, 750, 750, 1024};
    for (int src = 0; src < 8; src++) {
        for (int dst = 0; dst < 8; dst++) {
            if (src == dst) continue;
            for (int mode = 0; mode < 19; mode++) {
                for (int kind = migrate_load; kind <= migrate_misfit; kind++) {
                    memset(queues, 0, sizeof(queues));
                    for (int cpu = 0; cpu < 8; cpu++) {
                        queues[cpu].capacity = capacities[cpu];
                        queues[cpu].locked = true;
                        queues[cpu].cpu = cpu;
                    }
                    struct cpumask affinity = {.bits = 0xff};
                    struct sched_domain sd = {.flags = SD_ASYM_CPUCAPACITY,
                        .cache_nice_tries = 1};
                    struct task_struct p = {.cpus_ptr = &affinity,
                        .cookie = true, .hot = 1, .locality = -1,
                        .fits = 1U << dst};
                    struct lb_env env = {.src_rq = &queues[src], .dst_rq = &queues[dst],
                        .sd = &sd, .src_cpu = src, .dst_cpu = dst, .idle = CPU_IDLE,
                        .dst_grpmask = &group, .cpus = &cpus,
                        .migration_type = kind, .flags = LBF_ALL_PINNED};
                    vendor_veto = task_fit_queries = 0;
                    queues[dst].idle = true;
                    sysctl_sched_migration_cost = 500000U;
                    switch (mode) {
                    case 1: vendor_veto = 1; break;
                    case 2: p.throttled = true; break;
                    case 3: p.pcpu = true; break;
                    case 4: p.running = true; break;
                    case 5: affinity.bits = 1U << src; break;
                    case 6: p.migration_disabled = true; affinity.bits = 1U << src; break;
                    case 7: p.cookie = false; break;
                    case 8: queues[dst].idle = false; break;
                    case 9: queues[dst].ttwu_pending = 1; break;
                    case 10: queues[dst].nr_running = 1; break;
                    case 11: env.idle = CPU_NOT_IDLE; break;
                    case 12: sd.flags = 0; break;
                    case 13: p.fits |= 1U << src; break;
                    case 14: p.fits = 0; break; /* Includes thermal/uclamp fit rejection. */
                    case 15: p.locality = 1; break;
                    case 16: sysctl_sched_migration_cost = (unsigned int)-1; break;
                    case 17: env.idle = CPU_NEWLY_IDLE; break;
                    case 18: p.hot = 0; break;
                    }
                    int result = can_migrate_task(&p, &env);
                    printf("%d %d %d %d %d %u %d %d %d %d\n", src, dst, mode, kind,
                        result, env.flags, p.stats.nr_failed_migrations_hot,
                        p.stats.nr_forced_migrations, sd.lb_hot_gained[env.idle],
                        task_fit_queries);
                }
            }
        }
    }
    return 0;
}
"""

BALANCE = r"""
#define all 2
#define LIST_HEAD_INIT(head) {0}
static struct cpumask balance_mask, active_mask = {.bits = 3};
#define cpu_active_mask (&active_mask)
#define this_cpu_cpumask_var_ptr(mask) (&balance_mask)
#define sched_domain_span(sd) (&active_mask)
#define group_balance_mask(sg) (&(sg)->mask)
#define cpumask_and(dst, a, b) ((dst)->bits = (a)->bits & (b)->bits)
#define cpumask_subset(a, b) (!((a)->bits & ~(b)->bits))
#define __cpumask_clear_cpu(cpu, mask) ((mask)->bits &= ~(1U << (cpu)))
#define WARN_ON_ONCE(x) assert(!(x))
#define rq_lock_irqsave(rq, rf) \
    do { assert(!(rq)->locked); (rq)->locked = true; (rf)->flags = 0; } while (0)
#define rq_unlock(rq, rf) \
    do { (void)(rf); assert((rq)->locked); (rq)->locked = false; } while (0)
#define raw_spin_rq_lock_irqsave(rq, flags) \
    do { assert(!(rq)->locked); (rq)->locked = true; (flags) = 0; } while (0)
#define raw_spin_rq_unlock_irqrestore(rq, flags) \
    do { (void)(flags); assert((rq)->locked); (rq)->locked = false; } while (0)
#define update_rq_clock(rq) assert((rq)->locked)
#define local_irq_restore(flags) ((void)(flags))
#define preempt_disable() ((void)0)
#define preempt_enable() ((void)0)
static unsigned int sysctl_sched_nr_migrate = 32;
static int mode, selected_kind, stop_calls;
static bool should_we_balance(struct lb_env *env) { (void)env; return mode != 6; }
static struct sched_group *find_busiest_group(struct lb_env *env)
{
    env->migration_type = selected_kind;
    env->imbalance = 1;
    return mode == 5 ? NULL : env->sd->groups;
}
static struct rq *find_busiest_queue(struct lb_env *env, struct sched_group *group)
{
    (void)env; (void)group;
    return mode == 7 ? NULL : &queues[0];
}
static int detach_tasks(struct lb_env *env)
{
    assert(env->src_rq->locked);
    if (mode == 3) return 0; /* All tasks pinned or vendor-vetoed. */
    env->flags &= ~LBF_ALL_PINNED;
    if (mode == 4) { env->imbalance = 0; return 1; }
    return 0; /* Cache-hot task. */
}
static void attach_tasks(struct lb_env *env) { assert(!env->src_rq->locked); }
static bool need_active_balance(struct lb_env *env)
{
    return mode == 1 || env->migration_type == migrate_misfit;
}
static int active_load_balance_cpu_stop(void *data) { (void)data; return 0; }
static void stop_one_cpu_nowait(int cpu, int (*fn)(void *), void *data, int *work)
{
    (void)fn; (void)data; (void)work;
    assert(cpu == 0 && !queues[cpu].locked);
    stop_calls++;
}
/* EXTRACTED */
int main(void)
{
    struct cpumask affinity;
    struct task_struct running = {.cpus_ptr = &affinity};
    struct sched_group group = {.mask = {.bits = 2}};
    group.sgc = &group.storage;
    for (int kind = migrate_load; kind <= migrate_misfit; kind++) {
        for (int idle_kind = CPU_NOT_IDLE; idle_kind <= CPU_NEWLY_IDLE; idle_kind++) {
            for (mode = 0; mode < 8; mode++) {
                for (int trial = 0; trial < 12; trial++) {
                    memset(queues, 0, sizeof(queues));
                    queues[0].cpu = 0;
                    queues[1].cpu = 1;
                    queues[0].curr = &running;
                    queues[0].nr_running = mode == 1 ? 1 : 2;
                    affinity.bits = mode == 1 ? 1 : 3;
                    struct sched_domain sd = {.groups = &group,
                        .nr_balance_failed = trial % 4,
                        .balance_interval = 1U << (trial % 10),
                        .min_interval = 1, .max_interval = 64};
                    selected_kind = kind;
                    stop_calls = 0;
                    int continue_balancing = 1;
                    int result = load_balance(1, &queues[1], &sd,
                                              idle_kind, &continue_balancing);
                    assert(!queues[0].locked && !queues[1].locked);
                    printf("%d %d %d %d %d %d %u %d %d %u %u %u\n", kind,
                        idle_kind, mode, trial, result, sd.nr_balance_failed,
                        sd.balance_interval, stop_calls, continue_balancing,
                        sd.lb_failed[idle_kind], sd.lb_balanced[idle_kind],
                        balance_mask.bits);
                }
            }
        }
    }
    return 0;
}
"""


class MisfitBalanceTests(unittest.TestCase):
    def run_c(self, harness, declaration):
        if not shutil.which("cc"):
            self.skipTest("Host C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="evonix-misfit-") as directory:
            exe = Path(directory) / "test"
            compiled = subprocess.run(
                ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wno-sign-compare", "-Wno-unused-function", "-x", "c", "-", "-o", str(exe)],
                input=COMMON + harness.replace("/* EXTRACTED */", declaration),
                text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            output = subprocess.check_output([str(exe)], text=True)
        return [tuple(map(int, row.split())) for row in output.splitlines()]

    def migration_outputs(self):
        return [self.run_c(MIGRATION, definition(source(old), "can_migrate_task",
            "static int can_migrate_task(struct task_struct *p, struct lb_env *env)"))
            for old in (True, False)]

    def balance_outputs(self):
        return [self.run_c(BALANCE, definition(source(old), "load_balance",
            "static int load_balance(int this_cpu, struct rq *this_rq, struct sched_domain *sd, enum cpu_idle_type idle, int *continue_balancing)"))
            for old in (True, False)]

    def test_only_idle_fitting_misfit_upgrades_bypass_cache_delay(self):
        old, new = self.migration_outputs()
        self.assertEqual(len(old), 4256)
        changes = 0
        capacities = [550] * 4 + [750] * 3 + [1024]
        for a, b in zip(old, new):
            src, dst, mode, kind = b[:4]
            uplift = capacities[dst] * 1024 > capacities[src] * 1078
            permitted = kind == 3 and mode in (0, 17, 18) and uplift
            if permitted:
                self.assertEqual(b[4], 1)
                self.assertEqual(b[6:9], (0, 0, 0))
                if mode != 18:
                    self.assertEqual(a[4], 0)
                    changes += 1
            else:
                self.assertEqual(a[:-1], b[:-1])
            if kind != 3:
                self.assertEqual(b[-1], 0, "Ordinary balancing must not query fit")
        self.assertGreater(changes, 0)

    def test_vendor_affinity_running_and_locality_guards_precede_fit_queries(self):
        _, new = self.migration_outputs()
        for row in new:
            if row[2] in (1, 2, 3, 4, 5, 6, 15):
                self.assertEqual(row[4], 0)
                self.assertEqual(row[-1], 0)

    def test_misfit_failures_do_not_raise_ordinary_migration_pressure(self):
        old, new = self.balance_outputs()
        self.assertEqual(len(old), 1152)
        differences = 0
        for a, b in zip(old, new):
            kind, idle_kind, mode, trial = b[:4]
            if kind != 3:
                self.assertEqual(a, b)
                continue
            # Failure counters change only after a real unsuccessful pull.
            if mode in (0, 1, 2):
                self.assertEqual(b[5], trial % 4)
                if idle_kind != 2:
                    self.assertEqual(a[5], b[5] + 1)
                    differences += 1
            else:
                self.assertEqual(a[5], b[5])
            # Pinning paths preserve their interval rather than doubling it.
            if mode in (1, 3, 5, 7):
                self.assertEqual(b[6], 1 << (trial % 10))
            else:
                self.assertEqual(a[6], b[6])
            self.assertEqual(a[7:], b[7:])
            self.assertEqual(a[4], b[4])
        self.assertGreater(differences, 0)

    def test_no_new_scheduler_state_or_global_threshold_change(self):
        before = source(True)
        after = source()
        old_env = re.search(r"struct lb_env \{.*?\n\};", before, re.S).group()
        new_env = re.search(r"struct lb_env \{.*?\n\};", after, re.S).group()
        self.assertEqual(old_env, new_env)
        self.assertEqual(function(before, "task_hot"), function(after, "task_hot"))
        self.assertEqual(function(before, "detach_tasks"), function(after, "detach_tasks"))
        self.assertEqual(function(before, "need_active_balance"), function(after, "need_active_balance"))
        self.assertIn("sysctl_sched_migration_cost\t= 500000UL;", after)

    def test_repeated_misfit_failures_do_not_force_unrelated_hot_tasks(self):
        main = r"""
int main(void)
{
    struct cpumask affinity = {.bits = 3};
    struct sched_group group = {.mask = {.bits = 2}};
    group.sgc = &group.storage;
    struct task_struct p = {.cpus_ptr = &affinity, .hot = 1, .locality = -1};
    struct sched_domain sd = {.groups = &group, .cache_nice_tries = 1,
        .balance_interval = 1, .min_interval = 1, .max_interval = 64};
    queues[0].curr = &p;
    queues[0].nr_running = 2;
    queues[0].capacity = queues[1].capacity = 1024;
    sysctl_sched_migration_cost = 500000U;
    selected_kind = migrate_misfit;
    for (int attempt = 0; attempt < 8; attempt++) {
        int continue_balancing = 1;
        queues[0].active_balance = 0;
        assert(!load_balance(1, &queues[1], &sd, CPU_IDLE, &continue_balancing));
    }
    struct lb_env env = {.src_rq = &queues[0], .dst_rq = &queues[1],
        .sd = &sd, .src_cpu = 0, .dst_cpu = 1, .idle = CPU_IDLE,
        .dst_grpmask = &group.mask, .cpus = &affinity,
        .migration_type = migrate_load};
    queues[0].locked = true;
    int moved = can_migrate_task(&p, &env);
    printf("%d %d %d %d\n", sd.nr_balance_failed, moved,
        p.stats.nr_forced_migrations, p.stats.nr_failed_migrations_hot);
    return 0;
}
"""
        results = []
        for old in (True, False):
            text = source(old)
            migration = definition(text, "can_migrate_task",
                "static int can_migrate_task(struct task_struct *p, struct lb_env *env)")
            balance = definition(text, "load_balance",
                "static int load_balance(int this_cpu, struct rq *this_rq, struct sched_domain *sd, enum cpu_idle_type idle, int *continue_balancing)")
            harness = MIGRATION.split("int main(void)", 1)[0].replace(
                "/* EXTRACTED */", migration)
            harness += BALANCE.split("int main(void)", 1)[0].replace(
                "/* EXTRACTED */", balance)
            results.append(self.run_c(harness + main, ""))
        self.assertEqual(results[0], [(8, 1, 1, 0)])
        self.assertEqual(results[1], [(0, 0, 0, 1)])


if __name__ == "__main__":
    unittest.main()
