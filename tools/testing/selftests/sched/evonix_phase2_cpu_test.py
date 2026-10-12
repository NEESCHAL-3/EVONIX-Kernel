#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Phase 2 source contracts and extracted-C equivalence, not a kernel build."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from evonix_policy_test import function as extract_function


ROOT = Path(__file__).resolve().parents[4]
BASELINE = "28c35d7de6f2bd4c8039c640e6937f430ce1c0de"
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
CORE = (ROOT / "kernel/sched/core.c").read_text()
CPUFREQ = (ROOT / "drivers/cpufreq/cpufreq.c").read_text()


def function(source, name):
    return extract_function(re.sub(r"/\*.*?\*/", "", source, flags=re.S), name)


class Phase2Contracts(unittest.TestCase):
    def test_policy_mask_is_initialized_before_publication(self):
        body = function(CPUFREQ, "cpufreq_policy_alloc")
        self.assertLess(body.index("zalloc_cpumask_var(&policy->cpus"),
                        body.index("kobject_init_and_add"))

    def test_policy_lock_is_initialized_before_publication(self):
        body = function(CPUFREQ, "cpufreq_policy_alloc")
        self.assertLess(body.index("init_rwsem(&policy->rwsem)"),
                        body.index("kobject_init_and_add"))

    def test_idle_search_reuses_caller_snapshot(self):
        child = function(FAIR, "select_idle_capacity")
        self.assertNotIn("task_util_est(", child)
        self.assertNotIn("uclamp_eff_value(", child)
        self.assertIn("util_fits_cpu(task_util, util_min, util_max, cpu)", child)
        parent = function(FAIR, "select_idle_sibling")
        self.assertRegex(parent, r"select_idle_capacity\(p, sd, target, task_util,\s*util_min, util_max\)")
        self.assertLess(parent.index("sync_entity_load_avg"),
                        parent.index("select_idle_capacity("))

    def test_idle_capacity_reads_follow_idle_and_fit_checks(self):
        child = function(FAIR, "select_idle_capacity")
        self.assertLess(child.index("if (!available_idle_cpu(cpu)"),
                        child.index("capacity_of(cpu)"))
        self.assertLess(child.index("if (fits > 0)"),
                        child.index("capacity_of(cpu)"))
        self.assertIn("capacity_orig_of(cpu) - thermal_load_avg(cpu_rq(cpu))", child)

    def test_eas_retains_whole_domain_for_energy_accounting(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        allowed = body.index("cpumask_intersects(cpus, p->cpus_ptr)")
        self.assertLess(allowed, body.index("for_each_cpu(cpu, cpus)"))
        self.assertIn("cpumask_intersects(cpus, sched_domain_span(sd))", body)
        self.assertNotIn("cpumask_and(cpus, cpus, p->cpus_ptr)", body)
        self.assertLess(body.index("trace_android_rvh_find_energy_efficient_cpu"), allowed)

    def test_vendor_eas_return_precedes_fallback_clamp_queries(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        self.assertLess(body.index("return new_cpu"), body.index("uclamp_eff_value("))
        self.assertLess(body.index("if (!sd)"), body.index("uclamp_eff_value("))
        self.assertLess(body.index("uclamp_eff_value("), body.index("p_util_min == 0"))

    def test_clamp_change_notifies_only_after_accounting_under_lock(self):
        body = function(CORE, "uclamp_update_active")
        order = ["task_rq_lock", "min_util = uclamp_rq_get",
                 "uclamp_rq_reinc_id", "if (min_util != uclamp_rq_get",
                 "update_rq_clock", "cpufreq_update_util(rq, 0)",
                 "task_rq_unlock"]
        positions = [body.index(term) for term in order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(body.count("cpufreq_update_util("), 1)
        self.assertNotIn("SCHED_CPUFREQ_IOWAIT", body)

    def test_eevdf_snapshot_is_after_single_entity_fast_path(self):
        body = function(FAIR, "pick_eevdf")
        self.assertEqual(body.count("avg_vruntime_snapshot("), 1)
        self.assertLess(body.index("nr_running == 1"),
                        body.index("avg_vruntime_snapshot("))
        self.assertLess(body.index("avg_vruntime_snapshot("),
                        body.index("curr = NULL"))
        self.assertNotIn("entity_eligible(", body)
        self.assertNotIn("vruntime_eligible(", body)
        self.assertIn("sched_feat(RUN_TO_PARITY)", body)
        self.assertIn("curr->vlag == curr->deadline", body)
        self.assertIn("entity_before(curr, best)", body)

    def test_eligibility_retains_division_free_weighted_comparison(self):
        body = function(FAIR, "vruntime_eligible_snapshot")
        self.assertIn("avg >= (s64)(vruntime - cfs_rq->min_vruntime) * load", body)
        self.assertNotIn("div_", body)
        snapshot = function(FAIR, "avg_vruntime_snapshot")
        self.assertIn("curr && curr->on_rq", snapshot)
        self.assertIn("entity_key(cfs_rq, curr) * weight", snapshot)

    def test_eas_domain_pruning_has_no_false_negative(self):
        # Rodin has 8 CPUs. Exhaust every online-domain/allowed intersection;
        # a runnable candidate must never be removed by the new early guard.
        for online in range(256):
            for allowed in range(256):
                for span in (0xFF, 0x0F, 0x70, 0x80):
                    skip = not (online & allowed) or not (online & span)
                    if online & allowed & span:
                        self.assertFalse(skip)


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef int64_t s64;
struct sched_entity;
struct rb_node {
    struct rb_node *rb_left, *rb_right;
    struct sched_entity *owner;
};
struct sched_entity {
    u64 vruntime, deadline, min_vruntime;
    s64 vlag;
    int on_rq, id;
    struct { unsigned long weight; } load;
    struct rb_node node;
};
struct cfs_rq {
    struct sched_entity *curr;
    s64 avg_vruntime;
    long avg_load;
    u64 min_vruntime;
    int nr_running;
    struct { struct { struct rb_node *rb_node; } rb_root; } tasks_timeline;
};
static int parity;
#define RUN_TO_PARITY 1
#define sched_feat(x) (parity)
#define scale_load_down(x) ((x) >> 10)
#define div_s64(a, b) ((a) / (b))
#define printk_deferred(...) ((void)0)
#define __node_2_se(node) ((node)->owner)
static struct sched_entity *__pick_first_entity(struct cfs_rq *rq)
{
    struct rb_node *node = rq->tasks_timeline.rb_root.rb_node;
    if (!node) return NULL;
    while (node->rb_left) node = node->rb_left;
    return node->owner;
}
/* EXTRACTED */
static u64 random_state = 8400;
static u64 random_u64(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}
static struct rb_node *make_tree(struct sched_entity **items, int lo, int hi)
{
    if (lo >= hi) return NULL;
    int mid = (lo + hi) / 2;
    struct sched_entity *se = items[mid];
    se->node.owner = se;
    se->node.rb_left = make_tree(items, lo, mid);
    se->node.rb_right = make_tree(items, mid + 1, hi);
    se->min_vruntime = se->vruntime;
    struct rb_node *children[] = {se->node.rb_left, se->node.rb_right};
    for (int i = 0; i < 2; i++)
        if (children[i] && (s64)(children[i]->owner->min_vruntime - se->min_vruntime) < 0)
            se->min_vruntime = children[i]->owner->min_vruntime;
    return &se->node;
}
int main(void)
{
    struct sched_entity entities[32], *sorted[31];
    for (int trial = 0; trial < 12000; trial++) {
        struct cfs_rq rq = {0};
        memset(entities, 0, sizeof(entities));
        rq.min_vruntime = random_u64(); /* Includes 64-bit wrap boundaries. */
        int n = random_u64() % 31;
        int current = random_u64() % 3; /* absent, queued, or dequeued current */
        parity = random_u64() & 1;
        for (int i = 0; i <= n; i++) {
            struct sched_entity *se = &entities[i];
            se->vruntime = rq.min_vruntime + (s64)(random_u64() % 2000001) - 1000000;
            se->deadline = rq.min_vruntime + (random_u64() % 5) * 10000;
            se->vlag = (random_u64() & 1) ? (s64)se->deadline : 0;
            se->load.weight = (1 + random_u64() % 88761) << 10;
            se->id = i;
            se->on_rq = i < n || current == 1;
            if (i < n) {
                rq.avg_vruntime += entity_key(&rq, se) * scale_load_down(se->load.weight);
                rq.avg_load += scale_load_down(se->load.weight);
                sorted[i] = se;
            }
        }
        rq.curr = current ? &entities[n] : NULL;
        rq.nr_running = n + (current == 1);
        for (int i = 1; i < n; i++) {
            struct sched_entity *se = sorted[i];
            int j = i;
            while (j && entity_before(se, sorted[j - 1])) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = se;
        }
        rq.tasks_timeline.rb_root.rb_node = make_tree(sorted, 0, n);
        printf("%llu", (unsigned long long)avg_vruntime(&rq));
        for (int i = 0; i <= n; i++) printf(" %d", entity_eligible(&rq, &entities[i]));
        for (int protect = 0; protect < 2; protect++) {
            struct sched_entity *winner = pick_eevdf(&rq, protect);
            printf(" %d", winner ? winner->id : -1);
        }
        putchar('\n');
    }
    return 0;
}
"""


def extracted_c(source):
    declarations = [
        ("static bool entity_before(const struct sched_entity *a, const struct sched_entity *b)", "entity_before"),
        ("static s64 entity_key(struct cfs_rq *cfs_rq, struct sched_entity *se)", "entity_key"),
    ]
    if "static void avg_vruntime_snapshot(" in source:
        declarations.append(("static void avg_vruntime_snapshot(struct cfs_rq *cfs_rq, s64 *avg, long *load)", "avg_vruntime_snapshot"))
    declarations.append(("static u64 avg_vruntime(struct cfs_rq *cfs_rq)", "avg_vruntime"))
    if "static inline int vruntime_eligible_snapshot(" in source:
        declarations.append(("static int vruntime_eligible_snapshot(struct cfs_rq *cfs_rq, u64 vruntime, s64 avg, long load)", "vruntime_eligible_snapshot"))
    declarations += [
        ("static int vruntime_eligible(struct cfs_rq *cfs_rq, u64 vruntime)", "vruntime_eligible"),
        ("static int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)", "entity_eligible"),
        ("static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq, bool protect_slice)", "pick_eevdf"),
    ]
    functions = "\n".join(decl + " {\n" + function(source, name) + "\n}"
                          for decl, name in declarations)
    return HARNESS.replace("/* EXTRACTED */", functions)


class ExtractedEEVDFAgreement(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
    def test_actual_c_helpers_and_picker_match_baseline(self):
        baseline = subprocess.check_output(
            ["git", "show", f"{BASELINE}:kernel/sched/fair.c"], cwd=ROOT, text=True)
        results = []
        with tempfile.TemporaryDirectory(prefix="evonix-eevdf-") as directory:
            for label, source in (("baseline", baseline), ("candidate", FAIR)):
                executable = Path(directory) / label
                compiled = subprocess.run(
                    ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                     "-fno-strict-overflow", "-x", "c", "-", "-o", str(executable)],
                    input=extracted_c(source), text=True, capture_output=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                results.append(subprocess.check_output([str(executable)], text=True))
        self.assertEqual(len(results[0].splitlines()), 12000)
        self.assertEqual(results[0], results[1])


IDLE_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
struct cpumask { unsigned bits; };
struct sched_domain { struct cpumask mask; };
struct task_struct { struct cpumask *cpus_ptr; };
static struct cpumask scratch;
static int idle[8], fit[8], reads;
static unsigned long cap[8], original[8], thermal[8];
#define UCLAMP_MIN 0
#define UCLAMP_MAX 1
#define select_rq_mask 0
#define this_cpu_cpumask_var_ptr(x) (&scratch)
#define sched_domain_span(sd) (&(sd)->mask)
#define cpumask_and(d,a,b) ((d)->bits = (a)->bits & (b)->bits)
#define for_each_cpu_wrap(cpu, mask, start) \
    for (int index = 0; index < 8 && ((cpu = ((start) + index) & 7), 1); index++) \
        if ((mask)->bits & (1U << cpu))
#define task_util_est(p) ((void)(p), 123UL)
#define uclamp_eff_value(p,id) ((void)(p), (id) ? 1024UL : 0UL)
#define available_idle_cpu(cpu) idle[cpu]
#define sched_idle_cpu(cpu) 0
#define util_fits_cpu(u,lo,hi,cpu) ((void)(u), (void)(lo), (void)(hi), fit[cpu])
#define cpu_rq(cpu) (cpu)
#define capacity_orig_of(cpu) original[cpu]
#define thermal_load_avg(cpu) thermal[cpu]
static unsigned long capacity_of(int cpu) { reads++; return cap[cpu]; }
/* EXTRACTED */
static uint64_t state = 6899;
static uint64_t next(void)
{
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}
int main(void)
{
    for (int trial = 0; trial < 12000; trial++) {
        struct sched_domain sd = {.mask.bits = next() & 255};
        struct cpumask allowed = {.bits = next() & 255};
        struct task_struct p = {.cpus_ptr = &allowed};
        int start = next() & 7;
        for (int cpu = 0; cpu < 8; cpu++) {
            idle[cpu] = next() & 1;
            fit[cpu] = next() % 3 - 1;
            original[cpu] = cpu < 4 ? 512 : cpu < 7 ? 768 : 1024;
            thermal[cpu] = next() % (original[cpu] + 1);
            cap[cpu] = original[cpu] - thermal[cpu];
        }
        reads = 0;
        int result = /* CALL */;
        printf("%d %d\n", result, reads);
    }
    return 0;
}
"""


class ExtractedIdleAgreement(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
    def test_actual_idle_scan_matches_baseline_with_fewer_capacity_reads(self):
        baseline = subprocess.check_output(
            ["git", "show", f"{BASELINE}:kernel/sched/fair.c"], cwd=ROOT, text=True)
        results = []
        with tempfile.TemporaryDirectory(prefix="evonix-idle-") as directory:
            for label, source in (("baseline", baseline), ("candidate", FAIR)):
                declaration = "static int select_idle_capacity(struct task_struct *p, struct sched_domain *sd, int target"
                call = "select_idle_capacity(&p, &sd, start"
                if label == "candidate":
                    declaration += ", unsigned long task_util, unsigned long util_min, unsigned long util_max"
                    call += ", 123, 0, 1024"
                declaration += ") {\n" + function(source, "select_idle_capacity") + "\n}"
                call += ")"
                c_source = IDLE_HARNESS.replace("/* EXTRACTED */", declaration).replace("/* CALL */", call)
                executable = Path(directory) / label
                compiled = subprocess.run(
                    ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-x", "c", "-", "-o", str(executable)], input=c_source,
                    text=True, capture_output=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                results.append([tuple(map(int, line.split())) for line in
                                subprocess.check_output([str(executable)], text=True).splitlines()])
        self.assertEqual(len(results[0]), 12000)
        self.assertEqual([winner for winner, _ in results[0]],
                         [winner for winner, _ in results[1]])
        self.assertTrue(all(new[1] <= old[1] for old, new in zip(*results)))
        self.assertLess(sum(reads for _, reads in results[1]),
                        sum(reads for _, reads in results[0]))


CLAMP_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
enum uclamp_id { UCLAMP_MIN, UCLAMP_MAX, UCLAMP_CNT };
struct rq_flags { int unused; };
struct rq { unsigned int value[2]; bool locked, clock_updated; };
struct task_struct { bool active; unsigned int effective[2]; };
static struct rq runqueue;
static int notifications;
#define for_each_clamp_id(id) for ((id) = UCLAMP_MIN; (id) < UCLAMP_CNT; (id)++)
static struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
{
    (void)p; (void)rf;
    assert(!runqueue.locked);
    runqueue.locked = true;
    return &runqueue;
}
static unsigned int uclamp_rq_get(struct rq *rq, enum uclamp_id id)
{
    assert(rq->locked);
    return rq->value[id];
}
static void uclamp_rq_reinc_id(struct rq *rq, struct task_struct *p, enum uclamp_id id)
{
    assert(rq->locked);
    if (p->active) rq->value[id] = p->effective[id];
}
static void update_rq_clock(struct rq *rq)
{
    assert(rq->locked);
    rq->clock_updated = true;
}
static void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
    assert(rq->locked && rq->clock_updated && flags == 0);
    notifications++;
}
static void task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
{
    (void)p; (void)rf;
    assert(rq->locked);
    rq->locked = false;
}
/* EXTRACTED */
int main(void)
{
    /* Inactive, unchanged, min-only, max-only, both, unchanged aggregate. */
    struct task_struct cases[] = {
        {false, {400, 512}}, {true, {0, 1024}}, {true, {400, 1024}},
        {true, {0, 512}}, {true, {400, 512}}, {true, {0, 1024}},
    };
    int expected[] = {0, 0, 1, 1, 1, 0};
    for (int i = 0; i < 6; i++) {
        runqueue = (struct rq){.value = {0, 1024}};
        notifications = 0;
        uclamp_update_active(&cases[i]);
        assert(notifications == expected[i]);
        assert(!runqueue.locked);
        assert(runqueue.clock_updated == (bool)expected[i]);
    }
    return 0;
}
"""


class ExtractedClampNotification(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
    def test_callback_conditions_clock_lock_and_flags(self):
        declaration = ("static void uclamp_update_active(struct task_struct *p) {\n"
                       + function(CORE, "uclamp_update_active") + "\n}")
        with tempfile.TemporaryDirectory(prefix="evonix-clamp-") as directory:
            executable = Path(directory) / "clamp"
            compiled = subprocess.run(
                ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", str(executable)],
                input=CLAMP_HARNESS.replace("/* EXTRACTED */", declaration),
                text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
