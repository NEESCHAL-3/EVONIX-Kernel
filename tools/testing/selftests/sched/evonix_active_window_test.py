#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline adaptive-margin arithmetic contracts, not a vendor-module run."""

from pathlib import Path
import random
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0008-mtk-sugov-preserve-active-window-precision.patch"


def ratio(duration, idle, previous=0, fixed=True):
    if fixed and duration == 0:
        return previous
    active = duration - min(idle, duration)
    divisor = duration if fixed else duration & 0xFFFFFFFF
    return (active << 10) // divisor


class ActiveWindow(unittest.TestCase):
    def test_zero_duration_keeps_previous_ratio(self):
        for previous in (0, 128, 512, 1024):
            self.assertEqual(ratio(0, 0, previous), previous)
        with self.assertRaises(ZeroDivisionError):
            ratio(0, 0, fixed=False)

    def test_normal_windows_are_unchanged(self):
        rng = random.Random(8400)
        for _ in range(4096):
            duration = rng.randint(1, 0xFFFFFFFF)
            idle = rng.randint(0, duration * 2)
            self.assertEqual(ratio(duration, idle), ratio(duration, idle, fixed=False))

    def test_long_window_keeps_full_divisor(self):
        duration = (1 << 32) + 1_000_000
        self.assertEqual(ratio(duration, duration // 2), 512)
        self.assertGreater(ratio(duration, duration // 2, fixed=False), 1024)
        self.assertEqual(ratio(1 << 32, 0), 1024)
        with self.assertRaises(ZeroDivisionError):
            ratio(1 << 32, 0, fixed=False)

    def test_idle_windows_stay_zero_and_ratios_bounded(self):
        for duration in (1, 1000, 0xFFFFFFFF, 1 << 32, 10**12):
            for idle in (0, duration // 2, duration, duration * 2):
                self.assertTrue(0 <= ratio(duration, idle) <= 1024)
            self.assertEqual(ratio(duration, duration), 0)

    def test_source_does_not_consume_zero_duration_history(self):
        patch = PATCH.read_text()
        self.assertLess(patch.index("+\tif (!duration)"), patch.index(" \tidle_duration ="))
        self.assertIn("+\t\treturn READ_ONCE(cpu_active_ratio_cap[cpu_idx]);", patch)
        self.assertIn("+\treturn div64_u64(active_duration << SCHED_CAPACITY_SHIFT, duration);", patch)
        self.assertIn("+#include <linux/math64.h>", patch)


if __name__ == "__main__":
    unittest.main(verbosity=2)
