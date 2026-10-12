#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""PREEMPT_SHORT policy models/source contracts; not kernel execution."""

from pathlib import Path
import random
import unittest

from evonix_short_slice_test import function

ROOT = Path(__file__).resolve().parents[4]
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
FEATURES = (ROOT / "kernel/sched/features.h").read_text()


def may_preempt(short, long, wake_eligible, current_eligible, earlier, enabled=True):
    return enabled and short < long and wake_eligible and (earlier or not current_eligible)


def renew_or_yield(expired, protected, eligible, fair_count, enabled=True,
                   deadline_veto=False, resched_veto=False):
    if deadline_veto:
        return False, False
    yield_short = enabled and not protected and not eligible and fair_count > 1
    return expired, (expired or yield_short) and fair_count > 1 and not resched_veto


class PreemptShort(unittest.TestCase):
    def test_only_shorter_eligible_earlier_requests_interrupt_protection(self):
        self.assertTrue(may_preempt(1, 2, True, True, True))
        for args in ((2, 2, True, True, True), (3, 2, True, True, True),
                     (1, 2, False, False, True), (1, 2, True, True, False)):
            self.assertFalse(may_preempt(*args))

    def test_ineligible_current_and_runtime_rollback(self):
        self.assertTrue(may_preempt(1, 2, True, False, False))
        self.assertFalse(may_preempt(1, 2, True, False, False, enabled=False))

    def test_preempted_current_yields_at_negative_lag_without_renewal(self):
        self.assertEqual(renew_or_yield(False, False, False, 2), (False, True))
        self.assertEqual(renew_or_yield(False, True, False, 2), (False, False))
        self.assertEqual(renew_or_yield(False, False, True, 2), (False, False))

    def test_one_fair_entity_does_not_reschedule_because_rt_tasks_exist(self):
        self.assertEqual(renew_or_yield(True, False, False, 1), (True, False))
        body = function(FAIR, "update_deadline")
        self.assertIn("if (cfs_rq->nr_running > 1)", body)
        self.assertNotIn("rq_of(cfs_rq)->nr_running", body)

    def test_both_android_vetoes_are_retained(self):
        for expired in (False, True):
            self.assertEqual(renew_or_yield(expired, False, False, 2,
                                           deadline_veto=True), (False, False))
            self.assertEqual(renew_or_yield(expired, False, False, 2,
                                           resched_veto=True), (expired, False))
        body = function(FAIR, "update_deadline")
        self.assertLess(body.index("if (skip_preempt)"), body.index("did_preempt_short("))
        self.assertLess(body.index("trace_android_rvh_update_deadline"),
                        body.index("expired = (s64)"))
        self.assertLess(body.index("trace_android_vh_resched_curr_lazy"),
                        body.index("resched_curr(rq_of(cfs_rq));"))

    def test_no_protection_or_buddy_mutation_before_winner_and_veto(self):
        body = function(FAIR, "check_preempt_wakeup")
        helper = function(FAIR, "do_preempt_short")
        self.assertNotIn("vlag =", helper)
        self.assertNotIn("clear_buddies", helper)
        self.assertLess(body.index("pick_eevdf(cfs_rq, protect_slice) == pse"),
                        body.index("se->vlag = 0;"))
        tail = body[body.index("preempt:\n"):]
        self.assertLess(tail.index("if (ignore)"), tail.index("clear_buddies(cfs_rq, se);"))
        self.assertLess(tail.index("clear_buddies(cfs_rq, se);"), tail.index("resched_curr(rq);"))

    def test_short_slice_preference_matches_upstream_predicates(self):
        rng = random.Random(6899)
        for _ in range(8192):
            short, long = rng.randrange(1, 4096), rng.randrange(1, 4096)
            wake, current, earlier = (bool(rng.getrandbits(1)) for _ in range(3))
            expected = False
            if short < long and wake:
                expected = earlier or not current
            self.assertEqual(may_preempt(short, long, wake, current, earlier), expected)
        helper = function(FAIR, "do_preempt_short")
        for token in ("pse->slice >= se->slice", "!entity_eligible(cfs_rq, pse)",
                      "entity_before(pse, se) || !entity_eligible(cfs_rq, se)"):
            self.assertIn(token, helper)

    def test_feature_is_appended_and_run_to_parity_remains_enabled(self):
        self.assertIn("SCHED_FEAT(RUN_TO_PARITY, true)", FEATURES)
        self.assertGreater(FEATURES.index("SCHED_FEAT(PREEMPT_SHORT, true)"),
                           FEATURES.index("SCHED_FEAT(EVONIX_LATENCY_SLICE, false)"))
        self.assertIn("SCHED_FEAT(PREEMPT_SHORT, false)", FEATURES)


if __name__ == "__main__":
    unittest.main(verbosity=2)
