#!/usr/bin/env python3
"""Offline source contracts and arithmetic models; not kernel execution."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def source(path):
    return (ROOT / path).read_text()


def function(path, name):
    text = source(path)
    # Preserve offsets while ignoring names and braces inside C comments.
    code = re.sub(r"/\*.*?\*/|//[^\n]*", lambda match: " " * len(match[0]),
                  text, flags=re.S)
    start = None
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", code):
        depth = 1
        end = match.end()
        while depth and end < len(code):
            if code[end] == "(":
                depth += 1
            elif code[end] == ")":
                depth -= 1
            end += 1
        while end < len(code) and code[end].isspace():
            end += 1
        if depth == 0 and end < len(code) and code[end] == "{":
            start = end
            break
    if start is None:
        raise AssertionError("Missing function definition: " + name)
    depth = 0
    for end in range(start, len(code)):
        if code[end] == "{":
            depth += 1
        elif code[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise AssertionError("Unterminated function: " + name)


class StableBackportContracts(unittest.TestCase):
    def test_scheduler_runnable_product_is_wide_before_multiplication(self):
        body = function("kernel/sched/fair.c", "update_tg_cfs_runnable")
        self.assertIn("u64 new_sum;", body)
        self.assertIn("new_sum = (u64)se->avg.runnable_avg * divider;", body)
        product = 200_000 * 47_742
        self.assertGreater(product, 2**32 - 1)
        self.assertNotEqual(product, product & (2**32 - 1))

    def test_memcg_superseded_limits_stop_before_reclaim(self):
        for name, limit in (("memory_high_write", "high"),
                            ("memory_max_write", "max")):
            with self.subTest(limit=limit):
                body = function("mm/memcontrol.c", name)
                guard = f"if ({limit} != READ_ONCE(memcg->memory.{limit}))"
                self.assertIn(guard + "\n\t\t\tbreak;", body)
                self.assertLess(body.index(guard),
                                body.index("try_to_free_mem_cgroup_pages"))
                self.assertIn("signal_pending(current)", body)

    def test_munlock_counter_uses_interrupt_safe_accessor(self):
        body = function("mm/mlock.c", "__munlock_folio")
        self.assertIn("zone_stat_mod_folio(folio, NR_MLOCK, -nr_pages)", body)
        self.assertNotIn("__zone_stat_mod_folio(folio, NR_MLOCK", body)

    def test_reclaim_reports_tasks_rcu_quiescent_state(self):
        body = function("mm/vmscan.c", "shrink_lruvec")
        self.assertIn("cond_resched_tasks_rcu_qs();", body)

    def test_f2fs_overlap_check_uses_expanded_length(self):
        body = function("fs/f2fs/file.c", "f2fs_move_file_range")
        expand = "olen = len = src->i_size - pos_in;"
        overlap = "if (src == dst && pos_out > pos_in && pos_out < pos_in + len)"
        self.assertLess(body.index(expand), body.index(overlap))
        self.assertIn(overlap + "\n\t\tgoto out_unlock;", body)
        pos_in, pos_out, file_size = 0, 4096, 16384
        self.assertFalse(pos_in < pos_out < pos_in + 0)
        self.assertTrue(pos_in < pos_out < file_size)

    def test_f2fs_collapse_stops_on_writeback_failure(self):
        body = function("fs/f2fs/file.c", "f2fs_collapse_range")
        guarded_write = (
            "ret = filemap_write_and_wait_range(inode->i_mapping, offset, LLONG_MAX);"
            "\n\tif (ret)\n\t\tgoto out_unlock;"
        )
        self.assertIn(guarded_write, body)
        self.assertLess(body.index(guarded_write),
                        body.index("new_size = i_size_read(inode) - len;"))
        self.assertIn("out_unlock:\n\tfilemap_invalidate_unlock", body)
        self.assertIn("if (!ret)\n\t\tf2fs_i_size_write", body)

    def test_f2fs_symlink_propagates_error_and_removes_failed_link(self):
        body = function("fs/f2fs/namei.c", "f2fs_symlink")
        self.assertIn("err = filemap_write_and_wait_range", body)
        self.assertIn("if (!err && IS_DIRSYNC(dir))", body)
        self.assertIn("if (err)\n\t\tf2fs_unlink(dir, dentry);", body)

    def test_arm64_write_cast_preserves_expression_evaluation(self):
        text = source("arch/arm64/include/asm/percpu.h")
        for bits in (8, 16, 32, 64):
            with self.subTest(bits=bits):
                self.assertIn(
                    f"_pcp_protect(__percpu_write_{bits}, pcp, (unsigned long)(val))",
                    text,
                )
        u32_result = (0 - 1) & (2**32 - 1)
        self.assertEqual(u32_result, 0xFFFFFFFF)
        self.assertNotEqual(u32_result, (0 - 1) & (2**64 - 1))

    def test_arm64_and_cast_precedes_negation(self):
        text = source("arch/arm64/include/asm/percpu.h")
        for bits in (8, 16, 32, 64):
            with self.subTest(bits=bits):
                self.assertIn(
                    f"_pcp_protect(__percpu_andnot_case_{bits}, pcp, ~(u{bits})(val))",
                    text,
                )
        # A u32 zero must clear every bit of a u64 counter, not only its low half.
        counter = 2**64 - 1
        correct_mask = ~0 & counter
        truncated_mask = ~0 & (2**32 - 1)
        self.assertEqual(counter & ~correct_mask, 0)
        self.assertNotEqual(counter & ~truncated_mask, 0)

    def test_worker_without_current_item_is_not_dereferenced(self):
        body = function("kernel/workqueue.c", "check_flush_dependency")
        self.assertIn("worker && worker->current_pwq &&", body)
        self.assertIn("current->flags & PF_MEMALLOC", body)

    def test_watchdog_bound_is_checked_before_timer_change(self):
        body = function("kernel/workqueue.c", "wq_watchdog_param_set_thresh")
        guard = "if (thresh > MAX_JIFFY_OFFSET / HZ)\n\t\treturn -ERANGE;"
        self.assertIn(guard, body)
        self.assertLess(body.index(guard), body.index("wq_watchdog_set_thresh"))
        self.assertIn("if (system_wq)", body)
        for hz in (100, 250, 1000):
            max_offset = (2**63 - 1 >> 1) - 1
            self.assertLessEqual((max_offset // hz) * hz, max_offset)
            self.assertGreater((max_offset // hz + 1) * hz, max_offset)

    def test_block_latency_stats_include_offline_cpu_buckets(self):
        body = function("block/blk-stat.c", "blk_stat_timer_fn")
        self.assertIn("for_each_possible_cpu(cpu)", body)
        self.assertNotIn("for_each_online_cpu(cpu)", body)
        self.assertIn("blk_rq_stat_init", body)

    def test_blkcg_stats_iteration_uses_owning_list_lock(self):
        body = function("block/blk-cgroup.c", "blkcg_print_stat")
        self.assertLess(body.index("guard(spinlock_irq)(&blkcg->lock);"),
                        body.index("hlist_for_each_entry("))
        self.assertNotIn("blkg->q->queue_lock", body)

    def test_broadcast_exchange_is_locked_and_module_put_follows_unlock(self):
        body = function("kernel/time/tick-broadcast.c",
                        "tick_install_broadcast_device")
        self.assertLess(body.index("scoped_guard(raw_spinlock_irqsave"),
                        body.index("__clockevents_exchange_device(cur, dev)"))
        self.assertIn("WRITE_ONCE(tick_broadcast_device.evtdev, dev)", body)
        self.assertIn("}\n\n\t/* Module release must be outside of the lock */", body)
        self.assertLess(body.index("Module release must be outside"),
                        body.index("module_put(cur->owner)"))
        exchange = function("kernel/time/clockevents.c",
                            "__clockevents_exchange_device")
        self.assertNotIn("module_put", exchange)
        self.assertIn("__clockevents_exchange_device",
                      source("kernel/time/tick-internal.h"))

    def test_tcp_syn_replacement_repairs_hint_before_free(self):
        body = function("net/ipv4/tcp_output.c", "tcp_send_synack")
        self.assertLess(body.index("tp->retransmit_skb_hint = nskb;"),
                        body.index("tcp_rtx_queue_unlink_and_free(skb, sk)"))

    def test_tcp_urgent_retransmission_uses_one_segment(self):
        body = function("net/ipv4/tcp_output.c", "__tcp_retransmit_skb")
        self.assertIn("len = cur_mss * (tcp_urg_mode(tp) ? 1 : segs);", body)

    def test_tcp_ack_fast_path_checks_both_bounds(self):
        body = function("net/ipv4/tcp_input.c", "tcp_rcv_established")
        self.assertIn("between(TCP_SKB_CB(skb)->ack_seq, tp->snd_una, tp->snd_nxt)",
                      body)
        def between(value, start, end):
            return ((value - start) & 0xFFFFFFFF) <= ((end - start) & 0xFFFFFFFF)
        for value, expected in ((99, False), (100, True), (200, True),
                                (201, False)):
            self.assertEqual(between(value, 100, 200), expected)
        self.assertTrue(between(5, 0xFFFFFFF0, 10))
        self.assertFalse(between(20, 0xFFFFFFF0, 10))

    def test_ipv6_extension_pull_uses_requested_length_as_bound(self):
        body = function("net/core/skbuff.c", "skb_checksum_setup_ipv6")
        for header in ("ipv6_opt_hdr", "ip_auth_hdr", "frag_hdr"):
            pattern = (
                r"skb_maybe_pull_tail\(skb,\s*off\s*\+\s*sizeof\(struct "
                + header + r"\),\s*off\s*\+\s*sizeof\(struct " + header + r"\)\)"
            )
            self.assertRegex(body, pattern)
        self.assertEqual(704 - 700, 4)
        self.assertLess(256 - 700, 0)

    def test_udp_header_pointer_is_acquired_after_head_may_move(self):
        body = function("net/ipv4/udp_offload.c", "__skb_udp_tunnel_segment")
        self.assertLess(body.index("pskb_may_pull(skb, tnl_hlen)"),
                        body.index("uh = udp_hdr(skb);"))
        self.assertNotIn("struct udphdr *uh = udp_hdr(skb);", body)


if __name__ == "__main__":
    unittest.main()
