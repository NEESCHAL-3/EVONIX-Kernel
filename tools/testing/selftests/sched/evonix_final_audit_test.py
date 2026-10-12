#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline C boundary tests and source contracts; not a device/boot test."""
from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest

from evonix_policy_test import function as raw_function

ROOT = Path(__file__).resolve().parents[4]
BASE = "05af5952a767adf0a6e9b9c697831132d415ac2b"


def function(text, name):
    # Avoid names/braces in explanatory comments being parsed as definitions.
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    return raw_function(text, name)


def source(path, baseline=False):
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def inheritance(text):
    marker = "p->default_timer_slack_ns = current->timer_slack_ns;"
    end = text.index(marker) + len(marker)
    start = text.rfind("if (current->policy == SCHED_FIFO", 0, end)
    # The baseline has no RT-parent branch.
    if start < 0 or end - start > 300:
        start = text.index(marker)
    return text[start:end]


def reset(text):
    return re.search(r"p->policy = SCHED_NORMAL;.*?p->rt_priority = 0;"
                     r"(?:\s*p->timer_slack_ns = p->default_timer_slack_ns;)?",
                     function(text, "sched_fork"), re.S).group()


class TimerSlackC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-slack-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        c = """
#include <stdbool.h>
#include <stdio.h>
typedef unsigned long long u64;
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_DEADLINE 3
#define NICE_TO_PRIO(n) (120 + (n))
struct task_struct { int policy, static_prio, rt_priority;
                    u64 timer_slack_ns, default_timer_slack_ns; };
"""
        for name, baseline in (("old", True), ("new", False)):
            c += f"static u64 {name}(int policy, u64 slack, u64 def, int do_reset) {{\n"
            c += ("struct task_struct parent = {.policy=policy, .timer_slack_ns=slack,"
                  ".default_timer_slack_ns=def};\n"
                  "struct task_struct child=parent, *p=&child, *current=&parent;\n")
            c += inheritance(source("kernel/fork.c", baseline)) + "\n"
            c += "if (do_reset) {\n" + reset(source("kernel/sched/core.c", baseline)) + "\n}\n"
            c += "return p->timer_slack_ns ? p->timer_slack_ns : p->default_timer_slack_ns;\n}\n"
        c += """
int main(void) {
    int policy, do_reset;
    u64 slack, def;
    while (scanf("%d %llu %llu %d", &policy, &slack, &def, &do_reset)==4)
        printf("%llu %llu\\n", old(policy,slack,def,do_reset),
               new(policy,slack,def,do_reset));
    return 0;
}
"""
        (directory / "slack.c").write_text(c)
        cls.exe = directory / "slack"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(directory / "slack.c"), "-o", str(cls.exe)], check=True)

    def evaluate(self, cases):
        result = subprocess.check_output(
            [str(self.exe)], input="".join(" ".join(map(str, c))+"\n" for c in cases),
            text=True)
        return [tuple(map(int, line.split())) for line in result.splitlines()]

    def test_rt_reset_restores_default_and_baseline_loses_it(self):
        self.assertEqual(self.evaluate([(p, 0, 50000, 1) for p in (1, 2, 3)]),
                         [(0, 50000)] * 3)

    def test_normal_user_slack_is_unchanged(self):
        rng = random.Random(6899)
        cases = [(0, rng.randrange(1, 2**63), 50000, reset)
                 for reset in (0, 1) for _ in range(1000)]
        self.assertEqual(self.evaluate(cases), [(c[1], c[1]) for c in cases])

    def test_initial_task_has_a_valid_reset_target(self):
        text = source("init/init_task.c")
        live = re.search(r"\.timer_slack_ns\s*=\s*(\d+)", text).group(1)
        reset_value = re.search(r"\.default_timer_slack_ns\s*=\s*(\d+)", text).group(1)
        self.assertEqual(live, reset_value)
        self.assertGreater(int(reset_value), 0)

    def test_rt_zero_slack_policy_is_preserved(self):
        before = function(source("kernel/sched/core.c", True), "__setscheduler_params")
        after = function(source("kernel/sched/core.c"), "__setscheduler_params")
        self.assertEqual(before, after)


class LifetimeContracts(unittest.TestCase):
    def test_blkg_failed_insert_kills_percpu_reference(self):
        text = function(source("block/blk-cgroup.c"), "blkg_create")
        self.assertIn("percpu_ref_kill(&blkg->refcnt)", text)
        success = text.index("if (likely(!ret))")
        self.assertGreater(text.index("blkg->online = true"), success)

    def test_orphan_cleanup_includes_last_loaded_buffer(self):
        text = function(source("fs/ext4/orphan.c"), "ext4_init_orphan_info")
        self.assertLess(text.index("loaded++"), text.index("ot = ext4_orphan_block_tail"))
        self.assertIn("while (loaded > 0)", text)
        self.assertIn("brelse(oi->of_binfo[loaded].ob_bh)", text)

    def test_cpu_nanosleep_error_releases_pid_reference(self):
        text = function(source("kernel/time/posix-cpu-timers.c"), "do_cpu_nanosleep")
        error = re.search(r"if \(error\) \{(.*?)\n\s*\}", text, re.S).group(1)
        self.assertLess(error.index("posix_cpu_timer_del"),
                        error.index("spin_unlock_irq"))

    def test_exec_retry_has_ordered_signal_teardown(self):
        text = function(source("kernel/time/posix-cpu-timers.c"), "timer_lock_sighand")
        self.assertIn("guard(rcu)()", text)
        self.assertIn("for (;;)", text)
        self.assertIn("pid_task", text)
        self.assertIn("lock_task_sighand", text)
        self.assertIn("smp_rmb()", text)
        self.assertIn("smp_store_release(&tsk->sighand, NULL)",
                      function(source("kernel/exit.c"), "__exit_signal"))
        self.assertIn("smp_acquire__after_ctrl_dep()",
                      function(source("kernel/signal.c"), "__lock_task_sighand"))
        for name in ("posix_cpu_timer_del", "posix_cpu_timer_set",
                     "posix_cpu_timer_rearm"):
            self.assertIn("timer_lock_sighand", function(
                source("kernel/time/posix-cpu-timers.c"), name))

    def test_all_possible_cpu_latency_buckets_are_folded(self):
        for path, name in (("block/kyber-iosched.c", "kyber_timer_fn"),
                           ("block/blk-iocost.c", "ioc_lat_stat")):
            text = function(source(path), name)
            self.assertIn("for_each_possible_cpu", text)

    def test_idle_domain_name_is_an_owned_allocation(self):
        text = function(source("drivers/cpuidle/dt_idle_genpd.c"), "dt_idle_pd_alloc")
        self.assertIn("kstrdup(kbasename(of_node_full_name(np)), GFP_KERNEL)", text)
        self.assertNotIn("pd->name = kbasename(pd->name)", text)

    def test_direct_io_drains_before_buffered_fallback(self):
        text = function(source("fs/ext4/file.c"), "ext4_buffered_write_iter")
        self.assertLess(text.index("inode_dio_wait"), text.index("ext4_write_checks"))

    def test_redzone_alone_does_not_limit_zeroing(self):
        text = function(source("mm/slab.h"), "slab_post_alloc_hook")
        self.assertIn("kmem_cache_debug_flags(s, SLAB_STORE_USER)", text)
        self.assertNotIn("SLAB_RED_ZONE | SLAB_STORE_USER", text)

    def test_arp_names_are_terminated(self):
        self.assertIn("r.arp_dev[IFNAMSIZ - 1] = '\\0';",
                      function(source("net/ipv4/arp.c"), "arp_ioctl"))

    def test_existing_f2fs_unmount_fix_is_retained(self):
        text = function(source("fs/f2fs/data.c"), "f2fs_write_end_io")
        self.assertLess(text.index("f2fs_in_warm_node_list"), text.index("dec_page_count"))

    def test_existing_thp_publication_fix_is_retained(self):
        text = function(source("mm/huge_memory.c"), "get_huge_zero_page")
        self.assertLess(text.index("WRITE_ONCE(huge_zero_pfn"),
                        text.index("atomic_set_release"))
        self.assertIn("spin_lock(&huge_zero_lock)", text)

    def test_swap_rejects_fscrypt_files_before_header_io(self):
        text = source("mm/swapfile.c")
        swapon = text[text.index("SYSCALL_DEFINE2(swapon,"):]
        check = swapon.index("if (IS_ENCRYPTED(inode))")
        self.assertLess(check, swapon.index("Read the swap header"))
        self.assertIn("error = -EINVAL", swapon[check:check + 350])

    def test_configuration_and_module_abi_inputs_are_unchanged(self):
        for path in ("arch/arm64/configs/evonix.config",
                     "arch/arm64/configs/gki_defconfig", "android/abi_gki_aarch64",
                     "build.config.constants", "Makefile"):
            before, after = source(path, True), source(path)
            if path.endswith(("evonix.config", "gki_defconfig")):
                # Release comments are metadata, not configuration or KMI inputs.
                pattern = r"(?m)^(?:CONFIG_[A-Z0-9_]+=.*|# CONFIG_[A-Z0-9_]+ is not set)$"
                before = re.findall(pattern, before)
                after = re.findall(pattern, after)
            self.assertEqual(before, after, path)


class NetworkBoundsC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-net-bounds-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        xps = re.search(r"if \(tc < 0 \|\| tc >= num_tc\)\s*return -EINVAL;",
                        source("net/core/dev.c")).group()
        ipv6 = re.search(r"if \(skb->len - start < hdrlen\)\s*return -EBADMSG;",
                         source("net/ipv6/exthdrs_core.c")).group()
        c = "#include <errno.h>\n#include <stdio.h>\n"
        c += "static int xps(int tc, int num_tc) {\n{\n" + xps + "\n}\nreturn 0;\n}\n"
        c += ("struct packet { unsigned len; };"
              "static int ipv6(struct packet *skb, unsigned start, unsigned hdrlen) {\n{\n")
        c += ipv6 + "\n}\nreturn 0;\n}\n"
        c += """
int main(void) {
    int tc, n;
    unsigned length, start, hdrlen;
    while (scanf("%d %d %u %u %u", &tc,&n,&length,&start,&hdrlen)==5) {
        struct packet skb={length};
        printf("%d %d\\n",xps(tc,n),ipv6(&skb,start,hdrlen));
    }
    return 0;
}
"""
        (directory / "bounds.c").write_text(c)
        cls.exe = directory / "bounds"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(directory / "bounds.c"), "-o", str(cls.exe)], check=True)

    def evaluate(self, cases):
        data = "".join(" ".join(map(str,c))+"\n" for c in cases)
        out = subprocess.check_output([str(self.exe)], input=data, text=True)
        return [tuple(map(int,line.split())) for line in out.splitlines()]

    def test_xps_class_boundaries(self):
        cases = [(tc,n,128,40,8) for n in range(1,17) for tc in range(-1,18)]
        results = self.evaluate(cases)
        for (tc,n,*_), (xps,ipv6) in zip(cases,results):
            self.assertEqual(xps, 0 if 0 <= tc < n else -22)
            self.assertEqual(ipv6, 0)

    def test_ipv6_declared_header_cannot_cross_packet_end(self):
        rng = random.Random(8400)
        # The parser has already verified the starting offset/header pointer.
        cases = [(0,1,length,start,rng.randrange(8,2049))
                 for length in range(40,2049,7)
                 for start in (40,length)]
        results = self.evaluate(cases)
        for (_,_,length,start,hdrlen), (xps,ipv6) in zip(cases,results):
            self.assertEqual(xps,0)
            self.assertEqual(ipv6, 0 if hdrlen <= length-start else -74)


if __name__ == "__main__":
    unittest.main(verbosity=2)
