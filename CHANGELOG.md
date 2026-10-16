# Changelog

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
