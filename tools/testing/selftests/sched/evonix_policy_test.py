#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline source contracts and policy models; not a kernel/runtime test."""

import itertools
from pathlib import Path
import random
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[4]
# Recorded pre-tuning source, independent of which follow-up commit is HEAD.
BASELINE = "834acf43b765b9a799e8acc124a403d289c8814d"
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
CORE = (ROOT / "kernel/sched/core.c").read_text()
FEATURES = (ROOT / "kernel/sched/features.h").read_text()


def function(source, name):
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"Missing function: {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index]
    raise AssertionError(f"Unterminated function: {name}")


def scan_clamps(source, task_min, task_max, cpus, enabled=True):
    """Model clamp aggregation using the initialization scope in the source."""
    body = function(source, "find_energy_efficient_cpu")
    per_cpu = body.index("for_each_cpu(cpu, cpus)")
    reset = body.index("unsigned long util_min = p_util_min, util_max = p_util_max;")
    clamp_min, clamp_max = task_min, task_max
    result = []
    for idle, rq_min, rq_max in cpus:
        if reset > per_cpu:
            clamp_min, clamp_max = task_min, task_max
        if enabled and not idle:
            clamp_min = max(task_min, rq_min)
            clamp_max = max(task_max, rq_max)
        result.append((clamp_min, clamp_max))
    return result


def pick(current, queued, protect=True):
    """EEVDF decision model: (vruntime, deadline, weight, protected)."""
    entities = ([current] if current is not None else []) + queued
    if not entities:
        return None
    average = sum(e[0] * e[2] for e in entities) / sum(e[2] for e in entities)
    eligible = [e for e in entities if e[0] <= average]
    if current in eligible and protect and current[3]:
        return current
    # The queued deadline winner is retained on a tie with current, matching
    # entity_before(current, best), which is a strictly-earlier comparison.
    winner = min((e for e in queued if e in eligible),
                 key=lambda e: e[1], default=None)
    if winner is None or (current in eligible and current[1] < winner[1]):
        winner = current
    return winner


def wake(current, queued, wakee, wake_ls=True, current_ls=False,
         enabled=True, normal=True, throttled=False, vendor_ignore=False):
    if not normal or throttled or vendor_ignore:
        return False, current
    protect = not (enabled and wake_ls and not current_ls)
    if pick(current, queued, protect) != wakee:
        return False, current
    if not protect:
        current = (*current[:3], False)
    return True, current


class ClampRegression(unittest.TestCase):
    def test_busy_cpu_cannot_contaminate_idle_cpu(self):
        self.assertEqual(scan_clamps(FAIR, 0, 512,
                                    [(False, 900, 1024), (True, 1024, 1024)]),
                         [(900, 1024), (0, 512)])

    def test_baseline_reproduces_the_leak(self):
        try:
            baseline = subprocess.check_output(
                ["git", "show", f"{BASELINE}:kernel/sched/fair.c"],
                cwd=ROOT, text=True, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            self.skipTest("Pre-tuning commit is unavailable in this checkout")
        if "unsigned long util_min = p_util_min, util_max = p_util_max;" not in baseline:
            self.skipTest("Baseline no longer contains the old clamp scan")
        body = function(baseline, "find_energy_efficient_cpu")
        if body.index("unsigned long util_min = p_util_min, util_max = p_util_max;") > body.index("for_each_cpu(cpu, cpus)"):
            self.skipTest("Baseline already contains the per-CPU reset")
        self.assertEqual(scan_clamps(baseline, 0, 512,
                                    [(False, 900, 1024), (True, 1024, 1024)]),
                         [(900, 1024), (900, 1024)])

    def test_each_cpu_is_independent(self):
        rng = random.Random(8400)
        for _ in range(4096):
            task_min, task_max = sorted(rng.sample(range(1025), 2))
            cpus = [(bool(rng.getrandbits(1)), *sorted(rng.sample(range(1025), 2)))
                    for _ in range(4)]
            expected = [(task_min, task_max) if idle else
                        (max(task_min, rq_min), max(task_max, rq_max))
                        for idle, rq_min, rq_max in cpus]
            self.assertEqual(scan_clamps(FAIR, task_min, task_max, cpus), expected)

    def test_disabled_uclamp_preserves_task_values(self):
        self.assertEqual(scan_clamps(FAIR, 0, 1024,
                                    [(False, 900, 1000), (True, 800, 900)], False),
                         [(0, 1024), (0, 1024)])


class WakeupRegression(unittest.TestCase):
    def setUp(self):
        self.current = (100, 300, 1024, True)
        self.wakee = (100, 200, 1024, False)

    def test_ui_wins_and_remains_winner_on_next_pick(self):
        preempt, current = wake(self.current, [self.wakee], self.wakee)
        self.assertTrue(preempt)
        self.assertFalse(current[3])
        self.assertEqual(pick(current, [self.wakee]), self.wakee)

    def test_unmarked_background_and_ui_to_ui_preserve_protection(self):
        for wake_ls, current_ls in [(False, False), (False, True), (True, True)]:
            self.assertEqual(wake(self.current, [self.wakee], self.wakee,
                                  wake_ls, current_ls), (False, self.current))

    def test_runtime_rollback_preserves_original_policy(self):
        self.assertEqual(wake(self.current, [self.wakee], self.wakee, enabled=False),
                         (False, self.current))

    def test_ineligible_wakee_does_not_interrupt(self):
        wakee = (200, 250, 1024, False)
        self.assertEqual(wake(self.current, [wakee], wakee), (False, self.current))

    def test_later_deadline_does_not_interrupt(self):
        wakee = (100, 400, 1024, False)
        self.assertEqual(wake(self.current, [wakee], wakee), (False, self.current))

    def test_deadline_tie_follows_existing_picker_tiebreak(self):
        wakee = (100, 300, 1024, False)
        preempt, current = wake(self.current, [wakee], wakee)
        self.assertTrue(preempt)
        self.assertEqual(pick(current, [wakee]), wakee)

    def test_earlier_third_task_prevents_wakee_override(self):
        other = (100, 150, 1024, False)
        self.assertEqual(wake(self.current, [self.wakee, other], self.wakee),
                         (False, self.current))

    def test_vendor_veto_batch_idle_and_throttle_preserve_protection(self):
        for option in ({"vendor_ignore": True}, {"normal": False}, {"throttled": True}):
            self.assertEqual(wake(self.current, [self.wakee], self.wakee, **option),
                             (False, self.current))

    def test_weighted_eligibility_is_preserved(self):
        for vruntime, deadline, weight in itertools.product(
                [0, 50, 100, 150, 200], [50, 200, 350], [256, 1024, 4096]):
            wakee = (vruntime, deadline, weight, False)
            expected = pick(self.current, [wakee], protect=False) == wakee
            preempt, _ = wake(self.current, [wakee], wakee)
            self.assertEqual(preempt, expected)


class NoHzRegression(unittest.TestCase):
    def test_redundant_pending_flags_do_not_need_an_atomic_update(self):
        kick_mask = 0b1011  # BALANCE, STATS, NEXT; NEWILB is separate.
        for pending in range(16):
            for request in range(1, 16):
                if request & ~kick_mask:
                    continue
                old_flags = pending | request
                old_ipi = not (pending & kick_mask)
                if pending & request == request:
                    new_flags, new_ipi = pending, False
                else:
                    new_flags, new_ipi = pending | request, not (pending & kick_mask)
                self.assertEqual((new_flags, new_ipi), (old_flags, old_ipi))

    def test_new_work_retains_original_atomic_and_ipi_ownership(self):
        body = function(FAIR, "kick_ilb")
        guard = "if ((atomic_read(nohz_flags(ilb_cpu)) & flags) == flags)"
        self.assertLess(body.index("ilb_cpu >= nr_cpu_ids"), body.index(guard))
        self.assertLess(body.index(guard), body.index("atomic_fetch_or"))
        self.assertIn("if (flags & NOHZ_KICK_MASK)", body)
        self.assertIn("smp_call_function_single_async(ilb_cpu", body)
        self.assertIn("nohz.next_balance = jiffies+1;", body)


class SourceContracts(unittest.TestCase):
    def test_unavailable_or_overloaded_eas_skips_pelt_sync(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        gate = "if (!pd || READ_ONCE(rd->overutilized))"
        sync = "sync_entity_load_avg(&p->se);"
        self.assertLess(body.index(gate), body.index(sync))
        self.assertIn("goto unlock;", body[body.index(gate):body.index(sync)])
        self.assertLess(body.index(sync), body.index("task_fits_cpu(p, cpu)"))

    def test_pelt_sync_occurs_once_before_fit_check(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        self.assertEqual(body.count("sync_entity_load_avg(&p->se);"), 1)
        self.assertLess(body.index("sync_entity_load_avg(&p->se);"),
                        body.index("task_fits_cpu(p, cpu)"))

    def test_vendor_eas_override_remains_first(self):
        body = function(FAIR, "find_energy_efficient_cpu")
        self.assertLess(body.index("if (new_cpu != INT_MAX)"),
                        body.index("sync_entity_load_avg(&p->se);"))
        self.assertIn("return new_cpu;", body)

    def test_protection_is_released_only_after_vendor_resched_veto(self):
        body = function(FAIR, "check_preempt_wakeup")
        self.assertIn("bool protect_slice = true;", body)
        self.assertLess(body.index("p->policy != SCHED_NORMAL"),
                        body.index("protect_slice = !(evonix_ui_wakeup"))
        tail = body[body.index("preempt:\n"):]
        self.assertLess(tail.index("if (ignore)"), tail.index("se->vlag = 0;"))
        self.assertLess(tail.index("se->vlag = 0;"), tail.index("resched_curr(rq);"))
        self.assertIn("if (!protect_slice)", tail)

    def test_normal_selection_still_protects_slice(self):
        self.assertIn("return pick_eevdf(cfs_rq, true);",
                      function(FAIR, "pick_next_entity"))
        self.assertIn("protect_slice && sched_feat(RUN_TO_PARITY)",
                      function(FAIR, "pick_eevdf"))
        self.assertNotRegex(FAIR, r"pick_eevdf\(cfs_rq\)")

    def test_vendor_wakeup_hook_precedes_new_preference(self):
        body = function(FAIR, "check_preempt_wakeup")
        self.assertLess(body.index("trace_android_rvh_check_preempt_wakeup(rq"),
                        body.index("protect_slice = !(evonix_ui_wakeup"))
        self.assertLess(body.index("throttled_hierarchy"),
                        body.index("protect_slice = !(evonix_ui_wakeup"))

    def test_latency_flag_uses_ordered_access_and_no_task_names(self):
        body = function(FAIR, "evonix_ui_wakeup")
        self.assertIn("#ifdef CONFIG_EVONIX_RODIN_UI_WAKEUP", body)
        self.assertIn("sched_feat(EVONIX_UI_WAKEUP)", body)
        self.assertIn("READ_ONCE(task_group(p)->latency_sensitive)", body)
        self.assertIn("!READ_ONCE(task_group(curr)->latency_sensitive)", body)
        self.assertNotIn("comm", body)
        self.assertIn("WRITE_ONCE(tg->latency_sensitive, (unsigned int)ls);", CORE)
        self.assertIn("if (ls > 1)", function(CORE, "cpu_uclamp_ls_write_u64"))

    def test_feature_is_appended_and_has_disabled_config_path(self):
        self.assertGreater(FEATURES.index("SCHED_FEAT(EVONIX_UI_WAKEUP, true)"),
                           FEATURES.index("SCHED_FEAT(HZ_BW, true)"))
        self.assertIn("SCHED_FEAT(EVONIX_UI_WAKEUP, false)", FEATURES)


if __name__ == "__main__":
    unittest.main(verbosity=2)
