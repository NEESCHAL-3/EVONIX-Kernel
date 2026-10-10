#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline PM source contracts and snapshot fixtures, not kernel execution."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
MAIN = (ROOT / "kernel/power/main.c").read_text()
CONFIG = (ROOT / "arch/arm64/configs/evonix.config").read_text()
SCRIPT = ROOT / "tools/testing/selftests/pm/evonix_idle_snapshot.sh"


class PMContracts(unittest.TestCase):
    def test_pm_queue_is_opt_in_and_still_freezable(self):
        block = MAIN.split("static int __init pm_start_workqueues(void)", 1)[1]
        block = block.split("static int __init pm_init(void)", 1)[0]
        self.assertIn("unsigned int flags = WQ_FREEZABLE;", block)
        self.assertIn("if (IS_ENABLED(CONFIG_EVONIX_RODIN_PM_WQ))", block)
        self.assertIn("flags |= WQ_POWER_EFFICIENT;", block)
        self.assertIn('alloc_workqueue("pm", flags, 0)', block)
        self.assertIn('alloc_ordered_workqueue("pm_fs_sync", 0)', block)
        self.assertIn("destroy_workqueue(pm_wq);", block)

    def test_generic_default_is_unchanged(self):
        kconfig = (ROOT / "kernel/power/Kconfig").read_text()
        block = kconfig.split("config EVONIX_RODIN_PM_WQ\n", 1)[1]
        block = block.split("\nconfig ", 1)[0]
        self.assertIn("depends on PM && ARM64_TUNE_CORTEX_A725", block)
        self.assertNotRegex(block, r"\bdefault\s+y\b")
        self.assertIn("CONFIG_EVONIX_RODIN_PM_WQ=y", CONFIG)

    def test_existing_boot_rollback_reverts_conversion(self):
        source = (ROOT / "kernel/workqueue.c").read_text()
        self.assertIn("(flags & WQ_POWER_EFFICIENT) && wq_power_efficient", source)
        self.assertIn("module_param_named(power_efficient, wq_power_efficient, bool, 0444)", source)
        # Model the only new flag decision, including generic and rollback paths.
        for opt_in, efficient, unbound in ((False, False, False),
                                          (False, True, False),
                                          (True, False, False),
                                          (True, True, True)):
            self.assertEqual(opt_in and efficient, unbound)

    def test_runtime_and_suspend_safety_paths_remain(self):
        runtime = (ROOT / "drivers/base/power/runtime.c").read_text()
        for token in ("rpm_check_suspend_allowed(dev)", "pm_runtime_cancel_pending(dev)",
                      "rpm_get_suppliers(dev)", "cancel_work_sync(&dev->power.work)"):
            self.assertIn(token, runtime)
        self.assertIn("if (pm_wakeup_pending())", MAIN)
        self.assertIn("ksys_sync_helper();", MAIN)

    def test_existing_idle_savings_are_not_removed(self):
        for option in ("CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y", "CONFIG_RCU_LAZY=y",
                       "CONFIG_KFENCE_DEFERRABLE=y", "CONFIG_NO_HZ_IDLE=y"):
            self.assertIn(option, CONFIG)
        self.assertNotIn("CONFIG_RCU_LAZY_DEFAULT_OFF=y", CONFIG)

    def test_snapshot_has_no_background_monitor_or_control_writes(self):
        source = SCRIPT.read_text()
        self.assertNotRegex(source, r"\b(sleep|mount|umount|setprop|tee|insmod|rmmod)\s")
        self.assertNotRegex(source, r"(?<![>&])>\s*(/sys|/proc|\"?\$sample_root)")
        self.assertNotIn("while true", source)
        self.assertIn("timeout -k 1 2 head -n 256", source)
        self.assertNotIn("read_node /sys/power/wakeup_count", source)
        self.assertIn('"$sample_devices" -gt 128', source)


class SnapshotFixtures(unittest.TestCase):
    def run_snapshot(self, root, *options):
        return subprocess.run(["sh", str(SCRIPT), "--root", str(root), *options],
                              capture_output=True, text=True, timeout=15)

    def test_missing_nodes_are_explicit_not_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_snapshot(Path(tmp))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[unavailable]", result.stdout)
        self.assertNotIn("/system_stat", result.stdout)

    def test_fixture_preserves_values_without_writing_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            values = {
                "proc/uptime": "100.25 600.75\n",
                "proc/sys/kernel/random/boot_id": "fixture-boot\n",
                "sys/class/power_supply/battery/charge_counter": "4000000\n",
                "sys/bus/platform/devices/test/power/runtime_status": "suspended\n",
                "sys/bus/platform/devices/test/power/control": "auto\n",
                "sys/devices/system/cpu/cpu0/cpuidle/state0/time": "1234567\n",
                "sys/devices/system/cpu/cpufreq/policy0/scaling_governor": "sugov_ext\n",
                "sys/kernel/mtk_lpm/spm/system_stat": "Suspend AP:2:60.000\n",
            }
            for name, value in values.items():
                node = root / name
                node.parent.mkdir(parents=True, exist_ok=True)
                node.write_text(value)
            result = self.run_snapshot(root, "--vendor-stat")
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, value in values.items():
                self.assertIn(value.strip(), result.stdout)
                self.assertEqual((root / name).read_text(), value)

    def test_bad_arguments_do_not_capture(self):
        for args in (["--root"], ["--root", "relative"], ["--unknown"]):
            result = subprocess.run(["sh", str(SCRIPT), *args],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("EVONIX_IDLE_SNAPSHOT_V1", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
