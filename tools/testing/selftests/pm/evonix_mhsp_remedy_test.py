#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline remedy-history regression; no firmware or cpuidle execution."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0013-mtk-mhsp-handle-wfi-only-prediction-history.patch"


def off_prediction(total, matches, residency, fixed=True):
    if fixed and total == 0:
        return 0
    ratio = matches * 100 // total
    return residency - 1 if ratio <= 60 else 0


class MHSPRemedy(unittest.TestCase):
    def test_full_wfi_only_history_reaches_zero_denominator(self):
        residency = 1000
        history = [(0, 2 * residency)] * 32  # state, measured microseconds
        total = sum(state > 0 for state, _ in history)
        matches = sum(state > 0 and duration >= residency for state, duration in history)
        short_run = 0
        for _, duration in history:
            short_run = short_run + 1 if duration < residency else 0
        self.assertEqual((len(history), short_run, total, matches), (32, 0, 0, 0))
        with self.assertRaises(ZeroDivisionError):
            off_prediction(total, matches, residency, fixed=False)
        self.assertEqual(off_prediction(total, matches, residency), 0)

    def test_valid_history_results_are_unchanged(self):
        for total in range(1, 33):
            for matches in range(total + 1):
                self.assertEqual(off_prediction(total, matches, 1000),
                                 off_prediction(total, matches, 1000, fixed=False))

    def test_no_sample_does_not_force_a_deeper_state(self):
        for residency in (100, 1000, 10000):
            self.assertEqual(off_prediction(0, 0, residency), 0)

    def test_guard_precedes_existing_division(self):
        patch = PATCH.read_text()
        self.assertLess(patch.index("+\t\tif (!gov->remedy.total_cnt)"),
                        patch.index(" \t\tthreshold = div_u64("))
        added = [line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")]
        self.assertEqual(added, ["\t\tif (!gov->remedy.total_cnt)", "\t\t\treturn 0;", ""])


if __name__ == "__main__":
    unittest.main(verbosity=2)
