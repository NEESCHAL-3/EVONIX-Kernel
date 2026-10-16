#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline policy winner models for Rodin's nonempty 4+3+1 CPU masks."""

import itertools
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
PATCH = ROOT / "Documentation/evonix/vendor-candidates/0009-mtk-sugov-initialize-idle-policy-cpu.patch"


def winner(cpus, ratios):
    cpu_id, maximum = cpus[0], 0
    for cpu, active in zip(cpus, ratios):
        if active > maximum:
            cpu_id, maximum = cpu, active
    return cpu_id, maximum


class IdlePolicyCPU(unittest.TestCase):
    def test_idle_masks_choose_their_first_cpu(self):
        for cpus in ((0, 1, 2, 3), (4, 5, 6), (7,)):
            self.assertEqual(winner(cpus, [0] * len(cpus)), (cpus[0], 0))

    def test_busy_winner_and_tie_order_are_unchanged(self):
        for cpus in ((0, 1, 2, 3), (4, 5, 6), (7,)):
            for ratios in itertools.product((0, 256, 1024), repeat=len(cpus)):
                expected = max(ratios)
                self.assertEqual(winner(cpus, ratios), (cpus[ratios.index(expected)], expected))

    def test_sparse_policy_mask_never_selects_outside_policy(self):
        self.assertEqual(winner((1, 3), (0, 0)), (1, 0))
        self.assertEqual(winner((4, 6), (0, 512)), (6, 512))

    def test_patch_changes_only_local_initialization(self):
        lines = PATCH.read_text().splitlines()
        added = [line[1:] for line in lines if line.startswith("+") and not line.startswith("+++")]
        removed = [line[1:] for line in lines if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(added, ["\tunsigned int cpu_idx, first_cpu = cpumask_first(cpumask);",
                                 "\tunsigned int cpu_id = first_cpu;"])
        self.assertEqual(len(removed), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
