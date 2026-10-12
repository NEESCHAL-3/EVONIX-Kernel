#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual capacity guard; offline regression, not a boot test."""

from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest

from evonix_policy_test import function

ROOT = Path(__file__).resolve().parents[4]
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
BASE = subprocess.check_output(
    ["git", "show", "e98273ba3fa2b5eb0020c0e8d4b4eb17abec6cd8:kernel/sched/fair.c"], cwd=ROOT, text=True)


def guard(source):
    definition = re.search(r"(?m)^static bool update_sd_pick_busiest\(", source)
    if not definition:
        raise AssertionError("Missing busiest-group definition")
    body = function(source[definition.start():], "update_sd_pick_busiest")
    match = re.search(
        r"if \(\(env->sd->flags & SD_ASYM_CPUCAPACITY\) &&\s*"
        r"\(sgs->group_type <= group_fully_busy\).*?return false;",
        body, re.S)
    if not match:
        raise AssertionError("Missing higher-capacity group guard")
    return match.group()


class QueuedGroupRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-queued-group-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        capacity = re.search(r"^#define capacity_greater.*$", FAIR, re.M).group()
        source = """
#include <stdbool.h>
#include <stdio.h>
#define SD_ASYM_CPUCAPACITY 1
#define group_fully_busy 1
struct domain { unsigned flags; };
struct lb_env { struct domain *sd; int dst_cpu; };
struct sg_lb_stats { unsigned group_type, sum_h_nr_running, group_weight; };
struct capacity { unsigned min_capacity; };
struct sched_group { struct capacity *sgc; };
static unsigned dst_capacity;
static unsigned capacity_of(int cpu) { (void)cpu; return dst_capacity; }
""" + capacity + "\n"
        for name, data in (("baseline", BASE), ("candidate", FAIR)):
            source += (f"static bool {name}(struct lb_env *env, "
                       "struct sched_group *sg, struct sg_lb_stats *sgs) {\n"
                       + guard(data) + "\nreturn true;\n}\n")
        source += """
int main(void) {
    struct domain sd;
    struct lb_env env = { .sd = &sd, .dst_cpu = 0 };
    struct capacity cap;
    struct sched_group sg = { .sgc = &cap };
    struct sg_lb_stats sgs;
    while (scanf("%u %u %u %u %u %u", &sd.flags, &sgs.group_type,
                 &sgs.sum_h_nr_running, &sgs.group_weight,
                 &cap.min_capacity, &dst_capacity) == 6)
        printf("%d %d\\n", baseline(&env, &sg, &sgs),
               candidate(&env, &sg, &sgs));
    return 0;
}
"""
        path = work / "guard.c"
        path.write_text(source)
        cls.executable = work / "guard"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-Werror", str(path), "-o", str(cls.executable)],
                       check=True)

    def evaluate(self, cases):
        result = subprocess.check_output(
            [str(self.executable)], text=True,
            input="".join(" ".join(map(str, case)) + "\n" for case in cases))
        return [tuple(map(int, line.split())) for line in result.splitlines()]

    def test_device_99_task_prime_core_is_not_excluded(self):
        self.assertEqual(self.evaluate([(1, 1, 99, 1, 1020, 633)]), [(0, 1)])

    def test_single_task_stays_on_faster_cpu(self):
        self.assertEqual(self.evaluate([(1, 1, 1, 1, 1020, 633)]), [(0, 0)])

    def test_group_count_boundary(self):
        for weight in range(1, 33):
            cases = [(1, 1, count, weight, 1024, 697)
                     for count in (weight - 1, weight, weight + 1)]
            self.assertEqual(self.evaluate(cases), [(0, 0), (0, 0), (0, 1)])

    def test_actual_c_matches_only_intended_change(self):
        rng = random.Random(6899)
        cases = [(rng.randrange(2), rng.randrange(7), rng.randrange(1001),
                  rng.randrange(1, 33), rng.randrange(1, 1025),
                  rng.randrange(1, 1025)) for _ in range(25000)]
        results = self.evaluate(cases)
        self.assertEqual(len(results), len(cases))
        for case, (old, new) in zip(cases, results):
            asym, kind, tasks, weight, src, dst = case
            reject = asym and kind <= 1 and src * 1024 > dst * 1078
            self.assertEqual(old, int(not reject), case)
            self.assertEqual(new, int(not (reject and tasks <= weight)), case)
            if new != old:
                self.assertGreater(tasks, weight)
                self.assertEqual((old, new), (0, 1))

    def test_eas_and_migration_vetoes_are_unchanged(self):
        for name in ("group_is_overloaded", "find_busiest_group",
                     "find_busiest_queue", "can_migrate_task", "detach_tasks",
                     "sched_balance_newidle", "cpu_overutilized"):
            self.assertEqual(function(BASE, name), function(FAIR, name), name)

    def test_no_request_sizing_or_affinity_change(self):
        for name in ("evonix_entity_slice", "pick_eevdf",
                     "select_task_rq_fair", "task_hot"):
            self.assertEqual(function(BASE, name), function(FAIR, name), name)
        self.assertNotIn("task_of", guard(FAIR))
        self.assertNotIn("comm", guard(FAIR))


if __name__ == "__main__":
    unittest.main(verbosity=2)
