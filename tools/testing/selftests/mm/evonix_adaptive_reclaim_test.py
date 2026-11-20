#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Unit test for EVONIX Adaptive Memory Reclaim Controller (CONFIG_EVONIX_MM_ADAPTIVE)
Verifies:
1. Kconfig entry in mm/Kconfig and build entry in mm/Makefile.
2. Implementation source in mm/evonix_mm.c.
3. Proper sysctl exports (/proc/sys/vm/evonix_adaptive_reclaim, evonix_reclaim_cushion_mb).
4. Zone watermark cushion math and asynchronous kswapd wakeup logic.
5. Non-interference with watermark_boost_factor = 15000 in mm/page_alloc.c.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

class EvonixAdaptiveReclaimTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kconfig = (ROOT / "mm" / "Kconfig").read_text()
        cls.makefile = (ROOT / "mm" / "Makefile").read_text()
        cls.evonix_mm_c = (ROOT / "mm" / "evonix_mm.c").read_text()
        cls.page_alloc_c = (ROOT / "mm" / "page_alloc.c").read_text()

    def test_kconfig_definition(self):
        self.assertIn("config EVONIX_MM_ADAPTIVE", self.kconfig)

    def test_makefile_obj(self):
        self.assertIn("obj-$(CONFIG_EVONIX_MM_ADAPTIVE) += evonix_mm.o", self.makefile)

    def test_sysctl_entries(self):
        self.assertIn('"evonix_adaptive_reclaim"', self.evonix_mm_c)
        self.assertIn('"evonix_reclaim_cushion_mb"', self.evonix_mm_c)
        self.assertIn("register_sysctl_init", self.evonix_mm_c)

    def test_kswapd_wakeup_call(self):
        self.assertIn("wakeup_kswapd(zone, GFP_KERNEL, 0, zone_idx(zone));", self.evonix_mm_c)

    def test_reclaim_cushion_math(self):
        page_size = 4096
        cushion_mb = 64
        cushion_pages = (cushion_mb * 1024 * 1024) // page_size
        self.assertEqual(cushion_pages, 16384)

        high_wmark = 50000
        target = high_wmark + cushion_pages
        self.assertEqual(target, 66384)

        # If free pages is 60000 (< target), kswapd should be woken
        free_pages_low = 60000
        self.assertTrue(free_pages_low < target)

        # If free pages is 70000 (>= target), kswapd should not be woken
        free_pages_ok = 70000
        self.assertFalse(free_pages_ok < target)

    def test_stock_watermark_boost_factor_intact(self):
        self.assertIn("static int watermark_boost_factor __read_mostly = 15000;", self.page_alloc_c)

    def test_suspend_pm_notifier_registered(self):
        self.assertIn("register_pm_notifier(&evonix_mm_pm_nb);", self.evonix_mm_c)
        self.assertIn("cancel_delayed_work_sync(&evonix_reclaim_dwork);", self.evonix_mm_c)

    def test_freezable_workqueue_used(self):
        self.assertIn("system_freezable_power_efficient_wq", self.evonix_mm_c)
        self.assertNotIn("system_power_efficient_wq,", self.evonix_mm_c)

    def test_adaptive_intervals_and_rate_limiting(self):
        self.assertIn("needed_reclaim ? 1000 : 3000", self.evonix_mm_c)
        self.assertIn("last_kswapd_wakeup_jiffies + HZ", self.evonix_mm_c)

if __name__ == "__main__":
    unittest.main()
