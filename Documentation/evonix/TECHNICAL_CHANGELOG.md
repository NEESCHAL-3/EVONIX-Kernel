# EVONIX KERNEL V4.6.1 -- TECHNICAL MAINTENANCE CHANGELOG

**Target Platform:** Xiaomi POCO X7 Pro / Redmi Turbo 4 (`rodin` / `24129RT7CC`)  
**SoC:** MediaTek Dimensity 8400-Ultra (`MT6899`) -- All-Big-Core (8x Cortex-A725)  
**Kernel Baseline:** Linux 6.6.144 Android GKI (LTS)  
**Toolchain:** Android Clang r614150 (23.0.1) + LLD + ThinLTO + AutoFDO  
**Baseline Release:** EVONIX V4.6 (`b3c46ddbb8740`, 2026-11-20)  
**Release Tag:** `v4.6.1`  
**Developer:** NEESCHAL  

---

## Executive Summary

EVONIX Kernel V4.6.1 is a targeted maintenance and stability release addressing regressions identified in V4.6:
1. **SELinux Hook ABI Alignment (GitHub Issue #6):** In Linux 6.6 GKI, `security_context_to_sid_with_policy()` takes 8 arguments (`const struct selinux_policy *policy, const char *scontext, u32 scontext_len, u32 *sid, u32 def_sid, gfp_t gfp_flags, int force, int verbose`). KernelSU-Next's hook invoked it with 7 arguments, clobbering caller registers and faulting on pointer dereference at write `0x800` during app startup with `selinux_hide` enabled. Corrected prototype and argument marshalling across KernelSU-Next and BakaSU editions.
2. **ADIOS Multi-Queue Concurrency Serialization:** `adios_reset_pq1_dispatch_count()` previously performed an early `READ_ONCE()` check and reset without acquiring `pq_lock`, while `dispatch_from_pq()` updated the counter under lock. Re-architected with unconditional `spin_lock_irqsave(&q->pq_lock, flags)` linearization, preserving strict anti-starvation forward progress across concurrent UFS MCQ dispatchers.
3. **ColorOS Governor Selection Compatibility (GitHub Issue #5):** Restored native `sugov_ext` CPU governor compatibility on ColorOS ports, eliminating boot stalls caused by userspace governor policy rejections, while preserving enhanced Schedutil behavior on HyperOS.
4. **Namespace and Container Preservation:** Verified complete preservation of all Linux namespaces (`CONFIG_USER_NS`, `CONFIG_PID_NS`, `CONFIG_IPC_NS`, `CONFIG_NET_NS`, `CONFIG_UTS_NS`, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`) with zero Android KMI violations.

---

# EVONIX KERNEL V4.6 — COMPLETE HISTORICAL TECHNICAL CHANGELOG
**Target Platform:** Xiaomi POCO X7 Pro / Redmi Turbo 4 (`rodin` / `24129RT7CC`)  
**SoC:** MediaTek Dimensity 8400-Ultra (`MT6899`) — All-Big-Core (8× Cortex-A725)  
**Kernel Baseline:** Linux 6.6.144 Android GKI (LTS)  
**Toolchain:** Android Clang r614150 (23.0.1) + LLD + ThinLTO + AutoFDO  
**Baseline Release:** EVONIX V4.5 (`5ecc54f415f3f`, 2026-11-17)  
**Release Candidate Commits:** `8fabe278170b` (HOS KSU), `b324ec6f4098` (HOS BakaSU), `b8c2acc398b0` (HOS Normal), `8473f2064f96` (COS KSU), `806c240d45b7` (COS BakaSU), `a17a9bb15358` (COS Normal)  
**Author / Developer:** NEESCHAL  

---

## 1. Executive Overview & Development Trajectory

EVONIX Kernel V4.6 represents a major architectural leap over EVONIX V4.5. Development commenced on 2026-11-17 from the stable V4.5 baseline (`5ecc54f415f3f`), spanning **77 discrete Git commits** across seven distinct engineering phases:

1. **Phase 1: Upstream Linux 6.6 LTS & Android GKI Integration** (Commits 1–54, `795aa886ecd19` $\to$ `160865f48a35f`): Upgrading the base kernel to Linux 6.6.144, backporting critical MediaTek OPP stability patches, EEVDF/deadline PI scheduling fixes, memory hotplug hardening, and 12 upstream F2FS atomic-write/GC integrity patches.
2. **Phase 2: Core EVONIX Schedutil Engine & NoMount Subsystem** (Commits 55–66, `551815c692144` $\to$ `7b719a5f65b79`): Implementing EVONIX Enhanced Schedutil with decoupled rate-limiting, dynamic headroom mapping, and the 2,779-line NoMount systemless path redirection subsystem.
3. **Phase 3: ADIOS I/O Scheduler, NTSync & Droidspaces Architecture** (Commits 67–71, `2cbbd53479699` $\to$ `68a926a174cd0`): Introducing the ADIOS v3.3.0 blk-mq scheduler, the fast Wine/Proton NTSync driver, CAKE/FQ-PIE/IP Set networking, and unlocking Droidspaces Linux containers via zero-KMI `task_struct` KABI reserve remapping.
4. **Phase 4: Universal Block-Layer ADIOS Default & Safe Fallback** (Commits 72–74, `5029b2f1d224a` $\to$ `993eaf5298a9f`): Making ADIOS the default scheduler on all compatible blk-mq queues (including Rodin UFS MCQ) with clean multi-tier fallback to Kyber/mq-deadline/none.
5. **Phase 5: Hardware-Aware All-Big-Core Audit & EVONIX Original Mechanisms** (Commit 75, `805443b229088`): Correcting core topology assumptions to reflect Rodin's true 8× Cortex-A725 architecture, introducing the EVONIX UI Responsiveness Engine (`CONFIG_EVONIX_SCHED_RESPONSIVE`), EVONIX Adaptive Memory Reclaim (`CONFIG_EVONIX_MM_ADAPTIVE`), and Foreground Read Prioritization (`CONFIG_EVONIX_IO_PRIO`).
6. **Phase 6: Subsystem Hardening, PM Lifecycle & Full Governor Freedom** (Commit 76, `c3d174707e5da`): Restoring complete CPU governor switching freedom for root users, hardening ADIOS anti-starvation against priority inversion, and adding suspend/resume PM notification to Adaptive Reclaim.
7. **Phase 7: Schedutil Boot Default Guard & Memory Ordering Hardening** (Commit 77, `8fabe278170b` / `b324ec6f4098`): Intercepting MediaTek vendor init's boot-time sysfs write to guarantee Schedutil as default while preserving user freedom, and hardening ADIOS cross-core atomic synchronization with acquire/release memory barriers.

---

## 2. Evidence Classification Standard

To maintain absolute technical precision, all claims in this changelog are classified strictly into one of seven verification levels:

* **[SOURCE VERIFIED]**: Code presence, logic flow, and diffs inspected and verified in the repository.
* **[CONFIG VERIFIED]**: Verified present in defconfigs, `evonix.config`, and extracted `embedded.config` via `extract-ikconfig`.
* **[BINARY VERIFIED]**: Symbol presence, layout, and absence of forbidden hooks verified inside `vmlinux` via `nm` / `llvm-objdump`.
* **[HOST-TEST VERIFIED]**: Executed and passed within automated host unit tests (`tools/testing/selftests/*`).
* **[RUNTIME VERIFIED]**: Executed, queried, and verified on a live physical POCO X7 Pro / Redmi Turbo 4 device via root ADB.
* **[PERFORMANCE MEASURED]**: Quantified with reproducible benchmarking tools (e.g., FIO, Geekbench, Jankbench).
* **[NOT YET VERIFIED]**: Known operational path or hardware state that has not been explicitly measured on physical hardware.

---

## 3. Subsystem-by-Subsystem Engineering Changelog

### 3.1 Linux 6.6 LTS Stable & Android GKI Upstream Integration

During Phase 1, EVONIX V4.6 upgraded from the Linux 6.6.139 baseline to the official Linux 6.6.144 LTS Android GKI branch, integrating upstream stability and security fixes:

#### MediaTek OPP (Operating Performance Points) & Energy Model
* **Commits**: `795aa886ecd19`, `90e7771721ccc`, `c614e4c48a5ff`
* **Files**: `drivers/opp/core.c`, `drivers/opp/opp.h`
* **Type**: Backported from upstream Linux 6.8 / 6.9-rc.
* **Rationale**: Resolves memory leaks during OPP supply parsing on multi-regulator platforms, eliminates integer multiplication overflow during high-frequency OPP table parsing, and locks down race conditions between concurrent OPP dynamic additions and frequency lookups during CPUFreq initialization.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[CONFIG VERIFIED]**, **[BINARY VERIFIED]**.

#### CPU Scheduling (EEVDF & Deadline Priority Inheritance)
* **Commits**: `eef8bc4a00a2c`, `c8dc39c387670`
* **Files**: `kernel/sched/deadline.c`, `kernel/sched/core.c`
* **Type**: Backported from upstream Linux 6.7 / 6.8.
* **Rationale**: Fixes a long-standing `struct task_struct` reference leak during deadline task migration, corrects priority inheritance (PI) boosting across affinity masks, and prevents starvation during nested mutex contention on heterogeneous and symmetric multi-core systems.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]**.

#### F2FS (Flash-Friendly File System) Storage & GC Integrity
* **Commits**: `06201f5de2562`, `8c0420bf6b73b`, `3d24d1e0aa7fd`, `6c643f8fed9a1`, `0a1a2690373c6`, `b2ebc7f5ba91a`, `f3e4e6ffdc821`, `79e79804021f9`, `698bd1302ef78`, `e68fb241395a0`, `0ad86cf25e7e6`, `0cc14619c6c87` (12 commits)
* **Files**: `fs/f2fs/segment.c`, `fs/f2fs/gc.c`, `fs/f2fs/checkpoint.c`, `fs/f2fs/node.c`, `fs/f2fs/data.c`
* **Type**: Backported from Jaegeuk Kim's upstream F2FS repository (Linux 6.9 / 6.10).
* **Rationale**:
  - Resolves atomic-write commit failures and race conditions with background garbage collection (GC).
  - Eliminates dentry page reference leaks on encrypted/casefolded directories.
  - Fixes node GC fsync marker inconsistency during ungraceful power loss.
  - Corrects free-section accounting during online filesystem resize (`resize.f2fs`).
  - Evacuates all current segment (`curseg`) types during resize to prevent metadata corruption on Rodin's UFS 4.0 partitions (`/data`, `/metadata`).
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[RUNTIME VERIFIED]** (Rodin UFS partitions mount and operate without filesystem errors).

#### Security, Keyrings & Virtualization
* **Commits**: `8c4d7897e22d1`, `5f295804427d2`, `0ddd0d3d7ddbb`, `ef1470b65df4b`, `f4d2c56ca430f`, `8201583a541c1`, `14e87fbbec379`, `64fa5fdd16ecd`, `bb3f8bd8596df`, `b06a5af499c2b`, `440edf343b134`, `e99f50f7b815e`, `a4e256e61bbf7`
* **Files**: `security/selinux/`, `security/keys/`, `security/landlock/`, `kernel/bpf/`, `arch/arm64/kvm/`
* **Type**: Upstream Linux 6.6 LTS security patches.
* **Rationale**: Prevents Landlock parent directory use-after-free, fixes SELinux Access Vector Cache (AVC) lookup races, hardens ARM64 KVM LPI affinity lifetime, and rejects fs-verity truncation.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]**.

---

### 3.2 CPUFreq, Governor Freedom & Default Schedutil Evolution

The CPU governor implementation underwent significant technical evolution during V4.6, responding to hardware realities and user requirements:

#### EVONIX Enhanced Schedutil Engine
* **Commit**: `551815c692144` (Initial), `89a8b61db0bd3` (Stabilization)
* **Files**: `kernel/sched/cpufreq_schedutil.c`, `arch/arm64/configs/evonix.config`
* **Config**: `CONFIG_EVONIX_SCHEDUTIL=y`
* **Type**: Newly introduced in EVONIX V4.6.
* **Technical Details**:
  - **Decoupled Rate Limiting**: Stock Linux schedutil enforces a single bidirectional rate limit (`rate_limit_us`). EVONIX decouples this into `up_rate_limit_us` (default: 1,000 µs = 1 ms) and `down_rate_limit_us` (default: 4,000 µs = 4 ms). This permits instant ramp-up when interactive frames drop, while providing a 4 ms hysteresis against rapid frequency drops that cause PLL voltage oscillation.
  - **Dynamic Headroom Curve (`evonix_map_util_perf`)**: Implements a continuous, strictly monotonic piecewise-linear mapping:
    $$\text{Target}(u) = \begin{cases} 
      \frac{u \times 472}{350} & u \le 350 \quad (+35\%\text{ headroom}) \\
      472 + \frac{(u - 350) \times 428}{400} & 350 < u \le 750 \quad (+20\%\text{ headroom}) \\
      900 + \frac{(u - 750) \times 226}{274} & 750 < u \le 1024 \quad (+10\%\text{ headroom}) \\
      1126 + (u - 1024) & u > 1024 
    \end{cases}$$
  - **Idle OPP Stabilization**: `idle_fast_down = 0` eliminates frequency collapse during sub-millisecond thread migrations, keeping the CPU at an active OPP rather than plunging to F_min during temporary pipeline stalls.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[RUNTIME VERIFIED]** (Tunables active in `/sys/devices/system/cpu/cpufreq/policy*/schedutil/`).

#### The Governor Policy Saga: Enforcement vs. Freedom
* **Evolution & Reversals**:
  1. *Original Implementation (`bfaca0898b61e`)*: Implemented `CONFIG_EVONIX_GOVERNOR_ENFORCE=y` in `drivers/cpufreq/cpufreq.c`. Rejected any sysfs write to `scaling_governor` with `-EPERM` to prevent MediaTek vendor init from switching to `sugov_ext`.
  2. *First Revision (`928c0eb355374`)*: Modified enforcement to default to unlocked mode via sysfs module parameter.
  3. *Complete Removal (`c3d174707e5da`)*: Completely removed `CONFIG_EVONIX_GOVERNOR_ENFORCE` from all C source files and configurations because hardcoded `-EPERM` blocks legitimate root user tuning (`performance`, `powersave`).
  4. *Final Architectural Solution (`8fabe278170b5` / `b324ec6f40983`)*:
     Implemented the **Schedutil Default Guard** inside `store_scaling_governor()` (`drivers/cpufreq/cpufreq.c`):
     ```c
     if (!strcmp(str_governor, "sugov_ext"))
         strscpy(str_governor, "schedutil", sizeof(str_governor));
     ```
     - **Mechanism**: When MediaTek vendor init executes `/vendor/etc/init/hw/init.cgroup.rc` (lines 16–18) writing `sugov_ext`, the kernel transparently remaps the target governor to `schedutil` and returns `count` (success) to userspace.
     - **Result**: `schedutil` is selected automatically on boot across all policies with zero user scripts, while root users retain 100% freedom to switch to `performance`, `powersave`, `conservative`, or `schedutil`.
* **Verification**: **[SOURCE VERIFIED]**, **[RUNTIME VERIFIED]** (Booted on Rodin with `schedutil` default; switched to `performance` and back to `schedutil` without `-EPERM`).

#### All-Big-Core Hardware Awareness (MediaTek Dimensity 8400-Ultra / MT6899)
* **Commit**: `805443b229088`
* **Type**: Hardware audit and architectural correction.
* **Findings**:
  - Corrected prior mistaken documentation assuming Cortex-A720 and Cortex-A520 cores.
  - Confirmed 100% All-Big-Core topology: **8× Cortex-A725** cores:
    * **Policy 0 (CPUs 0–3)**: 4× Cortex-A725 @ 2.10 GHz max (Energy-efficient OPP domain).
    * **Policy 4 (CPUs 4–6)**: 3× Cortex-A725 @ 3.00 GHz max (Performance OPP domain).
    * **Policy 7 (CPU 7)**: 1× Cortex-A725 @ 3.25 GHz max (Prime Turbo OPP domain).
* **Verification**: **[RUNTIME VERIFIED]** (`cat /sys/devices/system/cpu/online` shows `0-7` online, 0 offline, 0 isolated).

---

### 3.3 EVONIX UI Responsiveness Engine

* **Commit**: `805443b229088`
* **Files**: `kernel/sched/cpufreq_schedutil.c`, `drivers/cpufreq/Kconfig`, `arch/arm64/configs/evonix.config`
* **Config**: `CONFIG_EVONIX_SCHED_RESPONSIVE=y`
* **Type**: Newly introduced in EVONIX V4.6.
* **Rationale**: Android touch events and UI rendering pipelines (RenderThread, SurfaceFlinger, compositor) experience micro-stutters when the frequency governor takes multiple cycles to react to sudden bursts of CPU utilization.
* **Technical Details**:
  - Adds `ui_boost_pct` sysfs tunable (range: 0–100%, default: 20%).
  - Evaluates runqueue characteristics during `sugov_get_util()`. If the CPU hosts a task belonging to an interactive or latency-sensitive cgroup (or flagged with `REQ_PRIO` / interactive latency hint), an immediate 20% frequency headroom boost is applied to the utilization target:
    $$\text{Util}_{\text{boosted}} = \text{Util} + \frac{\text{Util} \times \text{ui\_boost\_pct}}{100}$$
  - Fully bounds the resulting frequency to `policy->max` to strictly respect MediaTek thermal throttling and OPP limits.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[CONFIG VERIFIED]**, **[RUNTIME VERIFIED]** (`ui_boost_pct` attribute active in sysfs on Rodin).

---

### 3.4 ADIOS v3.3.0 blk-mq I/O Scheduler & Concurrency Hardening

* **Commits**: `2cbbd53479699` (Porting), `c0cb985c71cd5` (API Adaptations), `5029b2f1d224a` (Default Everywhere), `993eaf5298a9f` (Lock Contention), `805443b229088` (Foreground Guard), `c3d174707e5da` (Anti-Starvation), `8fabe278170b5` (Memory Ordering)
* **Files**: `block/adios.c`, `block/elevator.c`, `block/Kconfig.iosched`, `block/Makefile`
* **Configs**: `CONFIG_MQ_IOSCHED_ADIOS=y`, `CONFIG_MQ_IOSCHED_DEFAULT_ADIOS=y`, `CONFIG_EVONIX_IO_PRIO=y`
* **Type**: Ported from upstream ADIOS and heavily enhanced for Linux 6.6 LTS and MediaTek UFS MCQ.
* **Technical Details**:
  - **Linux 6.6 LTS API Adaptation**: Upstream ADIOS v3.3.0 targeted Linux 6.11+. EVONIX backported lock class key initializations, adapted blk-mq request macros, and resolved kernel header changes.
  - **Universal Default Selection**: Modified `elevator_get_default()` in `block/elevator.c` to select `adios` across all eligible queues, overriding `BLK_MQ_F_NO_SCHED_BY_DEFAULT` on storage queues while preserving stacked device rules.
  - **Multi-Tier Fallback Safety**: In `elevator_init_mq()`, if ADIOS initialization fails, it cleanly releases all elevator references via `elevator_put()` and falls back sequentially to `kyber`, `mq-deadline`, or `none`, preventing null-pointer dereferences.
  - **Lockless Concurrency Probe**: In `dispatch_from_pq()`, probes `eval_adios_state(ad, ADIOS_STATE_PQ)` before acquiring spinlocks, eliminating lock contention across Rodin's 8 UFS Multi-Circular Queues (`mq/0`–`mq/7`).
  - **Memory Barrier Hardening**:
    * Added `smp_wmb()` and `smp_mb__after_atomic()` to `set_adios_state()`.
    * Upgraded `get_adios_state()` to `(u32)atomic_read_acquire(&ad->state)`.
    * Ensures state bit changes are globally visible across all 8 cores before queue dispatching.
  - **EVONIX Foreground Read Prioritization (`CONFIG_EVONIX_IO_PRIO=y`)**:
    * Synchronous read requests (`REQ_SYNC` + `READ`) carrying `REQ_PRIO`, `REQ_META`, or RT/BE $\le 2$ I/O priority classes are expedited through `prio_queue[1]`.
    * **Anti-Starvation Guard**: If sustained Tier-1 foreground reads dispatch for $\ge 8$ consecutive requests and normal queues have work, dispatch yields to prevent background I/O starvation.
    * **Tier-0 Emergency Head Exemption**: Tier-0 emergency head requests are strictly exempt from starvation quotas.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[HOST-TEST VERIFIED]** (24/24 tests passed), **[RUNTIME VERIFIED]** (`sda` and `sdb` default to `[adios]`; `sdc` runtime switching tested; `adios_version = 3.3.0`, `read_priority = 8` verified).

---

### 3.5 EVONIX Adaptive Memory Reclaim Controller

* **Commits**: `22bdc028f81dd` (Early experiment), `efae2c0c3b59f` (Stock boost restoration), `805443b229088` (Adaptive Reclaim), `c3d174707e5da` (PM Lifecycle Hardening)
* **Files**: `mm/evonix_mm.c`, `mm/Kconfig`, `mm/Makefile`, `arch/arm64/configs/evonix.config`
* **Configs**: `CONFIG_EVONIX_MM_OPTIMIZE=y`, `CONFIG_EVONIX_MM_ADAPTIVE=y`
* **Type**: Newly introduced in EVONIX V4.6.
* **Evolution & Rationale**:
  - Direct memory reclaim during burst allocations (such as launching high-memory apps or switching camera modes) stalls foreground UI threads while the kernel synchronizes pages.
  - An early experiment in `22bdc028f81dd` modified watermark boost factors, but commit `efae2c0c3b59f` properly restored Linux 6.6 stock watermark boost to avoid breaking buddy allocator page compaction.
  - EVONIX replaced this with a dedicated background headroom controller (`mm/evonix_mm.c`).
* **Technical Details**:
  - **Proactive Headroom Monitoring**: Runs a periodic work function inspecting `NR_FREE_PAGES` across managed zones (`ZONE_NORMAL`, etc.). If free memory dips below $W_{\text{high}} + \text{cushion}$ (default: 64 MB cushion), it calls `wakeup_kswapd()` proactively.
  - **Wakeup Rate-Limiting**: Limits kswapd wakeups to at most once per second (unless memory drops below $W_{\text{low}}$), preventing ZRAM churn and page-cache thrashing.
  - **Power-Efficient Freezable WQ**: Queued on `system_freezable_power_efficient_wq`, allowing the workqueue to sleep during CPU low-power states.
  - **PM Notifier Lifecycle**: Implements `evonix_mm_pm_notify`:
    * `PM_SUSPEND_PREPARE`: Calls `cancel_delayed_work_sync(&evonix_reclaim_dwork)` to guarantee zero background wakeups during deep suspend.
    * `PM_POST_SUSPEND`: Requeues delayed work after 1,000 ms to resume headroom monitoring.
  - **Sysctl Control**: Configurable via `/proc/sys/vm/evonix_adaptive_reclaim` (0/1) and `/proc/sys/vm/evonix_reclaim_cushion_mb` (16–512 MB).
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]** (`ffffffc082364d2c d __initcall__kmod_evonix_mm`), **[RUNTIME VERIFIED]** (Active on Rodin: sysctls verified, cushion = 64 MB).

---

### 3.6 Droidspaces & Linux Container Virtualization Subsystems

* **Commit**: `2cbbd53479699`
* **Files**: `include/linux/sched.h`, `arch/arm64/configs/evonix.config`, `docs/evonix-optimization-matrix.md`
* **Configs**: `CONFIG_UTS_NS=y`, `CONFIG_IPC_NS=y`, `CONFIG_USER_NS=y`, `CONFIG_PID_NS=y`, `CONFIG_NET_NS=y`, `CONFIG_SYSVIPC=y`, `CONFIG_SYSVIPC_SYSCTL=y`, `CONFIG_POSIX_MQUEUE=y`, `CONFIG_POSIX_MQUEUE_SYSCTL=y`, `CONFIG_MACVLAN=y`, `CONFIG_TMPFS_XATTR=y`, `CONFIG_TMPFS_POSIX_ACL=y`
* **Type**: Newly introduced in EVONIX V4.6.
* **Breakthrough Engineering (Zero KMI Breakage)**:
  - Standard Android GKI kernels cannot enable System V IPC (`CONFIG_SYSVIPC=y`) because adding `struct sysv_sem sysvsem` and `struct sysv_shm sysvshm` into `struct task_struct` shifts member offsets, which violates the Android Kernel Module Interface (KMI) and causes vendor kernel modules (camera, display, GPU) to fail to load.
  - EVONIX re-routed these fields into existing Google KMI reserve slots in `include/linux/sched.h`:
    ```c
    #ifdef CONFIG_SYSVIPC
        ANDROID_KABI_USE(3, struct sysv_sem sysvsem);
        ANDROID_KABI_RESERVE(4);
        ANDROID_KABI_RESERVE(5);
        union {
            struct sysv_shm sysvshm;
            struct {
                ANDROID_KABI_RESERVE(6);
                ANDROID_KABI_RESERVE(7);
            };
        };
        ANDROID_KABI_RESERVE(8);
    #endif
    ```
  - This preserves the exact byte-for-byte struct layout and symbol CRC hashes of `task_struct`, achieving **0 KMI violations** while enabling full container capabilities.
* **Container Prerequisites Unlocked**:
  - **User Namespaces**: Allows rootless container execution (Podman, Docker, LXC, Droidspaces).
  - **System V IPC**: Provides shared memory (`shmget`/`shmat`), semaphores, and message queues required by X11, PulseAudio, Chromium, and Linux desktop apps.
  - **POSIX Message Queues**: Native `/dev/mqueue` support.
  - **Virtual Ethernet (veth)**: Full network virtualization between host and container.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[CONFIG VERIFIED]**, **[RUNTIME VERIFIED]** (Tested via live compiled binary on Rodin: SysV shm, sem, msg, namespaces unshare, and veth dynamic creation all passed).

---

### 3.7 NTSync: Fast NT Synchronization Driver

* **Commits**: `2cbbd53479699` (Integration), `c0cb985c71cd5` (API Adaptation)
* **Files**: `drivers/misc/ntsync.c`, `include/uapi/linux/ntsync.h`, `drivers/misc/Makefile`, `drivers/misc/Kconfig`
* **Config**: `CONFIG_NTSYNC=y`
* **Type**: Backported from Elizabeth Figura's upstream Wine/Proton kernel driver.
* **Rationale**: Wine and Windows translation layers (e.g., Winlator, Mobox, Box64) on Android suffer severe performance bottlenecks from userspace `eventfd` synchronization emulation. NTSync implements Windows NT primitives (events, semaphores, mutexes) directly in the Linux kernel via a character device.
* **Technical Details**:
  - Implements `/dev/ntsync` with dynamic minor registration (`MISC_DYNAMIC_MINOR`, permissions `0666`).
  - Adapted atomic waitqueues and ioctl handlers (`NTSYNC_IOC_CREATE_EVENT`, `NTSYNC_IOC_WAIT_ALL`, `NTSYNC_IOC_WAIT_ANY`) to Linux 6.6 LTS API.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]** (`ffffffc080be5b84 t ntsync_char_ioctl`, `ffffffc082364788 d __initcall__kmod_ntsync`), **[RUNTIME VERIFIED]** (`/dev/ntsync` character device present on Rodin).

---

### 3.8 Advanced Networking, Congestion Control & Packet Filtering

* **Commit**: `2cbbd53479699`
* **Files**: `net/sched/`, `net/netfilter/`, `arch/arm64/configs/evonix.config`
* **Configs**: `CONFIG_NET_SCH_CAKE=y`, `CONFIG_NET_SCH_FQ_PIE=y`, `CONFIG_NET_SCH_PIE=y`, `CONFIG_IP_SET=y`, `CONFIG_IP6_NF_NAT=y`, `CONFIG_NETFILTER_XT_TARGET_HL=y`
* **Type**: Newly introduced in EVONIX V4.6.
* **Technical Details**:
  - **CAKE (Common Applications Kept Enhanced)**: Advanced queue management (AQM) and packet scheduling eliminating bufferbloat on mobile Wi-Fi/cellular links.
  - **FQ-PIE & PIE**: Modern Fair Queueing and Proportional Integral controller Enhanced queueing.
  - **IP Set (`CONFIG_IP_SET=y`)**: $O(1)$ hash tables for IP/port matching in netfilter, accelerating firewalling for tethering and Droidspaces container networking.
  - **IPv6 NAT (`CONFIG_IP6_NF_NAT=y`)**: Native IPv6 masquerading and network address translation for container bridge interfaces.
  - **Hop Limit / TTL Mangling (`CONFIG_NETFILTER_XT_TARGET_HL=y`)**: Allows adjusting IP hop limits for tethered and containerized traffic.
* **Variants Affected**: All 6 variants.
* **Verification**: **[SOURCE VERIFIED]**, **[CONFIG VERIFIED]** (Confirmed in all 6 `embedded.config` files).

---

### 3.9 Root Frameworks, SUSFS & Pure Rootless Isolation

EVONIX V4.6 maintains strict isolation between rooted and non-rooted variants across both ROM ecosystems:

#### KernelSU-Next & BakaSU Integration
* **Submodules / Repositories**:
  - KernelSU-Next: v3.4.2 (version code `33365`, tag `v3.4.2`, manager hash `79e59011...`).
  - BakaSU: v4.2.0-rc3 (`4.2.0-rc3-91-g7b65bde0`).
* **Rooted Variants**: `hyperos-ksu-susfs`, `bakasu-hyperos-susfs`, `coloros-ksu-susfs`, `bakasu-coloros-susfs`.
* **Verification**: **[RUNTIME VERIFIED]** (BakaSU running on device: `uid=0(root)`, context `u:r:ksu:s0`).

#### SUSFS v2.4.0 Integration
* **Files**: `fs/susfs.c`, `include/linux/susfs.h`
* **Configs**: `CONFIG_KSU_SUSFS=y`, `CONFIG_KSU_SUSFS_SUS_PATH=y`, `CONFIG_KSU_SUSFS_SUS_MOUNT=y`, `CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT=y`
* **Features**: Kernel-level path hiding, mount hiding, and overlayfs stealth hooks to bypass root and integrity detection mechanisms.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]** (`ffffffc0807542dc T susfs_init`).

#### NoMount Subsystem
* **Commit**: `a5ab3ea0bbd6f`
* **Files**: `fs/nomount/nomount.c` (2,234 lines), `fs/nomount/nomount.h` (545 lines), `fs/nomount/Makefile`, `fs/nomount/Kconfig`
* **Config**: `CONFIG_NOMOUNT=y`
* **Type**: Ported and adapted for Linux 6.6 LTS.
* **Rationale**: Replaces overhead-heavy overlay mounts with in-kernel vfs lookup and path redirection for systemless module injection.
* **Verification**: **[SOURCE VERIFIED]**, **[BINARY VERIFIED]** (`ffffffc081ef9464 t nomount_init`), **[RUNTIME VERIFIED]** (dmesg verified injection of `rodin-leica-camera` module).

#### Pure Rootless Isolation (Normal Variants)
* **Variants**: `hyperos-normal`, `coloros-normal`.
* **Audit Standard**: Zero root symbols, zero hooks, zero backdoors.
* **Audit Evidence**:
  - Binary scan via `nm`: **0** `ksu_*`, **0** `susfs_*`, **0** `nomount_*` symbols.
  - Config audit via `extract-ikconfig`: **0** `CONFIG_KSU`, **0** `CONFIG_BAKASU`, **0** `CONFIG_KSU_SUSFS`, **0** `CONFIG_NOMOUNT`.
* **Verification**: **[BINARY VERIFIED]**, **[CONFIG VERIFIED]** (100% pure standard GKI compliance).

---

### 3.10 ROM Compatibility & Ecosystem Support

EVONIX V4.6 supports two major ROM environments on Rodin:
1. **Xiaomi HyperOS 4** (Android 17, `OS4.0.7.0.XPJEUXM`).
2. **ColorOS Port** (ColorOS 14/15 port for Rodin).

#### AnyKernel3 Preflight Protection
* **Files**: `tools/ak3-core.sh`, `anykernel.sh` (generated by `package_release_candidate.py`).
* **Mechanism**:
  - HyperOS packages inspect `ro.build.version.oplusrom`, `ro.oplus.version.my_manifest`, and `ro.build.version.opporom`. If a ColorOS environment is detected, installation safely aborts.
  - ColorOS packages inspect `ro.miui.ui.version.name` and `ro.miui.ui.version.code`. If HyperOS is detected, installation safely aborts.
  - Prevents accidental cross-flashing that leads to bootloops.
* **GKI `init_boot` Separation**: Inspects `/dev/block/by-name/init_boot_a`. Uses `split_boot; flash_boot;` to flash raw kernel images directly to `boot`, completely leaving the ramdisk and root modules in `init_boot` untouched.
* **Verification**: **[SOURCE VERIFIED]**, **[RUNTIME VERIFIED]** (Flashed cleanly via OrangeFox Recovery with installer exit code 0).

---

### 3.11 Build Infrastructure, ThinLTO, AutoFDO & Tooling

* **Compiler**: Android Clang r614150 (Clang 23.0.1, commit `52d25c7f6cf8d458a5d1cda9729c2ed757ebe86e`).
* **Linker**: LLD 23.0.1.
* **Optimization Flags**:
  - ThinLTO: `--lto=thin`, `-plugin-opt=thinlto`, `-z noexecstack`, `--pack-dyn-relocs=relr`.
  - AutoFDO: Profile-guided optimization using `kernel.afdo` with FS discriminators enabled (`-mllvm -enable-fs-discriminator=true`, `-mllvm -improved-fs-discriminator=true`).
* **Kleaf / Bazel Infrastructure**:
  - Shared disk cache (`WORKSPACE/out/cache/disk_cache`).
  - Dynamic parallel jobs (12 jobs).
  - Clean modular output directories (`out/dist-*`).
* **Orchestration Tooling**:
  - `tools/evonix_build_matrix.py`: Multi-variant build orchestrator with automated signature hashing, dependency tracking, artifact verification, and SHA-256 generation.
  - `tools/package_release_candidate.py`: Release candidate packager verifying embedded configs, image byte matching, and generating `RELEASE-CANDIDATE-MANIFEST.txt`.
* **Verification**: **[SOURCE VERIFIED]**, **[CONFIG VERIFIED]**, **[BINARY VERIFIED]**.

---

## 4. Reconciled Final Release Artifacts

All six variants have been compiled, verified, packaged, and validated with on-device SHA-256 checks:

```
==============================================================================================================
EVONIX KERNEL V4.6 — RELEASE CANDIDATE ARTIFACT MATRIX
Target: POCO X7 Pro / Redmi Turbo 4 (rodin) | SoC: MediaTek Dimensity 8400-Ultra (MT6899)
Toolchain: Clang r614150 (23.0.1) + ThinLTO + AutoFDO | Developer: NEESCHAL
==============================================================================================================
VARIANT                 GIT COMMIT   IMG SIZE  ZIP SIZE  FLASHABLE ZIP SHA-256
--------------------------------------------------------------------------------------------------------------
HyperOS KSU + SUSFS     8fabe27817   37.62 MB  18.53 MB  41993353f8cd32be283e07e5f1201d693f3f999e64b33446af682637f3f776f8
HyperOS BakaSU + SUSFS  b324ec6f40   37.49 MB  18.54 MB  69f22d70923b3a53ec37b8eea3af0d2fe4789dcccf5334df4ab407b241989fbf
HyperOS Normal          b8c2acc398   37.24 MB  18.41 MB  eb8fef695047218fd772eb60e5198a87b3e7bac5c4109bb00378a8494ebda458
ColorOS KSU + SUSFS     8473f2064f   38.08 MB  18.71 MB  ec111826ce51663b147491cb5645d641d20392166ba90b67f78bd2dc10d38242
ColorOS BakaSU + SUSFS  806c240d45   38.02 MB  18.72 MB  5ed75d0e3a362dae6759d2d4bad5b5ae7ac78fb9792cd92a7d9bb63fb4a7d26e
ColorOS Normal          a17a9bb153   37.65 MB  18.60 MB  428ea74e899f3e6634b3f89e8ae3b0a3e9e48562613f9fca39eba27bc328295e
==============================================================================================================
```

### On-Device Staging & Boot Verification
* **Storage Location on Rodin**: `/sdcard/EVONIX/V4.6/`
* **On-Device SHA-256 Verification Result**: **6/6 OK** (100% bit-for-bit integrity).
* **Physical Device Boot Verified**: **`EVONIX-V4.6-HyperOS-BakaSU-SUSFS-rodin.zip`** (Commit `b324ec6f40`) flashed via OrangeFox Recovery, running live on Android 17 HyperOS 4.
* **Other Variants**: Compiled, binary-verified, and host-test verified with identical kernel code, but not individually flashed on physical hardware.

---

## 5. Summary of Remaining Limitations & Outstanding Checks

1. **Cold Boot Runtime Verification on Other 5 Variants**: While `bakasu-hyperos-susfs` has been fully validated on hardware, the remaining 5 variants (`hyperos-ksu-susfs`, `hyperos-normal`, `coloros-ksu-susfs`, `bakasu-coloros-susfs`, `coloros-normal`) have passed all static, binary, and unit tests, but have not been physically flashed to a device.
2. **ColorOS Port Live Hardware Test**: The ColorOS preflight check and kernel image are verified, but testing on a physical ColorOS-ported Rodin device remains open.
3. **Formal Benchmark Suite**: Real-world performance under sustained synthetic workloads (Geekbench 6, 3DMark, FIO throughput graphs) should be captured during standard daily usage to quantify efficiency gains over stock.
