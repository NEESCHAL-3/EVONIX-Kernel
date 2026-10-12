#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline retry timing model for the unapplied MediaTek LPM candidate."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0002-mtk-lpm-avoid-retention-probe-catch-up.patch"


def next_probe(now, interval, bits):
    return (now + interval) % (1 << bits)


def time_after(a, b, bits):
    diff = (b - a) % (1 << bits)
    return diff >= (1 << (bits - 1))


class RetentionRetryRegression(unittest.TestCase):
    def test_delayed_callback_does_not_rearm_in_the_past(self):
        old_expiry, now, interval = 500, 10000, 500
        self.assertLess(old_expiry + interval, now)
        self.assertEqual(next_probe(now, interval, 64), 10500)

    def test_wraparound_keeps_next_attempt_in_the_future(self):
        for bits in (32, 64):
            for now in (0, 100, (1 << bits) - 100):
                for interval in (1, 5, 500, 1000):
                    self.assertTrue(time_after(next_probe(now, interval, bits), now, bits))

    def test_on_time_probe_retains_the_original_interval(self):
        now, interval = 500, 500
        self.assertEqual(next_probe(now, interval, 64), now + interval)

    def test_patch_changes_only_both_retry_deadlines(self):
        lines = PATCH.read_text().splitlines()
        added = [line for line in lines if line.startswith("+") and not line.startswith("+++")]
        removed = [line for line in lines if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(len(added), 2)
        self.assertEqual(len(removed), 2)
        self.assertTrue(all("expires = jiffies + cpu_ret_check_interval;" in line for line in added))
        self.assertTrue(all("expires +=" in line for line in removed))
        for token in ("crtd->ret_ready = 1", "del_timer(&crtd->ret_timer)",
                      "add_timer_on(&crtd->ret_timer, crtd->cpu)",
                      "add_timer_on(&crtd->ret_timer, cur_cpu)"):
            self.assertIn(token, PATCH.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
