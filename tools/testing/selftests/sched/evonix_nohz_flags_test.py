#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""NOHZ request-bit accumulation models; not compiled balancer tests."""

import subprocess
import unittest

from evonix_policy_test import ROOT, function


BEFORE = "fb659dd35fe3e518ebbe3701deb4bd767396e0a5"
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
BALANCE, STATS, NEXT = 1, 2, 8
REQUEST_MASK = BALANCE | STATS | NEXT


class NoHzFlags(unittest.TestCase):
    def test_generic_balance_retains_pending_next_deadline_request(self):
        requested = NEXT
        old = STATS | BALANCE
        new = requested | STATS | BALANCE
        self.assertFalse(old & NEXT)
        self.assertTrue(new & NEXT)

    def test_all_valid_request_combinations_are_accumulated(self):
        for requested in range(16):
            if requested & ~REQUEST_MASK:
                continue
            result = requested | STATS | BALANCE
            self.assertEqual(result & requested, requested)
            self.assertEqual(result & (STATS | BALANCE), STATS | BALANCE)
        # With no vendor request, ordinary generic behavior is identical.
        for requested in (0, STATS):
            self.assertEqual(requested | STATS | BALANCE, STATS | BALANCE)

    def test_source_changes_only_five_balance_assignments(self):
        before = subprocess.check_output(
            ["git", "show", f"{BEFORE}:kernel/sched/fair.c"], cwd=ROOT, text=True)
        old = function(before, "nohz_balancer_kick")
        new = function(FAIR, "nohz_balancer_kick")
        assignment = "flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;"
        self.assertEqual(old.count(assignment), 5)
        self.assertEqual(new, old.replace(assignment, assignment.replace(" = ", " |= ")))
        self.assertIn("trace_android_rvh_sched_nohz_balancer_kick", new)
        self.assertIn("if (done)\n\t\tgoto out;", new)


if __name__ == "__main__":
    unittest.main()
