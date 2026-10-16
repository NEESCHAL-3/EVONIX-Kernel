#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""NOHZ tick-path source contracts; not runtime RCU or CPU-hotplug tests."""

import re
import subprocess
import unittest

from evonix_policy_test import ROOT, function


BEFORE = "2cbe431fae0de47ea5df26d60e89681946f6a9ab"
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
CORE = (ROOT / "kernel/sched/core.c").read_text()


class NoHzRcu(unittest.TestCase):
    def test_tick_is_the_only_load_balance_trigger_caller(self):
        callers = []
        for path in (ROOT / "kernel/sched").glob("*.c"):
            for match in re.finditer(r"\btrigger_load_balance\(rq\);", path.read_text()):
                callers.append(path.name)
        self.assertEqual(callers, ["core.c"])
        self.assertIn("trigger_load_balance(rq);", function(CORE, "scheduler_tick"))
        self.assertIn("nohz_balancer_kick(rq);", function(FAIR, "trigger_load_balance"))

    def test_irq_context_is_asserted_and_rcu_accesses_are_checked(self):
        body = function(FAIR, "nohz_balancer_kick")
        self.assertIn("lockdep_assert_irqs_disabled();", body)
        self.assertEqual(body.count("rcu_dereference_sched("), 4)
        self.assertNotIn("rcu_read_lock();", body)
        self.assertNotIn("rcu_read_unlock();", body)
        self.assertNotIn("goto unlock;", body)
        header = (ROOT / "include/linux/rcupdate.h").read_text()
        self.assertIn("rcu_read_lock_sched_held()", header)
        update = function((ROOT / "kernel/rcu/update.c").read_text(),
                          "rcu_read_lock_sched_held")
        self.assertIn("!preemptible()", update)

    def test_balance_and_vendor_decisions_are_unchanged(self):
        before = subprocess.check_output(
            ["git", "show", f"{BEFORE}:kernel/sched/fair.c"], cwd=ROOT, text=True)
        old = function(before, "nohz_balancer_kick")
        new = function(FAIR, "nohz_balancer_kick")

        def normalize(body):
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            for line in ("rcu_read_lock();", "rcu_read_unlock();",
                         "lockdep_assert_irqs_disabled();", "unlock:"):
                body = body.replace(line, "")
            body = body.replace("rcu_dereference_sched(", "rcu_dereference(")
            body = body.replace("goto unlock;", "goto out;")
            # Request accumulation is independently checked by the flags suite.
            body = body.replace("flags |= NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;",
                                "flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;")
            return re.sub(r"\s+", "", body)

        self.assertEqual(normalize(new), normalize(old))
        # Keep the separate hotplug/idle helper locking outside this backport.
        for name in ("set_cpu_sd_state_busy", "set_cpu_sd_state_idle",
                     "nohz_balance_exit_idle", "nohz_balance_enter_idle"):
            self.assertEqual(function(FAIR, name), function(before, name))


if __name__ == "__main__":
    unittest.main()
