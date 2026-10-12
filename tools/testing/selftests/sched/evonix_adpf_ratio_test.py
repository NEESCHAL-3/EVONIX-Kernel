#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline wide-integer model for the unapplied MediaTek ADPF ratio fix."""

from pathlib import Path
import random
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0004-mtk-adpf-preserve-duration-precision.patch"
SCALE = 1024


def ratio(actual, target):
    if actual <= 0 or target <= 0:
        return 0
    if actual >= target:
        return SCALE
    return actual * SCALE // target


class ADPFRatioRegression(unittest.TestCase):
    def test_normal_frame_duration_demonstrates_old_overflow(self):
        frame = 16666667
        old = ((frame * SCALE) & 0xffffffff) // frame
        self.assertEqual(old, 250)
        self.assertEqual(ratio(frame, frame), SCALE)

    def test_under_and_over_budget_are_scaled_and_saturated(self):
        self.assertEqual(ratio(8333333, 16666666), 512)
        self.assertEqual(ratio(25000000, 16666666), SCALE)
        self.assertEqual(ratio(1, (1 << 63) - 1), 0)

    def test_nonpositive_values_do_not_create_unsigned_boosts(self):
        for actual, target in ((0, 10), (-1, 10), (10, 0), (10, -1), (-1, -1)):
            self.assertEqual(ratio(actual, target), 0)

    def test_large_valid_values_preserve_precision(self):
        high = (1 << 63) - 1
        self.assertEqual(ratio(high // 2, high), 511)
        self.assertEqual(ratio(high, high), SCALE)
        self.assertEqual(ratio(5_000_000_000, 10_000_000_000), 512)

    def test_randomized_results_match_exact_saturated_arithmetic(self):
        rng = random.Random(6899)
        for _ in range(4096):
            actual, target = rng.randrange(1, 1 << 63), rng.randrange(1, 1 << 63)
            self.assertEqual(ratio(actual, target), min(SCALE, actual * SCALE // target))

    def test_helper_is_already_in_the_mediatek_symbol_list(self):
        symbols = (ROOT / "android/abi_gki_aarch64_mtk").read_text().splitlines()
        self.assertIn("mul_u64_u64_div_u64", [s.strip() for s in symbols])
        text = PATCH.read_text()
        self.assertIn("+#include <linux/math64.h>", text)
        self.assertIn("+\ts64 durationNanos = 0;", text)
        self.assertIn("adpf_ratio = mul_u64_u64_div_u64", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
