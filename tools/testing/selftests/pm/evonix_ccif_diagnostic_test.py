#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline source contracts; do not claim call/SMS/data validation."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0012-mtk-ccif-defer-traffic-diagnostic-timer.patch"


class CCIFDiagnostic(unittest.TestCase):
    def test_only_the_traffic_timer_flag_changes(self):
        patch = PATCH.read_text()
        added = [line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")]
        removed = [line[1:] for line in patch.splitlines() if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(removed, ["\t\tmd_ccif_traffic_monitor_func, 0);"])
        self.assertEqual(added, ["\t\t    md_ccif_traffic_monitor_func, TIMER_DEFERRABLE);"])
        self.assertIn(" \ttimer_setup(&ccif_ctrl->traffic_monitor,", patch)

    def test_queued_diagnostic_work_is_preserved(self):
        patch = PATCH.read_text()
        self.assertIn(" \tINIT_WORK(&ccif_ctrl->traffic_info.traffic_work_struct,", patch)
        self.assertIn(" \t\tmd_ccif_traffic_work_func);", patch)
        self.assertNotIn("del_timer", patch)
        self.assertNotIn("disable_irq", patch)
        self.assertNotIn("ccci_md_force_assert", patch)


if __name__ == "__main__":
    unittest.main(verbosity=2)
