#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline governor decision contracts; no hardware frequency changes."""

import itertools
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0010-mtk-sugov-honor-forced-limit-updates.patch"


def accepts(previous, requested, elapsed, up_delay, down_delay, forced=False, vendor=False):
    limited = (requested > previous and elapsed < up_delay or
               requested < previous and elapsed < down_delay)
    if not forced and limited:
        return False
    return forced or vendor or previous != requested


class ForcedDVFS(unittest.TestCase):
    def test_policy_limit_drop_is_not_delayed(self):
        self.assertTrue(accepts(3000000, 1500000, 0, 1000, 10000, forced=True))
        self.assertFalse(accepts(3000000, 1500000, 0, 1000, 10000))

    def test_deadline_increase_is_not_delayed(self):
        self.assertTrue(accepts(1000000, 2000000, 0, 10000, 1000, forced=True))
        self.assertFalse(accepts(1000000, 2000000, 0, 10000, 1000))

    def test_ordinary_direction_limits_and_vendor_refresh_remain(self):
        for previous, requested, elapsed, up, down in itertools.product(
                (300000, 1500000, 3000000), repeat=5):
            expected = (previous != requested and not (
                requested > previous and elapsed < up or
                requested < previous and elapsed < down))
            self.assertEqual(accepts(previous, requested, elapsed, up, down), expected)
        self.assertTrue(accepts(1000, 1000, 0, 500, 500, vendor=True))
        self.assertFalse(accepts(1000, 1000, 0, 500, 500))

    def test_source_bypasses_delay_without_consuming_forced_flag(self):
        patch = PATCH.read_text()
        self.assertIn("+\tif (!sg_policy->need_freq_update &&", patch)
        self.assertIn("+\t    sugov_up_down_rate_limit(sg_policy, time, next_freq))", patch)
        added = [line for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")]
        self.assertEqual(len(added), 2)
        self.assertFalse(any("need_freq_update =" in line for line in added))


if __name__ == "__main__":
    unittest.main(verbosity=2)
