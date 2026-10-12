#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Runtime-idle callback lookup contracts/models; not device PM tests."""

import itertools
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[4]
BEFORE = "cf4438d00878a567542eb30c112a8865965898b3"
RUNTIME = (ROOT / "drivers/base/power/runtime.c").read_text()


def idle_body(source):
    match = re.search(r"static int rpm_idle\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError("Missing rpm_idle definition")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[start:index]
    raise AssertionError("Unterminated rpm_idle")


def notify(allowed, active, pending, already_notifying, no_callbacks,
           callback, async_request, fixed):
    # Abstract observable actions after existing validation, not a PM emulator.
    actions = []
    lookups = 0
    if not allowed or not active or pending or already_notifying:
        return ["reject"], lookups
    actions.append("cancel_request")
    if fixed and no_callbacks:
        return actions + ["autosuspend"], lookups
    lookups += 1
    if not callback or no_callbacks:
        return actions + ["autosuspend"], lookups
    if async_request:
        return actions + ["queue_idle"], lookups
    return actions + ["call_idle"], lookups


class RuntimeIdle(unittest.TestCase):
    def test_no_callback_flag_precedes_lookup_but_follows_request_cleanup(self):
        body = idle_body(RUNTIME)
        self.assertLess(body.index("dev->power.request = RPM_REQ_NONE;"),
                        body.index("if (dev->power.no_callbacks)"))
        self.assertLess(body.index("if (dev->power.no_callbacks)"),
                        body.index("RPM_GET_CALLBACK(dev, runtime_idle)"))
        self.assertIn("return retval ? retval : rpm_suspend(dev, rpmflags | RPM_AUTO);",
                      body)

    def test_source_delta_is_only_the_lookup_shortcut(self):
        before = subprocess.check_output(
            ["git", "show", f"{BEFORE}:drivers/base/power/runtime.c"],
            cwd=ROOT, text=True)
        old = idle_body(before)
        expected = old.replace(
            "\tcallback = RPM_GET_CALLBACK(dev, runtime_idle);",
            "\tif (dev->power.no_callbacks)\n\t\tgoto out;\n\n"
            "\tcallback = RPM_GET_CALLBACK(dev, runtime_idle);", 1).replace(
            "if (!callback || dev->power.no_callbacks)", "if (!callback)", 1)
        self.assertEqual(idle_body(RUNTIME), expected)

    def test_validation_and_callback_actions_remain_identical(self):
        for flags in itertools.product((False, True), repeat=7):
            old_actions, old_lookups = notify(*flags, False)
            actions, lookups = notify(*flags, True)
            self.assertEqual(actions, old_actions)
            self.assertLessEqual(lookups, old_lookups)
            if old_lookups and flags[4]:
                self.assertEqual(lookups, 0)
            else:
                self.assertEqual(lookups, old_lookups)


if __name__ == "__main__":
    unittest.main()
