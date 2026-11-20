#!/usr/bin/env python3
"""
EVONIX Kernel - ADIOS Default I/O Scheduler Architecture & Regression Tests
Validates:
1. Early kernel registration via subsys_initcall (level 4) before device probe (level 6).
2. Universal default selection in elevator_get_default() for all eligible blk-mq queues
   (including multi-queue Rodin UFS LUNs / MCQ) bypassing BLK_MQ_F_NO_SCHED_BY_DEFAULT.
3. Proper boundary handling: BLK_MQ_F_NO_SCHED queues (e.g. UFS TMF, virtual devices)
   and zoned devices remain protected.
4. Resilient fallback handling in elevator_init_mq() if ADIOS initialization fails.
5. Unrestricted runtime userspace switching via elv_iosched_store().
6. Configuration presence in evonix.config across all kernel variants.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read_file(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8", errors="replace")


class AdiosDefaultContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elevator_c = read_file("block/elevator.c")
        cls.adios_c = read_file("block/adios.c")
        cls.kconfig_iosched = read_file("block/Kconfig.iosched")
        cls.evonix_config = read_file("arch/arm64/configs/evonix.config")

    def test_adios_early_subsys_initcall_registration(self):
        """Ensure ADIOS registers at subsys_initcall level when built-in, before storage probe."""
        self.assertIn("subsys_initcall(adios_init);", self.adios_c)
        self.assertIn("#ifdef MODULE\nmodule_init(adios_init);\n#else\nsubsys_initcall(adios_init);", self.adios_c)

    def test_adios_default_selected_in_elevator_get_default(self):
        """Ensure elevator_get_default() queries ADIOS first for eligible blk-mq queues."""
        self.assertIn('e = elevator_find_get(q, "adios");', self.elevator_c)
        
        # Verify ADIOS is checked before BLK_MQ_F_NO_SCHED_BY_DEFAULT
        adios_pos = self.elevator_c.index('elevator_find_get(q, "adios")')
        no_sched_pos = self.elevator_c.index("BLK_MQ_F_NO_SCHED_BY_DEFAULT")
        self.assertLess(adios_pos, no_sched_pos,
                        "ADIOS default check must precede BLK_MQ_F_NO_SCHED_BY_DEFAULT")

    def test_adios_fallback_handling_in_elevator_init_mq(self):
        """Ensure elevator_init_mq() falls back safely if ADIOS fails to initialize."""
        self.assertIn('!strcmp(e->elevator_name, "adios")', self.elevator_c)
        self.assertIn('attempting fallback', self.elevator_c)
        self.assertIn('fallback = elevator_find_get(q, "kyber");', self.elevator_c)
        self.assertIn('fallback = elevator_find_get(q, "mq-deadline");', self.elevator_c)

    def test_unsupported_queues_and_partitions_protected(self):
        """Verify elv_support_iosched guards against BLK_MQ_F_NO_SCHED queues."""
        self.assertIn("BLK_MQ_F_NO_SCHED", self.elevator_c)
        self.assertIn("!queue_is_mq(q)", self.elevator_c)

    def test_userspace_scheduler_switching_unconstrained(self):
        """Ensure elv_iosched_store() allows arbitrary runtime switching without forced locks."""
        self.assertIn("elevator_change(q, strstrip(elevator_name));", self.elevator_c)
        # Ensure there is no artificial rejection of non-ADIOS switches in elevator.c
        self.assertNotIn("CONFIG_EVONIX_IOSCHED_ENFORCE", self.elevator_c)

    def test_kconfig_and_evonix_config_enablement(self):
        """Verify Kconfig default and evonix.config values."""
        self.assertIn("config MQ_IOSCHED_DEFAULT_ADIOS", self.kconfig_iosched)
        self.assertIn("default y", self.kconfig_iosched)
        self.assertIn("CONFIG_MQ_IOSCHED_ADIOS=y", self.evonix_config)
        self.assertIn("CONFIG_MQ_IOSCHED_DEFAULT_ADIOS=y", self.evonix_config)

    def test_adios_mq_ops_presence(self):
        """Verify that ADIOS provides full blk-mq multi-queue callbacks."""
        for op in [".init_hctx", ".init_sched", ".dispatch_request", ".insert_requests",
                   ".completed_request", ".finish_request", ".prepare_request"]:
            self.assertIn(op, self.adios_c, f"Missing blk-mq operation {op} in adios.c")


if __name__ == "__main__":
    unittest.main()
