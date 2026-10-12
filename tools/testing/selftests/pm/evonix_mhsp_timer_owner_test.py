#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline timer-owner regression models; these do not execute the driver."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0006-mtk-mhsp-use-prediction-timer-owner.patch"


def callback(governors, timer_owner, executing_cpu, fixed=True):
    gov = governors[timer_owner if fixed else executing_cpu]
    if gov["cancel"]:
        gov["cancel"] = False
    else:
        gov["invalid"] = True


class TimerOwner(unittest.TestCase):
    def test_migrated_callback_does_not_invalidate_other_cpu(self):
        governors = [{"cancel": False, "invalid": False} for _ in range(8)]
        callback(governors, 7, 0)
        self.assertTrue(governors[7]["invalid"])
        self.assertFalse(governors[0]["invalid"])

    def test_cancellation_flag_is_consumed_on_owner_only(self):
        governors = [{"cancel": True, "invalid": False} for _ in range(8)]
        callback(governors, 4, 1)
        self.assertFalse(governors[4]["cancel"])
        self.assertTrue(governors[1]["cancel"])
        self.assertFalse(any(gov["invalid"] for gov in governors))

    def test_pinned_callbacks_are_unchanged(self):
        for cpu in range(8):
            for cancel in (False, True):
                old = [{"cancel": cancel, "invalid": False} for _ in range(8)]
                new = [gov.copy() for gov in old]
                callback(old, cpu, cpu, fixed=False)
                callback(new, cpu, cpu)
                self.assertEqual(old, new)

    def test_patch_changes_only_owner_lookup(self):
        lines = PATCH.read_text().splitlines()
        removed = [line[1:] for line in lines if line.startswith("-") and not line.startswith("---")]
        added = [line[1:] for line in lines if line.startswith("+") and not line.startswith("+++")]
        self.assertEqual(removed, ["\tstruct gov_info *gov = this_cpu_ptr(&gov_infos);"])
        self.assertEqual(added, ["\tstruct gov_info *gov = container_of(h, struct gov_info, shallow.histtimer);"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
