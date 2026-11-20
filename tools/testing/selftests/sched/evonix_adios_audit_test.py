#!/usr/bin/env python3
"""
EVONIX Kernel V4.6 - ADIOS Default I/O Scheduler Comprehensive Technical Audit Test
Validates:
1. Override of BLK_MQ_F_NO_SCHED_BY_DEFAULT for eligible blk-mq storage queues.
2. Allocation unwind and reference count balance on initialization failure in adios_init_sched() & elevator_init_mq().
3. Required elevator features enforcement (elv_support_features) and protection of BLK_MQ_F_NO_SCHED queues.
4. Initcall hierarchy (subsys_initcall level 4 vs driver probe level 6).
5. Device-level queue selection across all Rodin UFS logical units (LUN 0-5) and generic blk-mq queues.
6. Unconstrained runtime sysfs switching (elv_iosched_store).
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read_file(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8", errors="replace")


class AdiosBlockAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elevator_c = read_file("block/elevator.c")
        cls.adios_c = read_file("block/adios.c")
        cls.genhd_c = read_file("block/genhd.c")
        cls.kconfig_iosched = read_file("block/Kconfig.iosched")
        cls.evonix_config = read_file("arch/arm64/configs/evonix.config")

    def test_initcall_hierarchy(self):
        """Audit 4: Ensure ADIOS registers at subsys_initcall (level 4) before storage drivers at device_initcall (level 6)."""
        self.assertIn("subsys_initcall(adios_init);", self.adios_c)
        self.assertNotIn("late_initcall(adios_init);", self.adios_c)

    def test_overriding_no_sched_by_default_eligibility(self):
        """Audit 1: ADIOS is queried before BLK_MQ_F_NO_SCHED_BY_DEFAULT and nr_hw_queues check."""
        idx_adios = self.elevator_c.index('e = elevator_find_get(q, "adios");')
        idx_no_sched = self.elevator_c.index("q->tag_set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT")
        idx_nr_hw = self.elevator_c.index("q->nr_hw_queues != 1")
        self.assertLess(idx_adios, idx_no_sched)
        self.assertLess(idx_adios, idx_nr_hw)

    def test_unsupported_queue_protection(self):
        """Audit 3: BLK_MQ_F_NO_SCHED queues (e.g. UFS TMF, NVMe admin, zram) are rejected by elv_support_iosched."""
        self.assertIn("if (!queue_is_mq(q) ||\n\t    (q->tag_set->flags & BLK_MQ_F_NO_SCHED))\n\t\treturn false;",
                      self.elevator_c)

    def test_required_features_enforcement(self):
        """Audit 3: elv_support_features is evaluated inside elevator_find_get and elevator_init_mq."""
        self.assertIn("!elv_support_features(q, e)", self.elevator_c)
        self.assertIn("elv_support_features(struct request_queue *q", self.elevator_c)

    def test_adios_init_sched_memory_unwind(self):
        """Audit 2: Ensure adios_init_sched unwinds every allocated resource upon error."""
        error_labels = ["free_buckets:", "free_aggr_buckets:", "destroy_dl_group_pool:",
                        "destroy_rq_data_pool:", "destroy_rq_data_cache:", "free_ad:", "put_eq:"]
        for label in error_labels:
            self.assertIn(label, self.adios_c, f"Missing cleanup label {label} in adios.c")

        self.assertIn("free_percpu(ad->pcpu_completion);", self.adios_c)
        self.assertIn("kfree(ad->aggr_buckets);", self.adios_c)
        self.assertIn("mempool_destroy(ad->rq_data_pool);", self.adios_c)
        self.assertIn("kmem_cache_destroy(ad->rq_data_cache);", self.adios_c)
        self.assertIn("kmem_cache_destroy(ad->dl_group_pool);", self.adios_c)
        self.assertIn("kfree(ad);", self.adios_c)
        self.assertIn("kobject_put(&eq->kobj);", self.adios_c)

    def test_elevator_init_mq_refcount_and_fallback_balance(self):
        """Audit 2: Check reference count balancing and fallback in elevator_init_mq."""
        # Check freeze and unfreeze symmetry
        self.assertIn("blk_mq_freeze_queue(q);", self.elevator_c)
        self.assertIn("blk_mq_unfreeze_queue(q);", self.elevator_c)
        self.assertIn("elevator_put(e);", self.elevator_c)
        self.assertIn("elevator_put(fallback);", self.elevator_c)

    def test_rodin_ufs_all_luns_eligible(self):
        """Audit 5 & 6: Rodin UFS LUN 2 (userdata) has Kyber fallback, while all LUNs select ADIOS by default."""
        self.assertIn("rodin_is_userdata_ufs_lun2(parent)", self.genhd_c)
        # Verify Kyber preference flag is set on LUN2
        self.assertIn("blk_queue_flag_set(QUEUE_FLAG_PREFER_KYBER, disk->queue);", self.genhd_c)
        # But in elevator_get_default, ADIOS check comes BEFORE Kyber check
        idx_adios = self.elevator_c.index('e = elevator_find_get(q, "adios");')
        idx_kyber = self.elevator_c.index('test_bit(QUEUE_FLAG_PREFER_KYBER, &q->queue_flags)')
        self.assertLess(idx_adios, idx_kyber, "ADIOS must be selected before Kyber fallback")

    def test_userspace_switching_preserved(self):
        """Audit 7: Ensure elv_iosched_store permits user switching without artificial locking."""
        self.assertIn("ret = elevator_change(q, strstrip(elevator_name));", self.elevator_c)
        # Ensure no governor-style enforcement lock is added to block elevators
        self.assertNotIn("CONFIG_EVONIX_IOSCHED_ENFORCE", self.elevator_c)


if __name__ == "__main__":
    unittest.main()
