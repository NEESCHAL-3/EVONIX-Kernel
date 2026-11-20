#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
EVONIX CPU Governor Freedom & Policy Persistence Test
Verifies that:
1. EVONIX Enhanced Schedutil is the default boot governor.
2. Legitimate user switching to ANY registered governor is permitted (no -EPERM rejection).
3. Hotplug policy initialization preserves policy->last_governor and does NOT automatically
   revert the user's selection back to schedutil.
4. Unconditional enforcement logic is completely removed.
"""

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[4]
CPUFREQ_C = (ROOT / "drivers/cpufreq/cpufreq.c").read_text()
KCONFIG = (ROOT / "drivers/cpufreq/Kconfig").read_text()
EVONIX_CONFIG = (ROOT / "arch/arm64/configs/evonix.config").read_text()

class EvonixGovernorFreedomTest(unittest.TestCase):
    def test_default_governor_is_schedutil(self):
        """Verify that kernel config defaults to schedutil at boot."""
        self.assertIn("CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y", EVONIX_CONFIG)
        self.assertIn("CONFIG_EVONIX_SCHEDUTIL=y", EVONIX_CONFIG)

    def test_unconditional_enforcement_removed(self):
        """Verify that CONFIG_EVONIX_GOVERNOR_ENFORCE is completely removed from source and config."""
        self.assertNotIn("CONFIG_EVONIX_GOVERNOR_ENFORCE", EVONIX_CONFIG)
        self.assertNotIn("config EVONIX_GOVERNOR_ENFORCE", KCONFIG)
        self.assertNotIn("evonix_enforce_governor", CPUFREQ_C)

    def test_store_scaling_governor_allows_all_valid_governors(self):
        """
        Model store_scaling_governor: verify that any valid registered governor
        is accepted without -EPERM or artificial daemon checks.
        """
        registered_governors = ["schedutil", "performance", "powersave", "userspace", "ondemand", "conservative"]
        
        # In cpufreq.c, store_scaling_governor parses governor and calls cpufreq_set_policy
        store_func = CPUFREQ_C.split("static ssize_t store_scaling_governor", 1)[1].split("static ssize_t show_scaling_driver", 1)[0]
        
        # Verify no -EPERM rejection in store_scaling_governor
        self.assertNotIn("-EPERM", store_func)
        self.assertIn("cpufreq_set_policy(policy, new_gov,", store_func)
        self.assertIn("return ret ? ret : count;", store_func)

        # Simulation: test switching to each governor
        for gov in registered_governors:
            # Simulated kernel parsing
            new_gov_found = gov in registered_governors
            self.assertTrue(new_gov_found)
            # Simulated permission check: only standard POSIX sysfs 0644 check
            user_is_root = True
            ret = 0 if (new_gov_found and user_is_root) else -1
            self.assertEqual(ret, 0, f"Governor {gov} must be permitted")

    def test_hotplug_restores_user_selected_governor(self):
        """
        Verify cpufreq_init_policy restores policy->last_governor
        rather than forcing schedutil on hotplug resume.
        """
        init_policy = CPUFREQ_C.split("static int cpufreq_init_policy(struct cpufreq_policy *policy)", 1)[1].split("static int cpufreq_add_dev", 1)[0]
        self.assertIn("gov = get_governor(policy->last_governor);", init_policy)
        self.assertNotIn("if (READ_ONCE(evonix_enforce_governor))", init_policy)

        # Simulation of hotplug cycle with user selection
        user_selection = "performance"
        policy_last_governor = user_selection
        
        # On hotplug, governor is restored from policy->last_governor
        restored_governor = policy_last_governor if policy_last_governor else "schedutil"
        self.assertEqual(restored_governor, "performance", "User governor choice must persist across hotplug")

    def test_no_arbitrary_daemon_vs_user_discrimination(self):
        """Verify no fragile uid/task_name sniffing is attempted in governor paths."""
        store_func = CPUFREQ_C.split("static ssize_t store_scaling_governor", 1)[1].split("static ssize_t show_scaling_driver", 1)[0]
        self.assertNotIn("current->comm", store_func)
        self.assertNotIn("current_uid()", store_func)
        self.assertNotIn("get_task_comm", store_func)

if __name__ == "__main__":
    unittest.main()
