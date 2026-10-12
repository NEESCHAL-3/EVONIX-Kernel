#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""ColorOS source integration contracts; does not establish boot or runtime behavior."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[4]
OPTIONS = (
    "OPLUS_FEATURE_SCHED_ASSIST", "LOCKING_PROTECT", "EVONIX_OPLUS_SA_COMMON_HOOKS",
    "OPLUS_FEATURE_OPROJECT", "OPLUS_FEATURE_PHOENIX", "OPLUS_DEVICE_IFNO",
    "OPLUS_DEVICE_INFO_MTK_PLATFORM", "DUMP_TASKS_MEM", "OPLUS_FEATURE_SIGKILL_DIAGNOSIS",
    "PROCESS_RECLAIM_ENHANCE", "OPLUS_FEATURE_KSWAPD_OPT", "COSTLY_ALLOC_MASK_RECLAIM",
    "OPLUS_FEATURE_PROACTIVE_COMPACT", "OPLUS_FEATURE_MIDAS_V2", "OPLUS_FEATURE_OCH",
    "OPLUS_FEATURE_FRAME_BOOST", "EVONIX_OPLUS_HEALTHINFO_LITE", "OPLUS_FEATURE_WAKER_IDENTIFY",
)
def source(path):
    return (ROOT / path).read_text()

class ColorOSIntegrationTests(unittest.TestCase):
    def test_native_features_are_selected_and_verified_in_delivered_image(self):
        cfg = source("arch/arm64/configs/evonix.config")
        script = source("build-evonix.sh")
        for option in OPTIONS:
            self.assertRegex(cfg, rf"(?m)^CONFIG_{option}=y$")
            self.assertIn(f"CONFIG_{option}=y", script)

    def test_observation_mode_cannot_silently_enable_recovery(self):
        cfg = source("arch/arm64/configs/evonix.config")
        kconfig = source("drivers/soc/oplus/Kconfig")
        for option in ("WATCHDOG", "RECOVERY"):
            symbol = f"EVONIX_OPLUS_PHOENIX_{option}"
            self.assertNotRegex(cfg, rf"(?m)^CONFIG_{symbol}=[ym]$")
            block = kconfig.split("config " + symbol + "\n", 1)[1].split("\nconfig ", 1)[0]
            self.assertIn("default n", block)
        self.assertIn("^CONFIG_EVONIX_OPLUS_PHOENIX_(WATCHDOG|RECOVERY)=y$",
                      source("build-evonix.sh"))

    def test_enqueue_and_dequeue_preserve_both_native_and_android_hooks(self):
        core = source("kernel/sched/core.c")
        for phase in ("after_enqueue_task", "dequeue_task"):
            self.assertEqual(core.count(f"android_rvh_{phase}_handler(NULL, rq, p, flags);"), 1)
            self.assertIn(f"trace_android_rvh_{phase}(rq, p, flags);", core)
        self.assertIn("#if IS_ENABLED(CONFIG_EVONIX_OPLUS_SA_COMMON_HOOKS)", core)

    def test_frameboost_and_waker_paths_are_retained(self):
        core = source("kernel/sched/core.c")
        for name in ("evonix_fbg_try_to_wake_up(p)", "evonix_fbg_sched_fork(p)",
                     "evonix_fbg_new_task_stats(p)", "evonix_fbg_flush_task(prev)",
                     "evonix_waker_identify_try_to_wake_up(p)"):
            self.assertEqual(core.count(name + ";"), 1)
        self.assertIn("<linux/oplus_waker_identify.h>", core)
        self.assertIn("<linux/oplus_frame_boost.h>", core)

    def test_cpufreq_health_lifecycle_survives_common_dvfs_changes(self):
        freq = source("drivers/cpufreq/cpufreq.c")
        self.assertIn("<linux/cpufreq_health.h>", freq)
        self.assertIn("cpufreq_health_register(policy)", freq)
        self.assertIn("cpufreq_health_get_state(policy)", freq)
        self.assertRegex(freq, r"(?s)#if IS_ENABLED\(CONFIG_OPLUS_FEATURE_OCH\)\s+if \(new_policy\)")

    def test_compatibility_proc_providers_do_not_replace_native_owners(self):
        perf = source("drivers/misc/evonix_cos/evonix_cos_perf_compat.c")
        proc = source("drivers/misc/evonix_cos/evonix_cos_proc_compat.c")
        self.assertIn("!IS_ENABLED(CONFIG_OPLUS_FEATURE_OPROJECT)", perf)
        for text in (perf, proc):
            self.assertIn("!IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)", text)
        self.assertNotIn('proc_mkdir("sys/fbg"', perf)

    def test_coloros_release_identity(self):
        self.assertIn("EXTRAVERSION = -EVONIX-COS-O+-V4.5", source("Makefile"))
        self.assertTrue(source("arch/arm64/configs/evonix.config").startswith(
            "# EVONIX Kernel Configuration v4.5\n"))

    def test_production_safety_configuration_is_preserved(self):
        cfg = source("arch/arm64/configs/evonix.config")
        for option in ("CC_OPTIMIZE_FOR_PERFORMANCE", "LTO_CLANG_THIN", "AUTOFDO_CLANG", "SECURITY_SELINUX"):
            self.assertRegex(cfg, rf"(?m)^CONFIG_{option}=y$")
        for option in ("ZRAM", "ZSMALLOC"):
            self.assertRegex(cfg, rf"(?m)^CONFIG_{option}=m$")
        for option in ("CC_OPTIMIZE_FOR_PERFORMANCE_O3", "SECURITY_SELINUX_DEVELOP", "SECURITY_SELINUX_BOOTPARAM"):
            self.assertNotRegex(cfg, rf"(?m)^CONFIG_{option}=y$")

if __name__ == "__main__":
    unittest.main(verbosity=2)
