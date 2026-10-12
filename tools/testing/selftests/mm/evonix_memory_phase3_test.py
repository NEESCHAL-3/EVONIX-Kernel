#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extracted-C decision tests and source contracts, not kernel execution.

The harness substitutes compression, locking, allocation and device I/O.
It checks control flow; it does not measure compression, battery or latency.
"""

import ctypes
import itertools
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
BASELINE = "f1e55d1a493fd74ed837b511205aeec567c1f15f"
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "evonix"))
from stable_backports_test import function  # noqa: E402


def source(path, baseline=False):
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{BASELINE}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def body(text, name):
    # Locate a definition, not a call; ignore braces in comments.
    code = re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: " " * len(m[0]),
                  text, flags=re.S)
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", code)
    if not match:
        raise AssertionError(f"Definition missing: {name}")
    start = code.index("{", match.start())
    depth = 1
    for end in range(start + 1, len(code)):
        depth += (code[end] == "{") - (code[end] == "}")
        if not depth:
            return text[start:end + 1]
    raise AssertionError(f"Unterminated definition: {name}")


COMMON = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
typedef uint32_t u32;
#define unlikely(x) (x)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define abs(x) ((int)(x) < 0 ? -(int)(x) : (int)(x))
"""

RECLAIM = r"""
#define CONFIG_ANDROID_VENDOR_OEM_DATA 1
#define NUMA_BALANCING_MEMORY_TIERING 1
enum zone_watermarks { WMARK_HIGH, WMARK_PROMO };
struct zone { bool managed, safe; };
struct pglist_data { struct zone node_zones[3]; };
struct lruvec { struct pglist_data *pgdat; };
struct scan_control {
    bool proactive, root;
    unsigned long nr_reclaimed, nr_to_reclaim, android_vendor_data1;
    int order, reclaim_idx;
};
static int pending, kswapd, bypass_hook, bypass_order, hooks;
static int sysctl_numa_balancing_mode;
#define current 0
#define signal_pending(task) ((void)(task), pending)
#define root_reclaim(sc) ((sc)->root)
#define current_is_kswapd() (kswapd)
#define compact_gap(order) (2UL << (order))
#define MIN_LRU_BATCH 4
#define lruvec_pgdat(lruvec) ((lruvec)->pgdat)
#define wmark_pages(zone,mark) ((void)(zone), (void)(mark), 10UL)
#define managed_zone(zone) ((zone)->managed)
#define zone_watermark_ok(zone,order,size,idx,flags) \
    ((void)(order), (void)(size), (void)(idx), (void)(flags), (zone)->safe)
#define trace_android_vh_mglru_should_abort_scan(data,answer) \
    ((void)(data), hooks++, *(answer) = bypass_hook)
#define trace_android_vh_mglru_should_abort_scan_order(order,answer) \
    ((void)(order), hooks++, *(answer) = bypass_order)
"""

RECLAIM_PROBE = r"""
int reclaim_probe(int proactive, int root, int signal, int worker, int order,
                  int safe, int target_met, int bypass, int order_bypass)
{
    struct pglist_data pgdat = {.node_zones = {{true, safe}, {true, safe}, {true, safe}}};
    struct lruvec lruvec = {.pgdat = &pgdat};
    struct scan_control sc = {.proactive = proactive, .root = root,
        .nr_reclaimed = target_met ? 100 : 0, .nr_to_reclaim = 32,
        .order = order, .reclaim_idx = 2};
    pending = signal; kswapd = worker; bypass_hook = bypass;
    bypass_order = order_bypass; hooks = 0;
    return should_abort_scan(&lruvec, &sc);
}
int hook_count(void) { return hooks; }
"""

ZRAM = r"""
#define ZRAM_IDLE 1
#define ZRAM_INCOMPRESSIBLE 2
#define ZRAM_WB 4
#define ZRAM_UNDER_WB 8
#define ZRAM_SAME 16
#define ZRAM_HUGE 32
#define ZRAM_SECONDARY_COMP 1
#define ZRAM_MAX_COMPS 4
#define GFP_KERNEL 0
#define min(a,b) ((a) < (b) ? (a) : (b))
#define __GFP_KSWAPD_RECLAIM 1
#define __GFP_NOWARN 2
#define __GFP_HIGHMEM 4
#define __GFP_MOVABLE 8
#define ZS_MM_WO 0
#define IS_ERR_VALUE(x) ((unsigned long)(x) >= (unsigned long)-4095)
#define PTR_ERR(x) ((long)(x))
struct page { char data[4096]; };
struct zcomp_strm { char buffer[4096]; int priority; };
struct zram {
    unsigned long handle;
    unsigned int size, priority, flags, num_active_comps;
    struct zcomp_strm *comps[4];
    char *comp_algs[4];
    unsigned long disksize;
    int init_lock;
    bool initialized;
    int mem_pool;
    struct { long compr_data_size, pages_stored; } stats;
};
static int reads, compresses, gets, puts, allocations, fault;
static unsigned int compressed_size;
static char mapped[4096];
#define zram_get_handle(z,i) ((void)(i), (z)->handle)
#define zram_get_obj_size(z,i) ((void)(i), (z)->size)
#define zram_get_priority(z,i) ((void)(i), (z)->priority)
#define zram_clear_flag(z,i,f) ((void)(i), (z)->flags &= ~(f))
#define zram_set_flag(z,i,f) ((void)(i), (z)->flags |= (f))
#define zram_set_handle(z,i,h) ((void)(i), (z)->handle = (h))
#define zram_set_obj_size(z,i,s) ((void)(i), (z)->size = (s))
#define zram_set_priority(z,i,p) ((void)(i), (z)->priority = (p))
#define zram_free_page(z,i) ((void)(i), (z)->handle = 0)
#define zram_allocated(z,i) ((void)(i), !!(z)->handle)
#define zram_test_flag(z,i,f) ((void)(i), !!((z)->flags & (f)))
#define atomic64_add(n,p) (*(p) += (n))
#define atomic64_inc(p) (++*(p))
#define kmap_atomic(page) ((page)->data)
#define kunmap_atomic(src) ((void)(src))
static int zram_read_from_zspool(struct zram *z, struct page *p, u32 i)
{ (void)z; (void)p; (void)i; reads++; return fault == 1 ? -EIO : 0; }
static unsigned int zs_lookup_class_index(int pool, unsigned int size)
{ (void)pool; return (size + 63) / 64; }
static struct zcomp_strm *zcomp_stream_get(struct zcomp_strm *s)
{ gets++; return s; }
static void zcomp_stream_put(struct zcomp_strm *s) { (void)s; puts++; }
static int zcomp_compress(struct zcomp_strm *s, void *src, unsigned int *size)
{ (void)s; (void)src; compresses++; *size = compressed_size; return fault == 2 ? -EIO : 0; }
static unsigned long zs_malloc(int pool, unsigned int size, int gfp)
{ (void)pool; (void)size; (void)gfp; allocations++; return fault == 3 ? (unsigned long)-ENOMEM : 2; }
static void *zs_map_object(int pool, unsigned long handle, int mode)
{ (void)pool; (void)handle; (void)mode; return mapped; }
static void zs_unmap_object(int pool, unsigned long handle)
{ (void)pool; (void)handle; }
"""

ZRAM_PROBE = r"""
void recompress_probe(unsigned int priority, unsigned int mask,
    unsigned int first, unsigned int end, unsigned int old_size,
    unsigned int threshold, unsigned int new_size, int error, long *out)
{
    struct zcomp_strm streams[4] = {0};
    struct page page = {0};
    struct zram z = {.handle = old_size ? 1 : 0, .size = old_size,
        .priority = priority, .flags = ZRAM_IDLE, .num_active_comps = 1};
    for (unsigned int p = 1; p < 4; p++) {
        if (mask & (1U << p)) {
            z.comps[p] = &streams[p]; z.num_active_comps++;
        }
    }
    reads = compresses = gets = puts = allocations = 0;
    fault = error; compressed_size = new_size;
    out[0] = zram_recompress(&z, 0, &page, threshold, first, end);
    out[1] = reads; out[2] = compresses; out[3] = gets; out[4] = puts;
    out[5] = z.handle; out[6] = z.priority; out[7] = !!(z.flags & ZRAM_IDLE);
    out[8] = !!(z.flags & ZRAM_INCOMPRESSIBLE); out[9] = allocations;
    out[10] = z.size;
}
static char *skip_spaces(const char *s)
{ while (isspace((unsigned char)*s)) s++; return (char *)s; }
static char *next_arg(char *s, char **param, char **val)
{
    char *end = s;
    while (*end && !isspace((unsigned char)*end)) end++;
    if (*end) *end++ = 0;
    *param = s; *val = strchr(s, '=');
    if (*val) *(*val)++ = 0;
    return skip_spaces(end);
}
static int kstrtouint(const char *s, int base, unsigned int *value)
{
    char *end;
    if (*s == '-') return -EINVAL;
    errno = 0;
    unsigned long n = strtoul(s, &end, base);
    if (errno || n > UINT_MAX) return -ERANGE;
    if (end == s || *end) return -EINVAL;
    *value = n; return 0;
}
"""

STORE = r"""
#define PAGE_SHIFT 12
static unsigned int huge_class_size = 4096;
struct device { struct zram *zram; };
struct device_attribute { int unused; };
#define dev_to_zram(dev) ((dev)->zram)
#define init_done(z) ((z)->initialized)
static struct zram *watched;
static unsigned long replacement_pages;
static int slot_locks, out_of_bounds, scratch_allocs, scratch_error, unlocks;
static struct page scratch;
static void down_read(int *lock)
{
    (void)lock;
    /* Deterministic reset/reinitialize before acquiring the simulated lock. */
    watched->disksize = replacement_pages << PAGE_SHIFT;
}
static void up_read(int *lock) { (void)lock; unlocks++; }
static struct page *alloc_page(int gfp)
{ (void)gfp; scratch_allocs++; return scratch_error ? NULL : &scratch; }
static void __free_page(struct page *page) { (void)page; }
static void zram_slot_lock(struct zram *z, unsigned long index)
{ (void)z; slot_locks++; if (index >= replacement_pages) out_of_bounds++; }
static void zram_slot_unlock(struct zram *z, unsigned long index)
{ (void)z; (void)index; }
static void cond_resched(void) { }
"""

STORE_PROBE = r"""
void store_probe(char *command, unsigned long old_pages, unsigned long new_pages,
                 unsigned int active_comps, int initialized, int fail_alloc, long *out)
{
    struct zcomp_strm comp = {0};
    struct zram z = {.handle = 1, .size = 3000, .priority = 1, .flags = ZRAM_IDLE,
        .num_active_comps = active_comps, .initialized = initialized,
        .disksize = old_pages << PAGE_SHIFT, .comps = {NULL, &comp},
        .comp_algs = {NULL, "zstd"}};
    struct device dev = {.zram = &z};
    watched = &z; replacement_pages = new_pages;
    slot_locks = out_of_bounds = scratch_allocs = unlocks = 0;
    scratch_error = fail_alloc;
    out[0] = recompress_store(&dev, NULL, command, strlen(command));
    out[1] = slot_locks; out[2] = out_of_bounds;
    out[3] = scratch_allocs; out[4] = unlocks;
    watched = NULL;
}
"""

DAMON = r"""
struct damon_region {
    struct { unsigned long start, end; } ar;
    unsigned int nr_accesses, last_nr_accesses, age;
    struct damon_region *next;
};
struct damon_target { struct damon_region *regions; struct damon_target *next; };
struct damon_ctx {
    struct { unsigned int aggr_interval, sample_interval, max_nr_regions; } attrs;
    struct damon_target *targets;
};
static int merge_calls, bounded_stop;
static struct damon_ctx *active_ctx;
#define damon_for_each_target(t,c) for ((t) = (c)->targets; (t); (t) = (t)->next)
#define damon_for_each_region_safe(r,n,t) \
    for ((r) = (t)->regions; (r) && ((n) = (r)->next, true); (r) = (n))
#define damon_sz_region(r) ((r)->ar.end - (r)->ar.start)
static void damon_merge_two_regions(struct damon_target *t,
    struct damon_region *l, struct damon_region *r)
{ (void)t; l->ar.end = r->ar.end; l->next = r->next; }
static unsigned int damon_nr_regions(struct damon_target *t)
{
    unsigned int nr = 0;
    for (struct damon_region *r = t->regions; r; r = r->next) nr++;
    if (++merge_calls > 256) {
        bounded_stop = 1; active_ctx->attrs.max_nr_regions = UINT_MAX;
    }
    return nr;
}
"""

DAMON_PROBE = r"""
void merge_probe(unsigned int threshold, unsigned int max_threshold,
    unsigned int sample, long *out)
{
    struct damon_region b = {.ar = {100, 110}, .age = 10};
    struct damon_region a = {.ar = {0, 10}, .age = 10, .next = &b};
    struct damon_target target = {.regions = &a};
    struct damon_ctx ctx = {.attrs = {max_threshold, sample, 1}, .targets = &target};
    active_ctx = &ctx; merge_calls = bounded_stop = 0;
    kdamond_merge_regions(&ctx, threshold, 4096);
    active_ctx = NULL;
    out[0] = merge_calls; out[1] = bounded_stop; out[2] = a.age; out[3] = b.age;
}
"""


def harness(baseline=False):
    vmscan = source("mm/vmscan.c", baseline)
    zram = source("drivers/block/zram/zram_drv.c", baseline)
    damon = source("mm/damon/core.c", baseline)
    selector = body(zram, "recompress_store")
    selector = selector[selector.index("args = skip_spaces"):selector.index("if (threshold >=")]
    # Use the real parser decision block; tokenization is a harness substitute.
    parser = ("int selector_probe(char *buf) { char *args, *param, *val, *algo = NULL; "
              "u32 mode = 0, threshold = 0; long ret;\n" + selector +
              "(void)algo; return (int)mode; }\n")
    age_sig = ("static void damon_merge_regions_of(struct damon_target *t, "
               "unsigned int thres, unsigned long sz_limit" +
               ("" if baseline else ", bool count_age") + ")\n")
    return (COMMON + RECLAIM +
            "static bool should_abort_scan(struct lruvec *lruvec, struct scan_control *sc)\n" +
            body(vmscan, "should_abort_scan") + RECLAIM_PROBE + ZRAM +
            "static int zram_recompress(struct zram *zram, u32 index, struct page *page, "
            "u32 threshold, u32 prio, u32 prio_max)\n" + body(zram, "zram_recompress") +
            ZRAM_PROBE + "\n#define RECOMPRESS_IDLE 1\n#define RECOMPRESS_HUGE 2\n" + parser +
            STORE + "static long recompress_store(struct device *dev, struct device_attribute *attr, "
            "const char *buf, size_t len)\n" + body(zram, "recompress_store") + STORE_PROBE +
            DAMON + age_sig + body(damon, "damon_merge_regions_of") +
            "static void kdamond_merge_regions(struct damon_ctx *c, unsigned int threshold, "
            "unsigned long sz_limit)\n" + body(damon, "kdamond_merge_regions") + DAMON_PROBE +
            "unsigned int swap_probe(unsigned long prev_offset, unsigned long offset, "
            "int hits, int max_pages, int prev_win)\n" +
            body(source("mm/swap_state.c", baseline), "__swapin_nr_pages"))


class MemoryDecisions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise RuntimeError("A host C compiler is required for these tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-mm-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.libs = []
        for old in (False, True):
            path = Path(cls.temp.name) / f"memory-{int(old)}.c"
            path.write_text(harness(old))
            so = path.with_suffix(".so")
            subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-sign-compare", "-Wno-unused-parameter", "-shared", "-fPIC",
                            str(path), "-o", str(so)], check=True)
            lib = ctypes.CDLL(str(so))
            lib.reclaim_probe.argtypes = [ctypes.c_int] * 9
            lib.recompress_probe.argtypes = [ctypes.c_uint] * 7 + [ctypes.c_int, ctypes.POINTER(ctypes.c_long)]
            lib.selector_probe.argtypes = [ctypes.c_char_p]
            lib.store_probe.argtypes = [ctypes.c_char_p, ctypes.c_ulong, ctypes.c_ulong,
                                       ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_long)]
            lib.merge_probe.argtypes = [ctypes.c_uint] * 3 + [ctypes.POINTER(ctypes.c_long)]
            lib.swap_probe.argtypes = [ctypes.c_ulong, ctypes.c_ulong] + [ctypes.c_int] * 3
            lib.swap_probe.restype = ctypes.c_uint
            cls.libs.append(lib)
        cls.new, cls.old = cls.libs

    def recompress(self, lib=None, priority=0, mask=2, first=1, end=4,
                   size=3000, threshold=0, compressed=1000, error=0):
        out = (ctypes.c_long * 11)()
        (lib or self.new).recompress_probe(priority, mask, first, end, size,
                                         threshold, compressed, error, out)
        return list(out)

    def merge(self, threshold=1, maximum=64, sample=1, lib=None):
        out = (ctypes.c_long * 4)()
        (lib or self.new).merge_probe(threshold, maximum, sample, out)
        return list(out)

    def store(self, command="type=idle", old=128, new=3, active=2,
              initialized=1, fail_alloc=0, lib=None):
        out = (ctypes.c_long * 5)()
        (lib or self.new).store_probe(ctypes.create_string_buffer(command.encode()),
                                     old, new, active, initialized, fail_alloc, out)
        return list(out)

    def test_actual_recompression_walk_uses_replacement_table_size(self):
        self.assertEqual(self.store()[1:3], [3, 0])
        self.assertEqual(self.store(lib=self.old)[1:3], [128, 125])

    def test_recompression_bounds_cover_both_shrink_and_grow(self):
        for old, new in itertools.product((0, 1, 3, 128), (1, 3, 128)):
            result = self.store(old=old, new=new)
            self.assertEqual(result[1:3], [new, 0])
            self.assertEqual(result[4], 1)

    def test_single_compressor_skips_allocation_even_under_memory_pressure(self):
        self.assertEqual(self.store(active=1, fail_alloc=1), [9, 0, 0, 0, 1])
        self.assertEqual(self.store(active=1, fail_alloc=1, lib=self.old)[0], -12)

    def test_uninitialized_recompression_releases_lock_without_scanning(self):
        self.assertEqual(self.store(initialized=0), [-22, 0, 0, 0, 1])

    def test_invalid_selected_algorithm_is_not_hidden_by_no_work_fast_path(self):
        self.assertEqual(self.store(command="algo=missing", active=1), [-22, 0, 0, 0, 1])

    def test_scratch_allocation_failure_does_not_enter_page_scan(self):
        self.assertEqual(self.store(fail_alloc=1), [-12, 0, 0, 1, 1])

    def test_proactive_signals_cancel_root_and_targeted_reclaim(self):
        for root, worker, order, bypass in itertools.product((0, 1), (0, 1), (0, 2), (0, 1)):
            self.assertEqual(self.new.reclaim_probe(1, root, 1, worker, order, 0, 0, bypass, 0), 1)

    def test_baseline_misses_targeted_proactive_cancellation(self):
        args = (1, 0, 1, 0, 0, 0, 0, 0, 0)
        self.assertEqual(self.old.reclaim_probe(*args), 0)
        self.assertEqual(self.new.reclaim_probe(*args), 1)

    def test_reactive_reclaim_is_unchanged_including_vendor_bypass(self):
        for root, signal, worker, order, safe, met, bypass, order_bypass in itertools.product(
                (0, 1), (0, 1), (0, 1), (0, 2), (0, 1), (0, 1), (0, 1), (0, 1)):
            args = (0, root, signal, worker, order, safe, met, bypass, order_bypass)
            self.assertEqual(self.new.reclaim_probe(*args), self.old.reclaim_probe(*args))

    def test_no_signal_proactive_policy_is_unchanged(self):
        for root, worker, order, safe, met, bypass in itertools.product(
                (0, 1), (0, 1), (0, 2), (0, 1), (0, 1), (0, 1)):
            args = (1, root, 0, worker, order, safe, met, bypass, 0)
            self.assertEqual(self.new.reclaim_probe(*args), self.old.reclaim_probe(*args))

    def test_abort_hook_runs_before_cancellation(self):
        self.new.reclaim_probe(1, 0, 1, 0, 0, 0, 0, 0, 0)
        self.assertEqual(self.new.hook_count(), 1)

    def test_no_eligible_compressor_keeps_idle_and_skips_decompression(self):
        for priority, mask, first, end in itertools.product(range(4), range(0, 16, 2), range(1, 4), range(1, 5)):
            if first > end:
                continue
            eligible = any(mask & (1 << p) and p > priority for p in range(first, end))
            got = self.recompress(priority=priority, mask=mask, first=first, end=end)
            if not eligible:
                self.assertEqual(got[0:3], [0, 0, 0])
                self.assertEqual(got[5:8], [1, priority, 1])
                self.assertEqual(got[9], 0)
            else:
                self.assertEqual(got[1:3], [1, 1])
                self.assertEqual(got[3], got[4])

    def test_baseline_decompresses_already_highest_priority_page(self):
        old = self.recompress(lib=self.old, priority=1)
        new = self.recompress(priority=1)
        self.assertEqual((old[1], old[7]), (1, 0))
        self.assertEqual((new[1], new[7]), (0, 1))

    def test_successful_recompression_keeps_existing_commit_path(self):
        old, new = self.recompress(lib=self.old), self.recompress()
        self.assertEqual(new, old)
        self.assertEqual(new[5:8], [2, 1, 0])
        self.assertEqual(new[10], 1000)

    def test_threshold_below_old_size_skips_all_work(self):
        got = self.recompress(size=999, threshold=1000)
        self.assertEqual(got[0:3], [0, 0, 0])
        self.assertEqual(got[7], 1)

    def test_missing_handle_fails_without_decompression(self):
        got = self.recompress(size=0)
        self.assertEqual(got[0], -22)
        self.assertEqual(got[1], 0)

    def test_read_failure_preserves_original_page(self):
        got = self.recompress(error=1)
        self.assertEqual(got[0], -5)
        self.assertEqual(got[2], 0)
        self.assertEqual(got[5:8], [1, 0, 1])

    def test_compressor_failure_releases_stream_and_preserves_handle(self):
        got = self.recompress(error=2)
        self.assertEqual(got[0], -5)
        self.assertEqual(got[3:5], [1, 1])
        self.assertEqual(got[5], 1)

    def test_allocation_failure_preserves_existing_compressed_object(self):
        got = self.recompress(error=3)
        self.assertEqual(got[0], -12)
        self.assertEqual(got[3], got[4])
        self.assertEqual(got[5:7], [1, 0])
        self.assertEqual(got[10], 3000)

    def test_same_size_class_does_not_allocate(self):
        got = self.recompress(compressed=2990)
        self.assertEqual(got[5], 1)
        self.assertEqual(got[9], 0)
        self.assertEqual(got[8], 1)

    def test_filtered_priority_does_not_mark_all_algorithms_incompressible(self):
        got = self.recompress(mask=14, first=2, end=3, compressed=3000)
        self.assertEqual(got[8], 0)

    def test_threshold_failure_does_not_replace_object(self):
        got = self.recompress(threshold=1000, compressed=2000)
        self.assertEqual(got[5], 1)
        self.assertEqual(got[9], 0)
        self.assertEqual(got[3], got[4])

    def test_valid_selectors_and_omitted_type(self):
        for command, mode in (("type=idle", 1), ("type=huge", 2), ("type=huge_idle", 3),
                              ("type=all", 0), ("threshold=3000", 0), ("algo=zstd", 0),
                              ("type=idle threshold=2000 algo=zstd", 1)):
            self.assertEqual(self.new.selector_probe(ctypes.create_string_buffer(command.encode())), mode)

    def test_unknown_selectors_fail_instead_of_full_scan(self):
        for command in ("type=idlle", "types=idle", "thresholds=3000", "unknown=1"):
            self.assertEqual(self.old.selector_probe(ctypes.create_string_buffer(command.encode())), 0)
            self.assertEqual(self.new.selector_probe(ctypes.create_string_buffer(command.encode())), -22)

    def test_invalid_values_fail_before_scanning(self):
        for command in ("type", "type=", "threshold=-1", "threshold=abc", "threshold=4294967296"):
            self.assertLess(self.new.selector_probe(ctypes.create_string_buffer(command.encode())), 0)

    def test_repeated_damon_merges_age_each_region_once(self):
        new, old = self.merge(), self.merge(lib=self.old)
        self.assertEqual(new[1], 0)
        self.assertEqual(new[2:], [11, 11])
        self.assertGreater(old[2], 11)

    def test_damon_overflow_case_terminates_without_harness_watchdog(self):
        new = self.merge(maximum=2**32 - 1)
        old = self.merge(maximum=2**32 - 1, lib=self.old)
        self.assertEqual(new[1], 0)
        self.assertLess(new[0], 40)
        self.assertEqual(old[1], 1)

    def test_damon_extreme_thresholds_and_zero_sample_are_bounded(self):
        for threshold, maximum, sample in itertools.product(
                (0, 1, 31, 2**31, 2**32 - 1), (0, 1, 63, 2**31, 2**32 - 1), (0, 1, 100)):
            result = self.merge(threshold, maximum, sample)
            self.assertEqual(result[1], 0)
            self.assertLess(result[0], 40)
            self.assertEqual(result[2:], [11, 11])

    def test_swap_window_always_obeys_current_ceiling(self):
        for hits, maximum, previous, offset in itertools.product(
                range(32), (1, 2, 4, 8, 16, 32), (1, 2, 4, 8, 16, 32), (98, 99, 100, 101, 500)):
            window = self.new.swap_probe(100, offset, hits, maximum, previous)
            self.assertGreaterEqual(window, 1)
            self.assertLessEqual(window, maximum)

    def test_swap_steady_state_hysteresis_is_unchanged(self):
        for hits, maximum, previous, offset in itertools.product(
                range(32), (1, 2, 4, 8, 16, 32), (1, 2, 4, 8, 16, 32), (99, 101, 500)):
            if previous <= maximum:
                args = (100, offset, hits, maximum, previous)
                self.assertEqual(self.new.swap_probe(*args), self.old.swap_probe(*args))

    def test_baseline_history_exceeds_reduced_swap_ceiling(self):
        self.assertEqual(self.old.swap_probe(100, 500, 0, 2, 32), 16)
        self.assertEqual(self.new.swap_probe(100, 500, 0, 2, 32), 2)


class MemoryContracts(unittest.TestCase):
    def test_zram_walk_bounds_are_read_after_init_lock_and_check(self):
        for name in ("writeback_store", "recompress_store", "read_block_state"):
            code = function("drivers/block/zram/zram_drv.c", name)
            size = code.index("nr_pages = zram->disksize >> PAGE_SHIFT;")
            self.assertLess(code.index("down_read(&zram->init_lock)"), size)
            self.assertLess(code.index("if (!init_done(zram))"), size)
            self.assertLess(size, code.index("zram_slot_lock(zram, index)"))

    def test_targeted_writeback_validation_uses_locked_size(self):
        code = function("drivers/block/zram/zram_drv.c", "writeback_store")
        self.assertIn("kstrtoul(buf + sizeof(PAGE_WB_SIG) - 1, 10, &index)", code)
        self.assertLess(code.index("nr_pages = zram->disksize"), code.index("if (index >= nr_pages)"))
        self.assertLess(code.index("if (index >= nr_pages)"), code.index("alloc_page(GFP_KERNEL)"))

    def test_no_secondary_compressor_skips_scratch_allocation_and_scan(self):
        code = function("drivers/block/zram/zram_drv.c", "recompress_store")
        start = code.index("if (zram->num_active_comps < 2)")
        self.assertLess(start, code.index("page = alloc_page(GFP_KERNEL)"))
        self.assertIn("ret = len;", code[start:code.index("page = alloc_page(GFP_KERNEL)")])
        self.assertIn("goto release_init_lock;", code[start:code.index("page = alloc_page(GFP_KERNEL)")])
        self.assertLess(code.index("if (algo)"), start)

    def test_memory_reclaim_uses_restartable_error(self):
        code = function("mm/memcontrol.c", "memory_reclaim")
        self.assertIn("if (signal_pending(current))\n\t\t\treturn -ERESTARTSYS;", code)
        self.assertIn("MEMCG_RECLAIM_MAY_SWAP | MEMCG_RECLAIM_PROACTIVE", code)

    def test_tasks_rcu_progress_for_all_relevant_long_loops(self):
        for path, name in (("mm/vmscan.c", "try_to_shrink_lruvec"), ("mm/vmscan.c", "shrink_lruvec"),
                           ("mm/migrate.c", "migrate_pages_batch"),
                           ("mm/backing-dev.c", "cleanup_offline_cgwbs_workfn")):
            self.assertIn("cond_resched_tasks_rcu_qs();", function(path, name))

    def test_writeback_reports_quiescence_even_on_one_pass(self):
        code = function("mm/backing-dev.c", "cleanup_offline_cgwbs_workfn")
        self.assertIn("do {\n\t\t\tcond_resched_tasks_rcu_qs();\n\t\t} while (cleanup_offline_cgwb(wb));", code)
        self.assertLess(code.index("spin_unlock_irq(&cgwb_lock)"), code.index("cond_resched_tasks_rcu_qs"))

    def test_zsmalloc_unlock_order_matches_lock_nesting(self):
        code = function("mm/zsmalloc.c", "zs_page_migrate")
        self.assertLess(code.index("spin_lock(&pool->lock)"), code.index("migrate_write_lock(zspage)"))
        self.assertLess(code.index("migrate_write_unlock(zspage)"), code.index("spin_unlock(&pool->lock)"))
        self.assertLess(code.index("dec_zspage_isolation(zspage)"), code.index("migrate_write_unlock(zspage)"))

    def test_damon_sampling_has_no_cross_context_static_cache(self):
        code = function("mm/damon/vaddr.c", "__damon_va_check_access")
        self.assertNotIn("static ", code)
        self.assertIn("damon_va_young(mm, r->sampling_addr)", code)
        self.assertIn("damon_update_region_access_rate(r, false, attrs)", code)
        self.assertNotIn("folio_sz", source("mm/damon/vaddr.c"))

    def test_damon_kunit_caller_matches_private_helper(self):
        self.assertIn("damon_merge_regions_of(t, 9, 9999, true);", source("mm/damon/core-test.h"))

    def test_already_present_android_psi_correction_is_retained(self):
        code = function("mm/memcontrol.c", "mem_cgroup_handle_over_high")
        self.assertIn("bool record_psi = false;", code)
        self.assertIn("trace_android_vh_mem_cgroup_handle_over_high(&record_psi)", code)
        self.assertIn("timer_shutdown_sync(&cgroup->psi->rtpoll_timer)",
                      function("kernel/sched/psi.c", "psi_cgroup_free"))

    def test_all_existing_vendor_hooks_in_changed_files_are_retained(self):
        for path in ("mm/vmscan.c", "mm/memcontrol.c", "mm/migrate.c", "mm/zsmalloc.c",
                     "mm/backing-dev.c", "mm/swap_state.c", "drivers/block/zram/zram_drv.c"):
            hooks = lambda text: re.findall(r"\btrace_android_\w+\s*\(", text)
            self.assertEqual(hooks(source(path)), hooks(source(path, True)))

    def test_allocation_cache_reclaim_and_hardware_policies_are_unchanged(self):
        for path in ("mm/page_alloc.c", "mm/slub.c", "mm/compaction.c", "mm/cma.c",
                     "mm/readahead.c", "mm/workingset.c", "mm/page-writeback.c",
                     "arch/arm64/configs/gki_defconfig", "drivers/block/zram/zcomp.c",
                     "include/linux/zsmalloc.h", "include/linux/damon.h", "kernel/sched/psi.c"):
            self.assertEqual(source(path), source(path, True))

    def test_already_present_zram_io_fixes_are_retained(self):
        self.assertIn("zram_read_page(zram, page, index, NULL)",
                      function("drivers/block/zram/zram_drv.c", "zram_bvec_write_partial"))
        self.assertIn("goto end_bio;", function("drivers/block/zram/zram_drv.c", "zram_bio_discard"))
        self.assertNotIn("ZRAM_ANDROID_IOC_PROCESS_PREFETCH", source("drivers/block/zram/zram_drv.c"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
