#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline decision-model test for the MediaTek vendor source change."""

import itertools
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0001-mtk-eas-fix-max-clamp-dvfs-comparison.patch"


def needs_update(cached_min, cached_max, rq_min, rq_max, fixed=True):
    return cached_min != rq_min or cached_max != (rq_max if fixed else rq_min)


class ClampDVFSRegression(unittest.TestCase):
    def test_unchanged_non_equal_clamps_no_longer_request_dvfs(self):
        self.assertTrue(needs_update(0, 1024, 0, 1024, fixed=False))
        self.assertFalse(needs_update(0, 1024, 0, 1024))

    def test_max_only_change_is_not_missed(self):
        self.assertFalse(needs_update(256, 256, 256, 512, fixed=False))
        self.assertTrue(needs_update(256, 256, 256, 512))

    def test_min_and_max_changes_are_independent(self):
        values = [0, 128, 256, 512, 768, 1024]
        for cached_min, cached_max, rq_min, rq_max in itertools.product(values, repeat=4):
            if cached_min > cached_max or rq_min > rq_max:
                continue
            expected = (cached_min, cached_max) != (rq_min, rq_max)
            self.assertEqual(needs_update(cached_min, cached_max, rq_min, rq_max), expected)

    def test_patch_changes_only_the_wrong_comparison(self):
        lines = PATCH.read_text().splitlines()
        removed = [line for line in lines if line.startswith("-") and not line.startswith("---")]
        added = [line for line in lines if line.startswith("+") and not line.startswith("+++")]
        self.assertEqual(len(removed), 1)
        self.assertEqual(len(added), 1)
        self.assertIn("UCLAMP_MIN", removed[0])
        self.assertIn("UCLAMP_MAX", added[0])
        self.assertEqual(removed[0][1:].replace("UCLAMP_MIN", "UCLAMP_MAX"), added[0][1:])


if __name__ == "__main__":
    unittest.main(verbosity=2)
