#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline MHSP timer lifetime contracts, not kernel concurrency execution."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0007-mtk-mhsp-cancel-disabled-prediction-timer.patch"


def disable(instances, cpu):
    instances[cpu]["enabled"] = False
    instances[cpu]["timer"] = False
    return any(instance["enabled"] for instance in instances)


class TimerDisable(unittest.TestCase):
    def test_other_enabled_cpu_does_not_skip_timer_cleanup(self):
        instances = [{"enabled": True, "timer": True} for _ in range(8)]
        self.assertTrue(disable(instances, 4))
        self.assertFalse(instances[4]["timer"])
        self.assertTrue(instances[0]["timer"])

    def test_final_disable_cleans_all_timers(self):
        instances = [{"enabled": True, "timer": True} for _ in range(8)]
        for cpu in range(7):
            self.assertTrue(disable(instances, cpu))
        self.assertFalse(disable(instances, 7))
        self.assertFalse(any(instance["timer"] for instance in instances))

    def test_repeated_disable_is_idempotent(self):
        instances = [{"enabled": False, "timer": False}]
        for _ in range(10):
            self.assertFalse(disable(instances, 0))
            self.assertFalse(instances[0]["timer"])

    def test_patch_uses_sync_cancel_before_early_return_loop(self):
        patch = PATCH.read_text()
        self.assertIn("+\thrtimer_cancel(&gov->shallow.histtimer);", patch)
        self.assertLess(patch.index("+\thrtimer_cancel("), patch.index(" \tfor_each_possible_cpu(cpu)"))
        added = [line for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")]
        self.assertEqual(len(added), 1)
        self.assertNotIn("hrtimer_try_to_cancel", added[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
