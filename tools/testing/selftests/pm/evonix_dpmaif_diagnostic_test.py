#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline contracts for a diagnostic-only modem timer, not radio tests."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0011-mtk-dpmaif-defer-traffic-diagnostic-timer.patch"


class DPMAIFDiagnostic(unittest.TestCase):
    def test_only_diagnostic_timer_setup_changes(self):
        lines = PATCH.read_text().splitlines()
        added = [line[1:] for line in lines if line.startswith("+") and not line.startswith("+++")]
        removed = [line[1:] for line in lines if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(removed, ["\ttimer_setup(&dpmaif_ctl->traffic_monitor, dpmaif_traffic_monitor_func, 0);"])
        self.assertEqual(added, ["\ttimer_setup(&dpmaif_ctl->traffic_monitor, dpmaif_traffic_monitor_func,",
                                 "\t\t    TIMER_DEFERRABLE);"])

    def test_diagnostic_deadline_does_not_wake_idle_cpu_in_model(self):
        # (deadline, deferrable): the packet/health deadline is unchanged.
        events = [(10, True), (20, False), (30, False)]
        next_wakeup = min(deadline for deadline, deferred in events if not deferred)
        self.assertEqual(next_wakeup, 20)
        self.assertNotEqual(next_wakeup, events[0][0])
        # When other work wakes the CPU, both overdue callbacks are eligible.
        due = [deadline for deadline, _ in events if deadline <= next_wakeup]
        self.assertEqual(due, [10, 20])

    def test_kernel_flag_is_defined_as_idle_deferral(self):
        header = (ROOT / "include/linux/timer.h").read_text()
        self.assertIn("#define TIMER_DEFERRABLE", header)
        self.assertIn("will not cause a CPU to come out of idle", header)
        self.assertNotIn("TIMER_IRQSAFE", "\n".join(
            line for line in PATCH.read_text().splitlines() if line.startswith("+")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
