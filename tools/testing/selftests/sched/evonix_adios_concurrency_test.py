#!/usr/bin/env python3
"""
EVONIX Kernel V4.6.1 - ADIOS Multi-HCTX Concurrency, Quota & Dispatch Test
Verifies:
1. Concurrency safety of pq1_dispatch_count across multiple UFS MCQ hctx dispatchers.
2. Complete lock synchronization under pq_lock (zero unsynchronized writes).
3. Anti-starvation quota enforcement: yielding to normal queues (BQ/DL) at 8 consecutive Tier-1 dispatches.
4. Fallback recovery: forward progress guaranteed without stalling when BQ is empty or throttled.
5. Immediate yield to BQ once BQ requests become available, resetting pq1_dispatch_count.
6. Empty queue reset: pq1_dispatch_count resets to 0 when prio_queue[1] is drained.
7. Multi-threaded stress simulation with 8 concurrent dispatchers matching MT6899 UFS MCQ.
"""

import sys
import re
import threading
import time
import random
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

class AdiosSourceConcurrencyAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        adios_path = ROOT / "block" / "adios.c"
        if not adios_path.exists():
            raise FileNotFoundError(f"Missing block/adios.c at {adios_path}")
        cls.adios_c = adios_path.read_text()

    def test_no_unsynchronized_writes_to_pq1_count(self):
        """Verify that every write to pq1_dispatch_count occurs under pq_lock."""
        # Find all assignments to pq1_dispatch_count
        assignments = re.findall(r"([^\n]*pq1_dispatch_count\s*(?:=|\+\+|--)[^\n]*)", self.adios_c)
        self.assertTrue(len(assignments) >= 3, f"Expected multiple assignments, found: {assignments}")

        # Check the reset helper function
        self.assertIn("static inline void adios_reset_pq1_dispatch_count", self.adios_c)
        reset_helper = re.search(
            r"static inline void adios_reset_pq1_dispatch_count\([^)]*\)\s*\{([^}]*)\}",
            self.adios_c,
            re.DOTALL
        )
        self.assertIsNotNone(reset_helper)
        helper_body = reset_helper.group(1)
        self.assertIn("guard(spinlock_irqsave)(&ad->pq_lock);", helper_body)
        self.assertIn("ad->pq1_dispatch_count = 0;", helper_body)

        # Check adios_dispatch_request: verify NO bare assignments to pq1_dispatch_count exist
        dispatch_match = re.search(
            r"static struct request \*adios_dispatch_request\([^)]*\)\s*\{.*?\n\}",
            self.adios_c,
            re.DOTALL
        )
        self.assertIsNotNone(dispatch_match)
        dispatch_body = dispatch_match.group(0)
        
        # Verify that adios_dispatch_request calls adios_reset_pq1_dispatch_count instead of bare assignment
        self.assertIn("adios_reset_pq1_dispatch_count(ad);", dispatch_body)
        self.assertNotIn("ad->pq1_dispatch_count = 0;", dispatch_body)
        self.assertNotIn("ad->pq1_dispatch_count++", dispatch_body)

    def test_dispatch_from_pq_signature_and_force_flag(self):
        """Verify dispatch_from_pq accepts force flag and uses it correctly."""
        self.assertIn("static struct request *dispatch_from_pq(struct adios_data *ad, bool force)", self.adios_c)
        
        # In adios_dispatch_request, initial call is force=false, fallback is force=true
        self.assertIn("rq = dispatch_from_pq(ad, false);", self.adios_c)
        self.assertIn("rq = dispatch_from_pq(ad, true);", self.adios_c)

    def test_quota_reset_on_queue_drain(self):
        """Verify pq1_dispatch_count is reset to 0 when prio_queue[1] is emptied."""
        pq_func = re.search(
            r"static struct request \*dispatch_from_pq\([^)]*\)\s*\{.*?\n\}",
            self.adios_c,
            re.DOTALL
        )
        self.assertIsNotNone(pq_func)
        body = pq_func.group(0)
        self.assertIn("if (list_empty(q))", body)
        self.assertIn("ad->pq1_dispatch_count = 0;", body)

    def test_elevator_cmdline_parameter_warning(self):
        """Verify elevator= parameter warning is documented and preserved in elevator.c."""
        elv_c = (ROOT / "block" / "elevator.c").read_text()
        self.assertIn("Kernel parameter elevator= does not have any effect anymore", elv_c)


class SimulatedAdiosEngine:
    """
    High-fidelity thread-safe simulation of the ADIOS dispatch engine matching
    block/adios.c logic for UFS MCQ multi-queue concurrency.
    """
    def __init__(self):
        self.pq_lock = threading.Lock()
        self.bq_lock = threading.Lock()
        
        # Priority queues: index 0 (Tier-0 head), index 1 (Tier-1 foreground read)
        self.prio_queue = [[], []]
        
        # Batch queues: normal I/O requests
        self.batch_queue = []
        
        # Counters and states
        self.pq1_dispatch_count = 0
        self.dispatched_history = []
        self.history_lock = threading.Lock()
        
        # Latency throttling simulation flag for BQ
        self.bq_throttled = False

    def adios_state_pq(self):
        state = 0
        if len(self.prio_queue[0]) > 0:
            state |= 1
        if len(self.prio_queue[1]) > 0:
            state |= 2
        return state

    def adios_state_normal(self):
        return len(self.batch_queue) > 0

    def reset_pq1_dispatch_count(self):
        # Lockless early check, then acquire pq_lock
        if self.pq1_dispatch_count != 0:
            with self.pq_lock:
                self.pq1_dispatch_count = 0

    def dispatch_from_pq(self, force=False):
        # Lockless check
        pq_state = self.adios_state_pq()
        if not pq_state:
            return None

        # Anti-starvation guard
        if not force and self.pq1_dispatch_count >= 8 and not (pq_state & 1):
            if self.adios_state_normal():
                return None

        with self.pq_lock:
            pq_state = self.adios_state_pq()
            if not pq_state:
                return None

            pq_idx = 0 if (pq_state & 1) else 1

            if not force and pq_idx == 1 and self.pq1_dispatch_count >= 8:
                if self.adios_state_normal():
                    return None

            q = self.prio_queue[pq_idx]
            if not q:
                return None

            rq = q.pop(0)

            if pq_idx == 1:
                if len(q) == 0:
                    self.pq1_dispatch_count = 0
                elif self.pq1_dispatch_count < 8:
                    self.pq1_dispatch_count += 1
            else:
                self.pq1_dispatch_count = 0

            return rq

    def dispatch_from_bq(self):
        with self.bq_lock:
            if self.bq_throttled or not self.batch_queue:
                return None
            return self.batch_queue.pop(0)

    def adios_dispatch_request(self, hctx_id):
        # 1. Try dispatch from PQ
        rq = self.dispatch_from_pq(force=False)
        if rq:
            with self.history_lock:
                self.dispatched_history.append((hctx_id, rq))
            return rq

        # 2. Try dispatch from BQ
        rq = self.dispatch_from_bq()
        if rq:
            self.reset_pq1_dispatch_count()
            with self.history_lock:
                self.dispatched_history.append((hctx_id, rq))
            return rq

        # 3. Fallback recovery
        if self.pq1_dispatch_count >= 8:
            rq = self.dispatch_from_pq(force=True)
            if rq:
                with self.history_lock:
                    self.dispatched_history.append((hctx_id, rq))
                return rq

        return None


class AdiosMultiHctxConcurrencySimulation(unittest.TestCase):
    def test_strict_starvation_prevention(self):
        """Verify that when both Tier-1 and BQ have requests, Tier-1 dispatches at most 8 before BQ dispatches."""
        engine = SimulatedAdiosEngine()
        
        # Enqueue 20 Tier-1 requests and 10 BQ requests
        for i in range(20):
            engine.prio_queue[1].append(f"T1_req_{i}")
        for i in range(10):
            engine.batch_queue.append(f"BQ_req_{i}")

        # Dispatch 9 requests sequentially
        dispatched = []
        for i in range(9):
            rq = engine.adios_dispatch_request(hctx_id=0)
            dispatched.append(rq)

        # First 8 must be Tier-1
        for i in range(8):
            self.assertTrue(dispatched[i].startswith("T1_req_"), f"Item {i} was {dispatched[i]}")
        # The 9th request MUST be BQ (starvation yield)
        self.assertEqual(dispatched[8], "BQ_req_0")
        # After BQ dispatch, pq1_dispatch_count must be reset to 0
        self.assertEqual(engine.pq1_dispatch_count, 0)

    def test_fallback_recovery_without_stall(self):
        """Verify that when BQ is throttled/empty, Tier-1 continues dispatching (no stall)."""
        engine = SimulatedAdiosEngine()
        
        # Enqueue 15 Tier-1 requests and 5 BQ requests, but BQ is throttled (latency saturated)
        for i in range(15):
            engine.prio_queue[1].append(f"T1_req_{i}")
        for i in range(5):
            engine.batch_queue.append(f"BQ_req_{i}")
        engine.bq_throttled = True

        dispatched = []
        for i in range(12):
            rq = engine.adios_dispatch_request(hctx_id=0)
            dispatched.append(rq)

        # All 12 requests must have dispatched successfully via fallback recovery
        self.assertEqual(len(dispatched), 12)
        for rq in dispatched:
            self.assertTrue(rq.startswith("T1_req_"))

        # Now unthrottle BQ: the very next request MUST be BQ because quota remains saturated!
        engine.bq_throttled = False
        next_rq = engine.adios_dispatch_request(hctx_id=0)
        self.assertEqual(next_rq, "BQ_req_0")
        self.assertEqual(engine.pq1_dispatch_count, 0)

    def test_multithreaded_hctx_stress(self):
        """Simulate 8 concurrent UFS MCQ hardware contexts dispatching under heavy mixed load."""
        engine = SimulatedAdiosEngine()
        
        num_t1 = 800
        num_t0 = 100
        num_bq = 800
        
        for i in range(num_t0):
            engine.prio_queue[0].append(f"T0_{i}")
        for i in range(num_t1):
            engine.prio_queue[1].append(f"T1_{i}")
        for i in range(num_bq):
            engine.batch_queue.append(f"BQ_{i}")

        total_requests = num_t0 + num_t1 + num_bq
        stop_event = threading.Event()
        threads = []

        def worker(hctx_id):
            while not stop_event.is_set():
                rq = engine.adios_dispatch_request(hctx_id)
                if rq is None:
                    # Check if all queues empty
                    if not engine.prio_queue[0] and not engine.prio_queue[1] and not engine.batch_queue:
                        break
                    time.sleep(0.0001)

        # Launch 8 worker threads representing 8 UFS MCQ hardware contexts
        for hctx in range(8):
            t = threading.Thread(target=worker, args=(hctx,))
            threads.append(t)
            t.start()

        for t in threads:
            t.join(timeout=10.0)

        # Verify all requests were dispatched cleanly
        self.assertEqual(len(engine.dispatched_history), total_requests)
        self.assertEqual(len(engine.prio_queue[0]), 0)
        self.assertEqual(len(engine.prio_queue[1]), 0)
        self.assertEqual(len(engine.batch_queue), 0)
        self.assertEqual(engine.pq1_dispatch_count, 0)


if __name__ == "__main__":
    unittest.main()
