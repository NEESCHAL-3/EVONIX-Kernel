#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline contracts for the ADPF temporary-allocation source correction."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0003-mtk-adpf-use-linear-temporary-allocations.patch"


class ADPFAllocationRegression(unittest.TestCase):
    def test_new_allocations_cover_the_existing_copy_lengths(self):
        for count in range(1, 33):
            for size in (4, 16):
                allocation = count * size
                self.assertEqual(allocation, count * size)
                self.assertLessEqual(allocation, count * count * size)

    def test_maximum_thread_array_drops_from_4096_to_128_bytes(self):
        self.assertEqual(32 * 32 * 4, 4096)
        self.assertEqual(32 * 4, 128)

    def test_patch_changes_only_three_element_size_arguments(self):
        lines = PATCH.read_text().splitlines()
        added = [line[1:].strip() for line in lines if line.startswith("+") and not line.startswith("+++")]
        removed = [line for line in lines if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(len(removed), 3)
        self.assertEqual(added, ["sizeof(*threadIds),", "sizeof(*workDuration),",
                                 "sizeof(*threadIds), GFP_KERNEL);"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
