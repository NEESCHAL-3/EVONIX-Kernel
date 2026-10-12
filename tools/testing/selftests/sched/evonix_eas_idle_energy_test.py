#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Idle-energy fast-path models/source contracts; not compiled kernel tests."""

import random
import re
import subprocess
import unittest

from evonix_policy_test import ROOT, function as extract_function


BEFORE = "7395d2daf2ef1aac738c561a77b55276af751d1d"
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
EM = (ROOT / "include/linux/energy_model.h").read_text()


def function(source, name):
    # Both names also occur in prose comments before their definitions.
    signature = re.search(r"(?m)^(?:static inline unsigned long )?" + name + r"\(",
                          source)
    if not signature:
        raise AssertionError(f"Missing definition: {name}")
    return extract_function(source[signature.start():], name)


def energy(pd_busy, task_busy, pd_cap, dst, max_util, allowed, states, fast):
    busy = min(pd_cap, pd_busy + task_busy) if dst >= 0 else pd_busy
    if fast and not busy:
        return 0, 0
    # Model the scan being skipped, not the utilization implementation itself.
    scans = 1
    if not busy:
        return 0, scans
    util = min(max_util, allowed)
    cost = next((cost for perf, cost in states if perf >= util), states[-1][1])
    return cost * busy, scans


class IdleEnergy(unittest.TestCase):
    def test_zero_energy_short_circuits_before_utilization_scan(self):
        body = function(FAIR, "compute_energy")
        self.assertLess(body.index("busy_time + eenv->task_busy_time"),
                        body.index("if (!busy_time)"))
        self.assertLess(body.index("if (!busy_time)"),
                        body.index("eenv_pd_max_util("))
        em = function(EM, "em_cpu_energy")
        self.assertLess(em.index("if (!sum_util)"),
                        em.index("rcu_dereference(pd->em_table)"))

    def test_only_compute_energy_is_changed_from_recorded_source(self):
        before = subprocess.check_output(
            ["git", "show", f"{BEFORE}:kernel/sched/fair.c"], cwd=ROOT, text=True)
        old = function(before, "compute_energy")
        new = function(FAIR, "compute_energy")
        expected = old.replace(
            "\tunsigned long max_util = eenv_pd_max_util(eenv, pd_cpus, p, dst_cpu);\n",
            "", 1).replace(
            "\tunsigned long busy_time = eenv->pd_busy_time;\n",
            "\tunsigned long busy_time = eenv->pd_busy_time;\n"
            "\tunsigned long max_util;\n", 1).replace(
            "\treturn em_cpu_energy(",
            "\t/* The EM returns zero for an idle domain, irrespective of its OPP. */\n"
            "\tif (!busy_time)\n\t\treturn 0;\n\n"
            "\tmax_util = eenv_pd_max_util(eenv, pd_cpus, p, dst_cpu);\n\n"
            "\treturn em_cpu_energy(", 1)
        self.assertEqual(new, expected)

    def test_light_load_skips_only_empty_domain_calculations(self):
        states = [(256, 12), (512, 19), (1024, 37)]
        for dst, task_busy, expected_scans in [(-1, 128, 0), (0, 128, 1),
                                                (0, 0, 0)]:
            result, scans = energy(0, task_busy, 4096, dst, 512, 1024, states, True)
            old, _ = energy(0, task_busy, 4096, dst, 512, 1024, states, False)
            self.assertEqual(result, old)
            self.assertEqual(scans, expected_scans)
        self.assertEqual(energy(128, 0, 4096, -1, 512, 1024, states, True)[1], 1)

    def test_capacity_clamps_and_busy_work_retain_energy_result(self):
        rng = random.Random(6899)
        # Synthetic domains exercise 4+3+1 topology; not Rodin power calibration.
        for nr_cpus in (4, 3, 1):
            for _ in range(4096):
                cap = rng.randrange(1025)
                pd_cap = nr_cpus * cap
                pd_busy = rng.randrange(pd_cap + 1)
                task_busy = rng.randrange(1025)
                dst = rng.choice((-1, 0))
                states = [(256, 12), (512, 19), (1024, 37)]
                args = (pd_busy, task_busy, pd_cap, dst,
                        rng.randrange(2049), cap, states)
                self.assertEqual(energy(*args, True)[0], energy(*args, False)[0])


if __name__ == "__main__":
    unittest.main()
