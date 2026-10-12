#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Guard audited scheduler/DVFS/idle behavior; not runtime verification."""

from pathlib import Path
import re
import unittest

from evonix_policy_test import function as extract_function

ROOT = Path(__file__).resolve().parents[4]
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
IDLE = (ROOT / "drivers/cpuidle/cpuidle.c").read_text()


def function(source, name):
    # Discard comments so cross-references cannot be mistaken for definitions.
    return extract_function(re.sub(r"/\*.*?\*/", "", source, flags=re.S), name)


class SchedulerSafety(unittest.TestCase):
    def test_estimator_keeps_fast_up_and_contention_protection(self):
        body = function(FAIR, "util_est_update")
        self.assertIn("if (ewma <= dequeued)", body)
        self.assertIn("ewma = dequeued;", body)
        self.assertIn("(dequeued + UTIL_EST_MARGIN) < task_runnable(p)", body)
        self.assertLess(body.index("trace_android_rvh_util_est_update"),
                        body.index("if (!task_sleep)"))
        self.assertLess(body.index("if (!task_sleep)"), body.index("ewma = READ_ONCE"))

    def test_estimate_is_added_before_frequency_callbacks(self):
        body = function(FAIR, "enqueue_task_fair")
        self.assertLess(body.index("util_est_enqueue(&rq->cfs, p);"),
                        body.index("cpufreq_update_util("))
        self.assertLess(body.index("util_est_enqueue(&rq->cfs, p);"),
                        body.index("enqueue_entity("))
        self.assertLess(body.index("trace_android_rvh_set_iowait"),
                        body.index("cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT)"))

    def test_cache_affinity_and_cookie_safety_are_preserved(self):
        body = function(FAIR, "task_hot")
        self.assertIn("sched_feat(CACHE_HOT_BUDDY)", body)
        self.assertIn("sched_core_cookie_match", body)
        self.assertIn("sysctl_sched_migration_cost == -1", body)
        self.assertIn("sysctl_sched_migration_cost == 0", body)
        self.assertIn("sysctl_sched_migration_cost\t= 500000UL;", FAIR)

    def test_eas_keeps_thermal_capacity_affinity_and_prev_cpu(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        for token in ("cpu_thermal_cap -= arch_scale_thermal_pressure(cpu);",
                      "cpumask_test_cpu(cpu, p->cpus_ptr)", "cpu == prev_cpu",
                      "fits = util_fits_cpu(util, util_min, util_max, cpu);"):
            self.assertIn(token, body)
        self.assertIn("em_cpu_energy(pd->em_pd", function(FAIR, "compute_energy"))

    def test_idle_governor_and_exit_latencies_are_not_replaced(self):
        self.assertIn("return cpuidle_curr_governor->select(drv, dev, stop_tick);",
                      function(IDLE, "cpuidle_select"))
        body = function(IDLE, "find_deepest_state")
        self.assertIn("s->exit_latency_ns > max_latency_ns", body)
        self.assertIn("dev->states_usage[i].disable", body)
        self.assertIn("s->flags & forbidden_flags", body)

    def test_lag_bound_keeps_weight_scaling_and_tick_floor(self):
        body = function(FAIR, "entity_lag")
        self.assertIn("calc_delta_fair(max_t(u64, 2*se->slice, TICK_NSEC), se)", body)
        self.assertIn("return clamp(vlag, -limit, limit);", body)
        self.assertIn("entity_lag(avruntime, se)", function(FAIR, "reweight_eevdf"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
