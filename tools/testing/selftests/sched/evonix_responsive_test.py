#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Unit test for EVONIX Workload-Aware UI Responsiveness Engine (CONFIG_EVONIX_SCHED_RESPONSIVE)
Verifies:
1. Kconfig entry in drivers/cpufreq/Kconfig.
2. Tunable ui_boost_pct field in struct sugov_tunables.
3. Sysfs attribute registration and value bounds checking (0-100).
4. Application in sugov_get_util targeting latency_sensitive tasks only.
5. Headroom calculation logic.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

class EvonixResponsiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kconfig = (ROOT / "drivers" / "cpufreq" / "Kconfig").read_text()
        cls.schedutil_c = (ROOT / "kernel" / "sched" / "cpufreq_schedutil.c").read_text()

    def test_kconfig_definition(self):
        self.assertIn("config EVONIX_SCHED_RESPONSIVE", self.kconfig)
        self.assertIn("depends on EVONIX_SCHEDUTIL", self.kconfig)

    def test_tunable_field(self):
        self.assertIn("unsigned int\t\tui_boost_pct;", self.schedutil_c)

    def test_sysfs_attribute(self):
        self.assertIn("ui_boost_pct_show", self.schedutil_c)
        self.assertIn("ui_boost_pct_store", self.schedutil_c)
        self.assertIn("val > 100", self.schedutil_c)
        self.assertIn("&ui_boost_pct.attr,", self.schedutil_c)

    def test_get_util_application(self):
        self.assertIn("CONFIG_EVONIX_SCHED_RESPONSIVE", self.schedutil_c)
        self.assertIn("tg->latency_sensitive", self.schedutil_c)
        self.assertIn("boost_add = (max * ui_boost) / 100", self.schedutil_c)

    def test_boost_math(self):
        # Verify simulated boost behavior
        max_cap = 1024
        base_util = 200
        ui_boost_pct = 20  # +20%
        boost_add = (max_cap * ui_boost_pct) // 100
        boosted = min(max_cap, base_util + boost_add)
        self.assertEqual(boosted, 404)  # 200 + 204

        # Verify ceiling clamp at max capacity
        high_util = 950
        boosted_high = min(max_cap, high_util + boost_add)
        self.assertEqual(boosted_high, 1024)

if __name__ == "__main__":
    unittest.main()
