#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extracted common-kernel fast paths; not a kernel build or device benchmark."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from evonix_policy_test import function as extract_function

ROOT = Path(__file__).resolve().parents[4]
BASELINE = "9e26f60f4f5df190216d7949f0aa44a665b2c1f4"


def source(path, baseline=False):
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{BASELINE}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def body(text, name):
    return extract_function(re.sub(r"/\*.*?\*/", "", text, flags=re.S), name)


def definition(text, name, declaration):
    return declaration + " {\n" + body(text, name) + "\n}\n"


COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define unlikely(x) (x)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
static uint64_t seed = 6899;
static unsigned int random_value(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (unsigned int)seed;
}
"""

ACCOUNTING = r"""
#define CPUFREQ_TABLE_UNSORTED 0
struct cpu_freqs {
    unsigned int max_state, last_index, freq_table[32];
};
struct cpufreq_stats {
    unsigned int total_trans, max_state, state_num, last_index;
    u64 last_time, reset_time, time_in_state[32];
    unsigned int reset_pending, freq_table[32], trans_table[1024];
};
struct cpufreq_policy {
    int cpu, freq_table_sorted;
    struct cpufreq_stats *stats;
};
static struct cpu_freqs *all_freqs[8];
static unsigned int lookups;
static u64 now;
#define smp_rmb() ((void)0)
static u64 local_clock(void) { return now; }
/* EXTRACTED */
static void print_state(struct cpufreq_stats *s, struct cpu_freqs *f)
{
    unsigned long long hash = 0;
    for (unsigned int i = 0; i < s->max_state; i++)
        hash = hash * 31 + s->time_in_state[i];
    for (unsigned int i = 0; i < s->max_state * s->max_state; i++)
        hash = hash * 31 + s->trans_table[i];
    printf("%u %u %u %u %llu %llu %u\n", f->last_index, s->last_index,
           s->total_trans, s->reset_pending,
           (unsigned long long)s->last_time, hash, lookups);
}
int main(void)
{
    struct cpufreq_policy policy = {.cpu = 0};
    struct cpufreq_stats stats = {0};
    struct cpu_freqs freqs = {0};
    policy.stats = NULL;
    cpufreq_stats_record_transition(&policy, 300000);
    cpufreq_times_record_transition(&policy, 300000);
    assert(!lookups);
    policy.stats = &stats;
    all_freqs[0] = &freqs;
    for (int trial = 0; trial < 12000; trial++) {
        memset(&stats, 0, sizeof(stats));
        memset(&freqs, 0, sizeof(freqs));
        unsigned int n = 1 + random_value() % 19;
        policy.freq_table_sorted = trial % 3;
        freqs.max_state = n;
        stats.max_state = n + random_value() % 4;
        stats.state_num = n;
        for (unsigned int i = 0; i < n; i++) {
            unsigned int f = (i + 3) * 100000;
            stats.freq_table[i] = f;
            freqs.freq_table[i] = policy.freq_table_sorted == 2 ?
                                  (n - i + 2) * 100000 : f;
        }
        /* Times tables may retain duplicate entries when unsorted. */
        if (!policy.freq_table_sorted && n > 1)
            freqs.freq_table[n - 1] = freqs.freq_table[0];
        freqs.last_index = random_value() % (n + 3);
        stats.last_index = trial % 7 ? random_value() % n : (unsigned int)-1;
        stats.last_time = now = 100000;
        for (int step = 0; step < 12; step++) {
            now += 10 + random_value() % 1000;
            unsigned int target = stats.freq_table[random_value() % n];
            if (step % 3 == 0 && stats.last_index < n)
                target = stats.freq_table[stats.last_index];
            if (step % 11 == 0) target = 999999; /* Unknown frequency. */
            if (step == 7) target = 0; /* Includes unused stats tail slots. */
            if (step % 4 == 0 && stats.last_index < stats.max_state) {
                stats.reset_time = now - 5;
                stats.reset_pending = 1;
            }
            lookups = 0;
            cpufreq_stats_record_transition(&policy, target);
            cpufreq_times_record_transition(&policy, target);
            print_state(&stats, &freqs);
        }
    }
    return 0;
}
"""

BALANCE = r"""
struct cpumask { unsigned int bits; };
#define for_each_cpu_and(i, a, b) \
    for ((i) = 0; (i) < 8; (i)++) if (((a)->bits & (b)->bits) & (1U << (i)))
enum fbq_type { regular, remote, all };
enum migration_type { migrate_load, migrate_util, migrate_task, migrate_misfit };
#define SD_ASYM_CPUCAPACITY 1
#define SD_ASYM_PACKING 2
#define MISFIT_PERF 1
struct rq {
    struct { unsigned int h_nr_running; } cfs;
    unsigned long capacity, load, util, misfit_task_load;
    int misfit_reason, id, type;
};
struct sched_domain { unsigned int flags; };
struct lb_env {
    int dst_cpu;
    struct cpumask *cpus;
    struct sched_domain *sd;
    enum fbq_type fbq_type;
    enum migration_type migration_type;
    unsigned long imbalance;
};
struct sched_group { struct cpumask span; };
static struct rq queues[8];
static unsigned int queries;
static int override;
#define cpu_rq(i) (&queues[i])
#define capacity_of(i) (queues[i].capacity)
#define fbq_classify_rq(rq) ((enum fbq_type)(rq)->type)
#define capacity_greater(a, b) ((a) * 1024 > (b) * 1078)
#define sched_use_asym_prio(sd, i) (((sd)->flags & SD_ASYM_PACKING) && ((i) & 1))
#define sched_asym_prefer(a, b) ((a) < (b))
#define cpu_load(rq) ((rq)->load)
#define check_cpu_capacity(rq, sd) ((void)(sd), (rq)->capacity < 512)
#define sched_group_span(g) (&(g)->span)
static unsigned long cpu_util_cfs_boost(int cpu)
{
    queries++;
    return queues[cpu].util;
}
static void trace_android_rvh_find_busiest_queue(int dst, struct sched_group *g,
                            struct cpumask *cpus, struct rq **rq, int *done)
{
    (void)dst; (void)g; (void)cpus;
    if (override) { *rq = &queues[3]; *done = 1; }
}
/* EXTRACTED */
int main(void)
{
    struct sched_domain sd;
    struct cpumask cpus;
    struct sched_group group;
    struct lb_env env = {.sd = &sd, .cpus = &cpus};
    for (int trial = 0; trial < 12000; trial++) {
        sd.flags = random_value() % 4;
        cpus.bits = random_value() % 256;
        group.span.bits = random_value() % 256;
        env.dst_cpu = random_value() % 8;
        env.fbq_type = random_value() % 3;
        env.migration_type = trial % 4;
        env.imbalance = random_value() % 2049;
        override = trial % 13 == 0;
        for (int i = 0; i < 8; i++) {
            queues[i] = (struct rq){.id = i,
                .cfs.h_nr_running = random_value() % 5,
                .capacity = 256 + random_value() % 769,
                .load = random_value() % 4097,
                .util = random_value() % 1025,
                .misfit_task_load = random_value() % 2049,
                .misfit_reason = random_value() % 2,
                .type = random_value() % 3};
        }
        queries = 0;
        struct rq *winner = find_busiest_queue(&env, &group);
        printf("%d %u\n", winner ? winner->id : -1, queries);
        if (override) assert(!queries && winner == &queues[3]);
    }
    return 0;
}
"""


MIGRATION = r"""
struct cpumask { unsigned int bits; };
static unsigned int probes;
#define for_each_cpu_and(i, a, b) \
    for ((i) = 0; (i) < 8; (i)++) if (((a)->bits & (b)->bits) & (1U << (i)))
static bool cpumask_test_cpu(int cpu, struct cpumask *mask)
{
    probes++;
    return mask->bits & (1U << cpu);
}
#define CPU_NEWLY_IDLE 2
#define CPU_NOT_IDLE 0
#define SD_ASYM_CPUCAPACITY 1
enum migration_type { migrate_load, migrate_util, migrate_task, migrate_misfit };
#define sysctl_sched_migration_cost 500000UL
#define capacity_of(cpu) ((void)(cpu), 1024UL)
#define capacity_greater(a, b) ((a) * 1024 > (b) * 1078)
#define available_idle_cpu(cpu) ((void)(cpu), false)
#define task_fits_cpu(p, cpu) ((void)(p), (void)(cpu), true)
#define sched_core_cookie_match(rq, p) ((void)(rq), (void)(p), true)
#define LBF_DST_PINNED 1
#define LBF_ACTIVE_LB 2
#define LBF_SOME_PINNED 4
#define LBF_ALL_PINNED 8
#define schedstat_inc(x) ((x)++)
#define lockdep_assert_rq_held(rq) assert((rq)->locked)
struct rq { bool locked; };
struct task_struct {
    struct cpumask *cpus_ptr;
    bool migration_disabled, throttled, pcpu, running;
    int hot, locality;
    struct {
        int nr_failed_migrations_affine, nr_failed_migrations_running;
        int nr_forced_migrations, nr_failed_migrations_hot;
    } stats;
};
struct sched_domain {
    int nr_balance_failed, cache_nice_tries, lb_hot_gained[3];
    unsigned int flags;
};
struct lb_env {
    struct rq *src_rq, *dst_rq;
    struct sched_domain *sd;
    enum migration_type migration_type;
    int src_cpu, dst_cpu, idle, new_dst_cpu;
    unsigned int flags;
    struct cpumask *dst_grpmask, *cpus;
};
static int vendor_veto;
#define trace_android_rvh_can_migrate_task(p, cpu, answer) \
    ((void)(p), (void)(cpu), *(answer) = !vendor_veto)
#define task_group(p) (p)
#define throttled_lb_pair(p, src, dst) ((void)(src), (void)(dst), (p)->throttled)
#define kthread_is_per_cpu(p) ((p)->pcpu)
#define task_on_cpu(rq, p) ((void)(rq), (p)->running)
#define is_migration_disabled(p) ((p)->migration_disabled)
#define migrate_degrades_locality(p, env) ((void)(env), (p)->locality)
#define task_hot(p, env) ((void)(env), (p)->hot)
/* EXTRACTED */
int main(void)
{
    struct rq rq = {.locked = true};
    struct sched_domain sd;
    struct cpumask affinity, group, cpus;
    struct lb_env env = {.src_rq = &rq, .sd = &sd,
        .dst_grpmask = &group, .cpus = &cpus, .src_cpu = 7};
    for (int trial = 0; trial < 12000; trial++) {
        struct task_struct task = {.cpus_ptr = &affinity};
        sd = (struct sched_domain){.nr_balance_failed = random_value() % 4,
                                  .cache_nice_tries = random_value() % 3};
        task.migration_disabled = trial % 3 == 0;
        /* Scheduled-out migrate_disable tasks have a source-only cpus_ptr. */
        affinity.bits = task.migration_disabled ? 0x80 : random_value() % 256;
        task.throttled = trial % 17 == 0;
        task.pcpu = trial % 19 == 0;
        task.running = trial % 23 == 0;
        task.hot = random_value() % 2;
        task.locality = (int)(random_value() % 3) - 1;
        group.bits = random_value() % 128;
        cpus.bits = random_value() % 128;
        env.dst_cpu = random_value() % 7;
        env.idle = random_value() % 3;
        env.flags = random_value() % 16;
        env.new_dst_cpu = -1;
        vendor_veto = trial % 29 == 0;
        probes = 0;
        int result = can_migrate_task(&task, &env);
        printf("%d %u %d %d %d %d %d %d %u\n", result, env.flags,
            env.new_dst_cpu, task.stats.nr_failed_migrations_affine,
            task.stats.nr_failed_migrations_running, task.stats.nr_forced_migrations,
            task.stats.nr_failed_migrations_hot, sd.lb_hot_gained[env.idle], probes);
        if (task.migration_disabled) assert(!result && env.new_dst_cpu == -1);
    }
    return 0;
}
"""

ENERGY = r"""
struct energy_env { unsigned long cpu_cap; };
struct task_struct { int unused; };
struct cpumask { unsigned int bits; };
#define for_each_cpu(i, mask) \
    for ((i) = 0; (i) < 8; (i)++) if ((mask)->bits & (1U << (i)))
#define FREQUENCY_UTIL 1
static unsigned long values[8];
static unsigned int reads, callbacks, visited, task_visited;
static bool vendor_hook;
static unsigned long cpu_util(int cpu, struct task_struct *p, int dst, int boost)
{
    (void)p; (void)dst; assert(boost == 1);
    reads++;
    return values[cpu];
}
static unsigned long effective_cpu_util(int cpu, unsigned long util, int type,
                                       struct task_struct *p)
{
    assert(type == FREQUENCY_UTIL);
    if (vendor_hook) {
        callbacks++;
        visited |= 1U << cpu;
        if (p) task_visited |= 1U << cpu;
    }
    return util;
}
static bool trace_android_rvh_effective_cpu_util_enabled(void)
{
    return vendor_hook;
}
/* EXTRACTED */
int main(void)
{
    struct task_struct task = {0};
    for (int trial = 0; trial < 12000; trial++) {
        struct energy_env env = {.cpu_cap = random_value() % 1025};
        struct cpumask mask = {.bits = random_value() % 256};
        int dst = (int)(random_value() % 9) - 1;
        vendor_hook = trial % 2;
        for (int i = 0; i < 8; i++) {
            values[i] = random_value() % 1025;
            if (i == 0 && trial % 3 == 0) values[i] = 1024;
        }
        reads = callbacks = visited = task_visited = 0;
        unsigned long result = eenv_pd_max_util(&env, &mask, &task, dst);
        printf("%lu %u %u %u %u\n", result, callbacks, visited, task_visited, reads);
        if (vendor_hook) assert(visited == mask.bits);
    }
    return 0;
}
"""


class ExtractedFastPaths(unittest.TestCase):
    def run_c(self, harness, declarations):
        if not shutil.which("cc"):
            self.skipTest("Host C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="evonix-dvfs-placement-") as directory:
            executable = Path(directory) / "test"
            compiled = subprocess.run(
                ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wno-sign-compare", "-Wno-unused-function", "-x", "c", "-", "-o", str(executable)],
                input=COMMON + harness.replace("/* EXTRACTED */", declarations),
                text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            output = subprocess.check_output([str(executable)], text=True)
        return [tuple(map(int, line.split())) for line in output.splitlines()]

    def assert_equivalent_less_work(self, old, new):
        self.assertEqual(len(old), len(new))
        self.assertEqual([row[:-1] for row in old], [row[:-1] for row in new])
        self.assertTrue(all(b[-1] <= a[-1] for a, b in zip(old, new)))
        self.assertLess(sum(row[-1] for row in new), sum(row[-1] for row in old))

    def test_repeated_opp_accounting_preserves_resets_and_transitions(self):
        outputs = []
        for baseline in (True, False):
            stats = source("drivers/cpufreq/cpufreq_stats.c", baseline)
            times = source("drivers/cpufreq/cpufreq_times.c", baseline)
            declarations = ""
            for text, name, signature in (
                (stats, "freq_table_get_index", "static int freq_table_get_index(struct cpufreq_stats *stats, unsigned int freq)"),
                (times, "cpufreq_times_get_index", "static int cpufreq_times_get_index(struct cpu_freqs *freqs, unsigned int freq)"),
                (stats, "cpufreq_stats_update", "static void cpufreq_stats_update(struct cpufreq_stats *stats, unsigned long long time)"),
                (stats, "cpufreq_stats_reset_table", "static void cpufreq_stats_reset_table(struct cpufreq_stats *stats)"),
                (stats, "cpufreq_stats_record_transition", "static void cpufreq_stats_record_transition(struct cpufreq_policy *policy, unsigned int new_freq)"),
                (times, "cpufreq_times_record_transition", "static void cpufreq_times_record_transition(struct cpufreq_policy *policy, unsigned int new_freq)"),
            ):
                extracted = definition(text, name, signature)
                if name in ("freq_table_get_index", "cpufreq_times_get_index"):
                    extracted = extracted.replace("{\n", "{\nlookups++;\n", 1)
                declarations += extracted
            outputs.append(self.run_c(ACCOUNTING, declarations))
        self.assert_equivalent_less_work(*outputs)

    def test_busiest_queue_preserves_all_selection_modes_and_vendor_override(self):
        outputs = []
        for baseline in (True, False):
            declarations = definition(source("kernel/sched/fair.c", baseline),
                "find_busiest_queue", "static struct rq *find_busiest_queue(struct lb_env *env, struct sched_group *group)")
            outputs.append(self.run_c(BALANCE, declarations))
        self.assert_equivalent_less_work(*outputs)

    def test_hardware_fast_switch_and_android_transition_hook_unchanged(self):
        old = source("drivers/cpufreq/cpufreq.c", True)
        new = source("drivers/cpufreq/cpufreq.c")
        self.assertEqual(body(old, "cpufreq_driver_fast_switch"),
                         body(new, "cpufreq_driver_fast_switch"))

    def test_migration_disabled_skips_only_useless_alternate_search(self):
        outputs = []
        for baseline in (True, False):
            declarations = definition(source("kernel/sched/fair.c", baseline),
                "can_migrate_task", "static int can_migrate_task(struct task_struct *p, struct lb_env *env)")
            outputs.append(self.run_c(MIGRATION, declarations))
        self.assert_equivalent_less_work(*outputs)

    def test_saturated_eas_query_preserves_vendor_callback_traversal(self):
        outputs = []
        for baseline in (True, False):
            declarations = definition(source("kernel/sched/fair.c", baseline),
                "eenv_pd_max_util", "static unsigned long eenv_pd_max_util(struct energy_env *eenv, struct cpumask *pd_cpus, struct task_struct *p, int dst_cpu)")
            outputs.append(self.run_c(ENERGY, declarations))
        self.assert_equivalent_less_work(*outputs)

    def test_busy_time_accounting_is_not_pruned(self):
        old = source("kernel/sched/fair.c", True)
        new = source("kernel/sched/fair.c")
        self.assertEqual(body(old, "eenv_pd_busy_time"), body(new, "eenv_pd_busy_time"))

    def test_unsorted_times_tables_use_canonical_lookup(self):
        text = body(source("drivers/cpufreq/cpufreq_times.c"), "cpufreq_times_record_transition")
        self.assertIn("policy->freq_table_sorted != CPUFREQ_TABLE_UNSORTED", text)


if __name__ == "__main__":
    unittest.main()
