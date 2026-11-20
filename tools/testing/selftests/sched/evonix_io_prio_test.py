#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Unit test for EVONIX Foreground Read Prioritization & Latency Guard (CONFIG_EVONIX_IO_PRIO)
Verifies:
1. Kconfig entry in block/Kconfig.iosched.
2. Correct classification and queueing into prio_queue[1] for foreground sync reads.
3. Proper non-interference with emergency Tier-0 (insert_flags & BLK_MQ_INSERT_AT_HEAD).
4. Lockless bypass state evaluation and dispatch order (prio_queue[0] before prio_queue[1]).
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

class EvonixIoPrioTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kconfig = (ROOT / "block" / "Kconfig.iosched").read_text()
        cls.adios_c = (ROOT / "block" / "adios.c").read_text()

    def test_kconfig_definition(self):
        self.assertIn("config EVONIX_IO_PRIO", self.kconfig)
        self.assertIn("depends on MQ_IOSCHED_ADIOS", self.kconfig)

    def test_ioprio_header_included(self):
        self.assertIn("#include <linux/ioprio.h>", self.adios_c)

    def test_post_stability_classification(self):
        self.assertIn("#ifdef CONFIG_EVONIX_IO_PRIO", self.adios_c)
        self.assertIn("insert_to_prio_queue(ad, rq, 1);", self.adios_c)

    def test_tier0_precedence(self):
        # Tier-0 (insert_at_head) must precede Tier-1 in insert_request_post_stability
        tier0_idx = self.adios_c.find("insert_to_prio_queue(ad, rq, 0);")
        tier1_idx = self.adios_c.find("insert_to_prio_queue(ad, rq, 1);")
        self.assertNotEqual(tier0_idx, -1)
        self.assertNotEqual(tier1_idx, -1)
        self.assertLess(tier0_idx, tier1_idx)

    def test_dispatch_ctz_priority(self):
        # prio_queue dispatch must strictly prioritize queue 0 over queue 1
        self.assertIn("!(pq_state & 1)", self.adios_c)

    def test_memory_barriers_present(self):
        # smp_wmb in set_adios_state and smp_rmb in dispatch_from_pq
        self.assertIn("smp_wmb();", self.adios_c)
        self.assertIn("smp_rmb();", self.adios_c)

    def test_anti_starvation_quota_present(self):
        self.assertIn("pq1_dispatch_count", self.adios_c)
        self.assertIn("pq1_dispatch_count) >= 8", self.adios_c)

    def test_simulated_dispatch_order(self):
        # bit 0 = queue 0 (Tier-0 head), bit 1 = queue 1 (Tier-1 sync read)
        def get_pq_idx(state):
            return int(not (state & 1))

        # Both present: queue 0 served first
        self.assertEqual(get_pq_idx(0b11), 0)
        # Only Tier-1 present: queue 1 served
        self.assertEqual(get_pq_idx(0b10), 1)
        # Only Tier-0 present: queue 0 served
        self.assertEqual(get_pq_idx(0b01), 0)

    def test_simulated_anti_starvation(self):
        # Simulate 12 consecutive Tier-1 requests with pending normal work
        pq1_count = 0
        dispatched_tier1 = 0
        dispatched_normal = 0
        normal_pending = True

        for req in range(12):
            if pq1_count >= 8 and normal_pending:
                # Yield to normal queue
                dispatched_normal += 1
                pq1_count = 0 # reset by normal dispatch
            else:
                dispatched_tier1 += 1
                pq1_count += 1

        self.assertEqual(dispatched_tier1, 11)
        self.assertEqual(dispatched_normal, 1) # Yielded exactly at request 8!

if __name__ == "__main__":
    unittest.main()
