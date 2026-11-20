# Changelog

## V4.6

**Branch coverage:** The complete EVONIX V4.6 changelog is maintained across all six supported source branches. Shared kernel improvements are integrated across applicable variants, while ROM-specific and root-framework-specific components remain exclusive to their respective editions. The `coloros/ksu-susfs-permissive` branch is excluded from this release and remains unchanged.

### Linux 6.6 stable updates and Android GKI

- **Kernel Base Upgrade:** Upgraded the kernel base from Linux 6.6.142 to Linux 6.6.144 LTS Android GKI.
- **MediaTek OPP Stability:** Backported upstream fixes for Operating Performance Points and Energy Model parsing, eliminating multi-regulator supply leaks, preventing integer overflow during high-frequency table calculation, and locking down dynamic OPP addition races during CPUFreq initialization.
- **EEVDF and Priority Inheritance:** Resolved struct task_struct reference leaks during deadline task migration, corrected cross-affinity priority inheritance boosting, and mitigated starvation under nested mutex contention.
- **Memory Hotplug Hardening:** Synchronized deferred struct page initialization paths to eliminate initialization race conditions.
- **Android 15 GKI Backports:** Integrated common Android 15 GKI stability updates while maintaining 100% symbol and structure KMI compatibility.

### CPU scheduling, EEVDF and Enhanced Schedutil

- **Decoupled Asymmetric Rate Limiting:** Implemented asymmetric rate limits (1000 µs ramp-up via up_rate_limit_us=1000 and 4000 µs ramp-down via down_rate_limit_us=4000), allowing faster frequency elevation during rising load while preventing premature frequency drops during active workloads.
- **EVONIX Dynamic Headroom Curve:** Implemented a continuous piecewise-linear frequency headroom curve (+35% at light load tapering smoothly to +10% near saturation), providing additional capacity headroom for interactive tasks before frame deadlines.
- **Automatic Schedutil Boot Enforcement:** Intercepted MediaTek vendor init's boot-time sysfs write, ensuring Schedutil is enforced as the active system default across all CPU policies without requiring userspace helper scripts.
- **Restored CPU Governor Freedom:** Removed unconditional governor locking, restoring the ability for root users to switch to performance, powersave, conservative, or adjust custom frequency boundaries.
- **Hardware-Aware Core Topology:** Refactored MediaTek cluster topology configuration to model Rodin's All-Big-Core architecture (8x Cortex-A725 across 3 performance gear groups: 1x 3.25 GHz Prime, 3x 3.0 GHz Gold, 4x 2.0 GHz Silver).
- **EEVDF Slice Optimization:** Tuned normalized base scheduling slice to 500 µs (2.0 ms scaled across 8 cores), balancing interactive responsiveness and batch throughput.

### EVONIX UI Responsiveness Engine

- **Kernel-Level UI Identification (CONFIG_EVONIX_SCHED_RESPONSIVE):** Automatically identifies latency-critical user-interface threads (SurfaceFlinger, RenderThread, SystemUI, and foreground application compositor threads).
- **Targeted Headroom Bias:** Applies a configurable 20% frequency headroom bias (ui_boost_pct=20) to active UI threads during touch interactions and frame rendering to expedite task completion.
- **Thermal Coordination:** Coordinates with platform Energy-Aware Scheduling (EAS) while respecting hardware thermal governor limits.

### ADIOS I/O scheduler and Foreground Read Prioritization

- **Default blk-mq Selection (CONFIG_IOSCHED_ADIOS):** Configured ADIOS v3.3.0 as default scheduler for compatible blk-mq storage queues, with validated binding on Rodin UFS 4.0 MCQ (Multi-Circular Queues).
- **Foreground Read Prioritization (CONFIG_EVONIX_IO_PRIO):** Provides Tier-1 dispatch priority for synchronous reads initiated by foreground UI tasks over background writeback to reduce dispatch wait times during storage contention.
- **Lockless State Evaluation:** Eliminates dispatch lock contention across hardware queues using lockless queue state checks.
- **Anti-Starvation Throttling:** Enforces request-depth tracking and throttles lower-priority queues after 8 consecutive dispatches, with strict exemptions for emergency head requests.
- **Memory Ordering:** Enforces cross-core cache coherency and atomic visibility with explicit memory barriers (smp_mb__after_atomic).
- **Graceful Multi-Tier Fallback:** If a controller queue cannot bind to ADIOS, the block layer automatically falls back to Kyber, mq-deadline, or none without kernel panics.

### Filesystem integrity and F2FS fixes

- **Upstream F2FS Fixes:** Backported 12 upstream integrity and garbage-collection (GC) patches.
- **Atomic Write Integrity:** Hardened atomic write handling to reduce the risk of filesystem corruption during abrupt power loss or kernel panic scenarios.
- **Serialization and GC Stability:** Resolved checkpoint serialization races and improved background GC victim selection under high disk fill ratios.
- **Asynchronous I/O:** Fixed block allocation race conditions in multi-threaded asynchronous I/O paths and addressed stale inode writeback metadata leaks.

### Memory management, MGLRU and Adaptive Memory Reclaim

- **EVONIX Adaptive Memory Reclaim (CONFIG_EVONIX_MM_ADAPTIVE):** Proactively monitors managed memory zones against a 64 MB cushion (evonix_reclaim_cushion_mb = 64) to wake kswapd earlier before free memory breaches critical low watermarks, mitigating direct reclaim invocations during burst allocations.
- **Buddy Compaction Preserved:** Retains Linux 6.6 stock compaction watermark boost factors to maintain healthy high-order page compaction.
- **Power Management Lifecycle:** Bound to kernel suspend/resume notifiers and queued on system_freezable_power_efficient_wq, ensuring background reclaim activity is paused during system sleep.
- **MGLRU Aging:** Ensured Multi-Gen LRU proactive aging routines remain interruptible during system suspend transitions.

### Kernel power management and thermal integration

- **Authoritative Thermal Mitigation:** Preserved full MediaTek thermal governor protection across all CPU clusters and battery charging circuitry.
- **Power-Efficient Workqueues:** Bound all EVONIX background workers to freezable, power-efficient workqueues to maximize deep-sleep residency.
- **Timer Slack Correction:** Corrected timer slack inheritance for normal tasks spawned from real-time parents, reducing unnecessary CPU package wakeups.
- **Hardware Standby Validation:** Verified suspend/resume functionality and deep sleep state entry on physical hardware.

### Networking and congestion control

- **BBRv3 Default:** Configured BBRv3 as the default TCP congestion control algorithm for improved throughput and latency characteristics over cellular and Wi-Fi networks.
- **Queue Disciplines:** Enabled CAKE, PIE, and FQ-PIE packet schedulers for active queue management.
- **IP Sets and Filtering:** Enabled CONFIG_IP_SET with hash:ip, hash:net, and hash:mac types.
- **IPv6 NAT and TTL:** Enabled full IPv6 NAT, masquerade targets, and TTL/Hop-Limit manipulation (CONFIG_NETFILTER_XT_TARGET_HL).
- **Transport Layer Hardening:** Integrated upstream fixes for TCP retransmission lifetimes and UDP tunnel segmentation.

### NTSync and Windows synchronization

- **Fast NT Synchronization (CONFIG_NTSYNC):** Backported the upstream Linux NTSync driver, exposing /dev/ntsync to provide in-kernel synchronization primitives for Windows NT mutexes, semaphores, and events.
- **Emulation Support:** Provides kernel driver support to reduce userspace eventfd synchronization overhead in Wine and Proton environments; complete application compatibility depends on userspace translation runtime configuration.

### Container support and Droidspaces

- **Container Kernel Prerequisites:** Implemented kernel-level container prerequisites including User Namespaces (CONFIG_USER_NS with UID/GID mapping), PID, Network, Mount, UTS, and IPC namespace isolation, POSIX message queues (/dev/mqueue), virtual Ethernet (veth) pairs, and MACVLAN devices.
- **Zero KMI Breakage:** Routed System V IPC structures (sysvsem, sysvshm) through unused KMI reserved fields in struct task_struct, providing required container IPC primitives with zero vendor module symbol or structure offset breakage. Full userspace container and Droidspaces functionality requires appropriate userspace runtime setup.

### Root frameworks (KernelSU-Next, BakaSU, SUSFS v2.4.0, NoMount)

- **KernelSU-Next Editions (hyperos/ksu-susfs, coloros/ksu-susfs):**
  - Integrated KernelSU-Next v3.4.2 (commit 26499d89fb).
  - Integrated SUSFS v2.4.0 with kernel-level path, mount, and cgroup hiding.
  - Integrated NoMount subsystem (2,779 lines in fs/nomount/) for high-performance in-kernel VFS path redirection as a lightweight alternative to overlay mounts.
- **BakaSU Editions (bakasu/hyperos-susfs, bakasu/coloros-susfs):**
  - Integrated BakaSU with dedicated SUSFS 2.4.0 SELinux bridge (ksu_security_context_to_sid_with_policy) for stealth operation under enforcing SELinux.
  - Integrated SUSFS v2.4.0 and NoMount subsystem.
- **Normal Editions (hyperos/normal, coloros/normal):**
  - Pure rootless standard GKI builds with zero root hooks and zero symbols (0 ksu_*, 0 susfs_*, 0 nomount_*) for users requiring stock security integrity.

### Platform compatibility (HyperOS and ColorOS)

- **HyperOS Editions:** Native integration with Xiaomi HyperOS 4 / Android 17 userspace, full Dimensity 8400-Ultra vendor module compatibility, and hardware charging bypass via standard OEM interfaces.
- **ColorOS Editions:** Dedicated MT6899 OPlus SchedAssist integration with isolated scheduler data structures, FrameBoost integration with corrected cluster ordering, OPlus MIDAS v2, process-reclaim helpers, and HealthInfo Lite CPU-loading support.

### Build system, compiler optimizations and testing infrastructure

- **Compiler Toolchain:** Compiled with official Android Clang r614150 (23.0.1) and LLVM LLD.
- **Advanced Optimizations:** ThinLTO and AutoFDO profile-guided optimization.
- **Kleaf/Bazel Integration:** Built within Google Kleaf hermetic build system.
- **Test Harness:** Comprehensive selftest suite covering scheduler headroom, governor switching, ADIOS dispatch ordering, and container namespace isolation.


## V4.5

### CPU scheduling and performance

- Added latency-sensitive wakeup handling and shorter scheduling slices for tasks marked by the ROM.
- Added PREEMPT_SHORT scheduling while retaining Android and MediaTek scheduling decisions.
- Reduced repeated EEVDF eligibility calculations.
- Fixed task-group runnable accounting overflow.
- Corrected CPU-group selection for queued workloads.
- Fixed utilization-clamp values carrying over between EAS CPU candidates.
- Removed duplicate utilization updates during CPU and idle-state selection.
- Skipped EAS domains without an allowed CPU candidate.
- Avoided unused fallback calculations when MediaTek supplies CPU placement.
- Reduced unnecessary frequency and CPU-capacity scans.
- Improved guarded migration of queued workloads to suitable idle, higher-capacity CPUs.
- Prevented failed misfit migrations from delaying unrelated load balancing.
- Removed unnecessary destination searches and load-balancer updates.
- Preserved vendor-requested NOHZ balancing work.
- Added DVFS notifications when active cgroup performance clamps change.
- Fixed CPUFreq policy initialization and reduced repeated frequency-table lookups.
- Corrected schedutil rate-limit conversion overflow.

### Power management and idle

- Added power-efficient handling for selected asynchronous power-management work.
- Reduced redundant deferred DMA-buffer worker notifications.
- Avoided unused runtime-idle callback lookups.
- Corrected normal-task timer-slack inheritance from real-time parents.
- Fixed idle power-domain allocation cleanup and clocksource registration cleanup.
- Corrected the userspace wakelock-limit boundary and tick-broadcast replacement race.
- Reduced unnecessary memory clearing in OEM charging-status reads.

### Memory and multitasking

- Made proactive MGLRU reclaim interruptible during suspend.
- Made interrupted memory-reclaim requests restartable.
- Added Tasks-RCU progress reporting during reclaim, page migration and cgroup writeback.
- Stopped obsolete memory-cgroup reclaim after newer limit updates.
- Fixed locked-memory accounting and allocator zeroing.
- Fixed shrinker callback handling.
- Corrected swap readahead after its configured ceiling is lowered.
- Rejected unsafe direct swap files on filesystem-encrypted storage.
- Fixed repeated DAMON region aging, merge-threshold overflow and stale access-result reuse.

### Storage and filesystems

- Fixed ext4 orphan-buffer leaks and stale writeback tags.
- Corrected ext4 direct-I/O to buffered-I/O fallback handling.
- Fixed F2FS move-range validation and writeback error reporting.
- Fixed block-cgroup creation cleanup, statistics traversal and dying-group handling.
- Corrected block-device timeout cleanup and queue teardown.
- Included offline CPUs' pending latency samples in block, Kyber and I/O-cost accounting.

### Networking and kernel fixes

- Fixed TCP retransmission lifetime, urgent-data corruption and outdated-ACK handling.
- Fixed UDP tunnel-segmentation use-after-free.
- Corrected IPv6 checksum bounds, extension-header parsing and segment-routing validation.
- Fixed ARP interface-name termination, XPS traffic-class validation and IPv4 route cleanup.
- Fixed POSIX CPU-timer lifetime races and PID-reference leaks.
- Corrected timer-migration state, workqueue flush checks and watchdog overflow handling.
- Fixed ARM64 per-CPU write casting and bitmask generation.

### ColorOS

- Integrated native MT6899 OPlus SchedAssist with MediaTek scheduler hooks.
- Isolated OPlus scheduler storage from Xiaomi scheduler data.
- Added the native waker-identification scheduler bridge.
- Integrated FrameBoost with corrected MT6899 cluster ordering and CPUFreq-aware initialization.
- Added OPlus CPUFreq health support and fixed OCH frequency-table lookup.
- Integrated MIDAS v2.
- Added OPlus process-reclaim, kswapd and proactive-compaction helpers.
- Added HealthInfo Lite CPU-loading support.
- Added OPlus project identification, rodin fallback and device/memory diagnostics.
- Added Phoenix observation support without enabling its recovery watchdog.
- Brought the shared V4.5 changes to both ColorOS editions.

### KernelSU Next editions

- Updated KernelSU Next to v3.4.1.
- Corrected SUSFS kernel-stat compatibility with userspace.
- Simplified SUSFS filesystem-stat handling using the existing VFS helper.


## V4.0

### Rodin tuning

- Tune instruction scheduling for Cortex-A725 without changing the arm64 ISA.
- Select a 500-microsecond normalized EEVDF base slice (2 ms with the existing
  eight-CPU logarithmic scaling, previously 2.8 ms).
- Enable consolidation for workqueues explicitly marked power-efficient.
- Batch ordinary RCU callbacks with lazy RCU.
- Keep KFENCE enabled with a deferrable sampling timer.
- Preserve the 32-CPU GKI build ABI ceiling, stock governors and thermal protection.
- Use Android Clang 23.0.1 with ThinLTO, AutoFDO and O2.

### Charging

- Replace the previous bypass hooks with a shared OEM charge-pause backend
  for HyperOS and ColorOS.
- Use the stock navigation charge-pause path; do not use input suspend or
  replace vendor_boot/recovery.
- Add request ownership, busy-state rejection, readback, rollback and measured
  battery-neutral status.
- Avoid unnecessary charge-pump and supply reads while observation is inactive.
- Report observed status honestly: battery-neutral current is not proof of
  complete electrical isolation.

### Partition protection

- Integrate Baseband-guard from upstream commit
  `a54e0dc6cf0aff4dd87fec49644a02d2eb612905`.
- Use independent LSM credential storage and policy-aware SID tracking.
- Resolve actual partition metadata without a stale device-number cache.
- Keep boot, recovery and update-related access available.
- Rate-limit denial diagnostics. Covered root block writes are filtered;
  this does not close every possible privileged storage-write path.

### Release variants

- Provide HyperOS and ColorOS builds with KernelSU Next v3.4.0 / SUSFS v2.3.0,
  plus separate Normal builds without built-in root.
- Leave the permissive branch unchanged.
- Retain existing BBRv3/FQ and Kyber support; do not claim new vendor ZRAM
  capabilities from the boot-only package.

### Validation scope

The pre-version-bump HyperOS KernelSU candidate booted on Rodin with SELinux
enforcing, expected configuration, loaded vendor modules and Baseband-guard
registered. Host tests covered bypass observation equivalence, partition matching
and concurrent credential tracking. The V4.0 release rebuilds require separate
device validation; compile checks are not a guarantee for every ROM.
Inherited vendor-module CRC mismatch relaxation remains a known limitation.
No controlled gaming, long-duration battery or temperature improvement is claimed.
