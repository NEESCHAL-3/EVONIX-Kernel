#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline bounds contracts for the unapplied MediaTek ADPF report fix."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0005-mtk-adpf-bound-work-duration-reports.patch"


class ADPFBoundsRegression(unittest.TestCase):
    def test_only_counts_inside_the_existing_array_are_accepted(self):
        for count in (-2147483648, -1, 0, 1, 16, 32, 33, 2147483647):
            valid = not (count <= 0 or count > 32)
            self.assertEqual(valid, count in (1, 16, 32))
            if valid:
                self.assertTrue(all(0 <= index < 32 for index in range(count)))

    def test_null_callback_input_is_rejected(self):
        for present, count in ((False, 1), (False, 32), (True, 0), (True, 33)):
            self.assertFalse(present and 0 < count <= 32)

    def test_patch_checks_both_layers_without_changing_session_layout(self):
        text = PATCH.read_text()
        self.assertIn("t_msgKM->work_duration_size > ADPF_MAX_THREAD", text)
        self.assertIn("work_duration_size > ADPF_MAX_THREAD", text)
        self.assertIn("if (!workDuration", text)
        self.assertNotIn("diff --git a/include/", text)
        self.assertEqual(text.count("diff --git "), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
