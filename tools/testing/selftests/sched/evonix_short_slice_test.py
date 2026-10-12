#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Request-sizing model and source contracts, not compiled-kernel tests."""

from pathlib import Path
import re
import unittest

from evonix_policy_test import function as extract_function

ROOT = Path(__file__).resolve().parents[4]
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
FEATURES = (ROOT / "kernel/sched/features.h").read_text()


def function(source, name):
    signature = re.search(r"(?m)^(?:static (?:inline )?\w+ )?" + name + r"\(", source)
    if not signature:
        raise AssertionError(f"Missing definition: {name}")
    return extract_function(source[signature.start():], name)


def request(base, enabled=True, task=True, normal=True, sensitive=True):
    return max(1, base >> 1) if all((enabled, task, normal, sensitive)) else base


class ShortSlice(unittest.TestCase):
    def test_two_ms_base_yields_one_ms_ui_request(self):
        self.assertEqual(request(2_000_000), 1_000_000)

    def test_non_ui_groups_batch_and_rollback_keep_base(self):
        for gate in ("enabled", "task", "normal", "sensitive"):
            self.assertEqual(request(2_000_000, **{gate: False}), 2_000_000)

    def test_odd_and_small_requests_cannot_be_zero(self):
        for base in range(1, 8192):
            self.assertGreaterEqual(request(base), 1)
            self.assertLessEqual(request(base), base)

    def test_weighted_deadlines_keep_proportional_service_accounting(self):
        # Request length changes deadlines, not vruntime's nice-weight slope.
        for weight in (256, 1024, 4096):
            self.assertEqual(request(2_000_000) * 1024 / weight,
                             (2_000_000 * 1024 / weight) / 2)
        helper = function(FAIR, "evonix_entity_slice")
        self.assertNotIn("load.weight", helper)
        self.assertNotIn("vruntime =", helper)

    def test_only_placement_and_renewal_size_requests(self):
        self.assertEqual(FAIR.count("se->slice = evonix_entity_slice(se);"), 2)
        for name in ("place_entity", "update_deadline"):
            body = function(FAIR, name)
            self.assertIn("se->slice = evonix_entity_slice(se);", body)
            self.assertIn("calc_delta_fair(se->slice, se)", body)
        self.assertNotIn("evonix_entity_slice", function(FAIR, "check_preempt_wakeup"))

    def test_opt_in_uses_rom_flag_not_names_or_new_structures(self):
        helper = function(FAIR, "evonix_entity_slice")
        for token in ("CONFIG_EVONIX_RODIN_SHORT_SLICE", "entity_is_task(se)",
                      "p->policy == SCHED_NORMAL", "READ_ONCE(task_group(p)->latency_sensitive)"):
            self.assertIn(token, helper)
        self.assertNotIn("comm", helper)
        self.assertGreater(FEATURES.index("SCHED_FEAT(EVONIX_LATENCY_SLICE, true)"),
                           FEATURES.index("SCHED_FEAT(EVONIX_UI_WAKEUP, false)"))
        self.assertIn("SCHED_FEAT(EVONIX_LATENCY_SLICE, false)", FEATURES)
        self.assertIn("CONFIG_EVONIX_RODIN_SHORT_SLICE=y",
                      (ROOT / "arch/arm64/configs/evonix.config").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
