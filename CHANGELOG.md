# Changelog

## V4.6

This release is published across six dedicated editions: HyperOS KernelSU-Next, HyperOS BakaSU, HyperOS Normal, ColorOS KernelSU-Next, ColorOS BakaSU, and ColorOS Normal. Features listed below apply to all editions unless specifically noted otherwise.

### Linux 6.6 stable updates and Android GKI

- Upgraded the kernel base to Linux 6.6.144 LTS Android GKI.
- Backported MediaTek OPP (Operating Performance Points) and Energy Model stability fixes from upstream Linux, eliminating memory leaks during multi-regulator supply parsing, preventing integer overflow in high-frequency table calculation, and resolving race conditions during concurrent OPP dynamic additions.
- Integrated upstream EEVDF and deadline priority-inheritance (PI) fixes, resolving task reference leaks during deadline migrations and preventing priority-inversion starvation under nested mutex contention.
- Integrated memory hotplug synchronization and race-condition hardening for deferred struct page initialization.
- Backported Google Android 15 GKI common kernel updates while maintaining full KMI symbol compatibility.

### CPU scheduling, EEVDF and Enhanced Schedutil

- Implemented EVONIX Enhanced Schedutil with decoupled asymmetric rate-limiting: 1000 µs ramp-up for immediate frequency response during frame drops, and 4000 µs ramp-down to prevent premature down-clocking during active workloads.
- Implemented a continuous piecewise-linear dynamic frequency headroom curve (+35% at low utilization tapering to +10% near saturation), delivering burst headroom before frame deadlines.
- Corrected CPU governor boot default selection by intercepting MediaTek vendor init's boot-time sysfs write, ensuring Schedutil is enforced as the system default across all CPU policies without requiring userspace helper scripts.
- Restored complete CPU governor freedom for root users, removing unconditional governor locking while preserving user ability to select performance, powersave, conservative, or custom frequency bounds.
- Refactored MediaTek cluster topology configuration to reflect the true all-big-core architecture of the Dimensity 8400-Ultra (8x Cortex-A725 across 3 performance gear groups: 1x 3.25 GHz Prime, 3x 3.0 GHz Gold, 4x 2.0 GHz Silver).
- Tuned EEVDF base scheduling slice to 500 µs normalized base (2.0 ms scaled across 8 cores), providing balanced interactive responsiveness and throughput.

### EVONIX UI Responsiveness Engine

- Introduced kernel-level UI Responsiveness Engine (CONFIG_EVONIX_SCHED_RESPONSIVE).
- Automatically identifies latency-critical user-interface threads (SurfaceFlinger, RenderThread, SystemUI, and foreground application compositor threads) and applies an immediate 20% frequency headroom boost (ui_boost_pct=20).
- Provides instantaneous frame rendering acceleration during touch events and gestures while respecting thermal governor ceilings.
- Fully integrated with platform EAS energy models without inducing unnecessary thermal throttling.

### ADIOS I/O scheduler and Foreground Read Prioritization

- Integrated ADIOS v3.3.0 (Adaptive Dynamic I/O Scheduler) blk-mq scheduler and made it the default I/O scheduler across all compatible block storage queues, including Rodin's UFS 4.0 Multi-Circular Queues (MCQ).
- Introduced EVONIX Foreground Read Prioritization (CONFIG_EVONIX_IO_PRIO) with dedicated Tier-1 queue handling for synchronous reads initiated by foreground UI tasks, drastically reducing app cold-start times and UI stalls during background writeback.
- Hardened ADIOS against dispatch lock contention across hardware queues using lockless queue state checks.
- Eliminated priority inversion and starvation by implementing request-depth tracking and anti-starvation throttling after 8 consecutive dispatches, with strict exemptions for emergency head requests.
- Enforced cross-core memory ordering and cache coherency with explicit memory barriers (smp_mb__after_atomic).
- Added robust multi-tier fallback: if a storage controller does not support ADIOS, the block layer gracefully falls back to Kyber, mq-deadline, or none without kernel warnings or panics.

### Filesystem integrity and F2FS fixes

- Backported 12 critical upstream F2FS integrity and garbage-collection (GC) fixes.
- Hardened atomic write handling to eliminate filesystem corruption during sudden power-off or kernel crash scenarios.
- Fixed checkpoint serialization races and background GC victim selection under high storage fill ratios.
- Resolved block-allocation race conditions in multi-threaded asynchronous I/O paths.
- Addressed stale inode writeback metadata leaks and improved fsync flush latency.

### Memory management, MGLRU and Adaptive Memory Reclaim

- Introduced EVONIX Adaptive Memory Reclaim (CONFIG_EVONIX_MM_ADAPTIVE), proactively monitoring memory zones against a 64 MB cushion (evonix_reclaim_cushion_mb = 64) to wake kswapd gently before free memory hits critical low watermarks, eliminating direct reclaim latency spikes during app launches.
- Preserved Linux 6.6 stock compaction watermark boost factors to maintain reliable buddy allocator high-order page compaction.
- Bound Adaptive Memory Reclaim to system power-management suspend/resume notifiers using system_freezable_power_efficient_wq, ensuring zero background wakeups or battery drain while the device is suspended.
- Enhanced MGLRU (Multi-Gen LRU) proactive aging routines, ensuring reclaim workers remain interruptible during system suspend transitions.
- Fixed locked-memory accounting and refined zRAM swap readahead boundaries.

### Kernel power management and thermal integration

- Retained full MediaTek thermal governor and mitigation framework, ensuring hardware thermal protection limits remain authoritative across all CPU clusters and charging circuits.
- Bound all EVONIX background workers to power-efficient, freezeable workqueues to maximize deep-sleep residency.
- Corrected timer slack inheritance for normal tasks spawned from real-time parents, reducing unnecessary CPU package wakeups.
- Verified deep sleep and suspend/resume cycles on hardware, maintaining minimal standby battery drain.

### Networking and congestion control

- Enabled BBRv3 as the default TCP congestion control algorithm for improved throughput and lower queueing latency on cellular and Wi-Fi networks.
- Enabled CAKE, PIE, and FQ-PIE queue disciplines for bufferbloat mitigation.
- Enabled IP Sets (CONFIG_IP_SET) with hash:ip, hash:net, and hash:mac types.
- Enabled full IPv6 NAT (CONFIG_IP6_NF_NAT, CONFIG_IP6_NF_TARGET_MASQUERADE) and TTL/Hop-Limit manipulation (CONFIG_NETFILTER_XT_TARGET_HL).
- Integrated upstream TCP retransmission lifetime fixes and UDP tunnel segmentation hardening.

### NTSync and Windows synchronization

- Backported the upstream Linux NTSync driver (CONFIG_NTSYNC), exposing /dev/ntsync for fast Windows NT synchronization primitive emulation.
- Replaces high-overhead userspace eventfd synchronization with direct kernel-level primitives for NT mutexes, semaphores, and events.
- Accelerates game execution and improves frame pacing in Wine, Proton, Winlator, and Mobox Windows translation environments.

### Container support and Droidspaces

- Unlocked all kernel prerequisites for Linux containers, Droidspaces, Docker, and rootless Linux distributions without modifying the Google KMI ABI.
- Re-routed System V IPC (sysvsem, sysvshm) structures through unused KMI reserved fields in struct task_struct, achieving zero KMI symbol and structure offset violations.
- Enabled User Namespaces (CONFIG_USER_NS=y) with full UID/GID mapping support (up to 26,926 user namespaces).
- Enabled PID, Network, Mount, UTS, and IPC namespace isolation.
- Enabled POSIX Message Queues (CONFIG_POSIX_MQUEUE) mounted at /dev/mqueue.
- Enabled Virtual Ethernet (CONFIG_VETH) pair creation and MACVLAN network device support.

### Root frameworks (KernelSU-Next, BakaSU, SUSFS v2.4.0, NoMount)

- KernelSU-Next editions (hyperos/ksu-susfs, coloros/ksu-susfs):
  - Integrated KernelSU-Next v3.4.2 (commit 26499d89fb).
  - Integrated SUSFS v2.4.0 with comprehensive kernel-level path, mount, and cgroup hiding.
  - Integrated NoMount subsystem (2,779 lines in fs/nomount/), providing high-performance in-kernel VFS path redirection as a lightweight alternative to overlay mounts for systemless modules.
- BakaSU editions (bakasu/hyperos-susfs, bakasu/coloros-susfs):
  - Integrated BakaSU with dedicated SUSFS 2.4.0 SELinux bridge (ksu_security_context_to_sid_with_policy) for stealth operation under enforcing SELinux.
  - Integrated SUSFS v2.4.0 and NoMount subsystem.
- Normal editions (hyperos/normal, coloros/normal):
  - Completely rootless standard GKI builds with zero root hooks, zero symbols (0 ksu_*, 0 susfs_*, 0 nomount_*), and 100% stock security architecture for users requiring pure system integrity.

### Platform compatibility (HyperOS and ColorOS)

- HyperOS editions:
  - Native integration with Xiaomi HyperOS 4 / Android 17 userspace.
  - Full compatibility with MediaTek Dimensity 8400-Ultra vendor modules and proprietary services.
  - Verified hardware charging-bypass backend via standard OEM interfaces.
- ColorOS editions:
  - Dedicated MT6899 OPlus SchedAssist integration with isolated scheduler data structures.
  - FrameBoost integration with corrected cluster ordering and CPUFreq-aware initialization.
  - Integrated OPlus MIDAS v2, process-reclaim helpers, and HealthInfo Lite CPU-loading support.
  - Full compatibility with ColorOS port ROMs for Rodin.

### Build system, compiler optimizations and testing infrastructure

- Compiled using official Android Clang r614150 (Clang 23.0.1) with LLVM LLD linker.
- Configured with full ThinLTO (Thin Link-Time Optimization) and AutoFDO (Feedback-Directed Optimization) profile data.
- Built under Google Kleaf/Bazel build environment for reproducible release images.
- Comprehensive selftest test harness covering scheduler headroom, governor switching, ADIOS dispatch ordering, and container namespace isolation.

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
