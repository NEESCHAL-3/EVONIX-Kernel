# EVONIX KERNEL V4.6 — FEATURE VERIFICATION MATRIX

**Target Device:** POCO X7 Pro / Redmi Turbo 4 (`rodin` / `24129RT7CC`)  
**Platform:** MediaTek Dimensity 8400-Ultra (`MT6899`) — All-Big-Core (8× Cortex-A725)  
**Baseline:** Linux 6.6.144 Android GKI (LTS)  
**Verification Audit Date:** 2026-11-20  
**Developer:** NEESCHAL  

---

## 1. Verification Standards & Definitions

* **[SOURCE VERIFIED]**: Code presence, control flow, and syntax verified in source files.
* **[CONFIG VERIFIED]**: Config presence confirmed in `evonix.config`, `defconfig`, and extracted `embedded.config`.
* **[BINARY VERIFIED]**: Symbol existence, layout, or absence confirmed inside compiled `vmlinux` / `Image`.
* **[HOST-TEST VERIFIED]**: Verified via automated Python selftests in `tools/testing/selftests/`.
* **[RUNTIME VERIFIED]**: Executed and verified directly on live Rodin physical device via root ADB.
* **[PERFORMANCE MEASURED]**: Quantitatively measured using benchmarking tools.
* **[NOT YET VERIFIED]**: A code path or combination not yet evaluated on live hardware.

---

## 2. Complete Subsystem Feature Verification Table

| # | Feature / Mechanism Name | Source Files / Scope | Kconfig / Key Symbols | Target Variants | Verification Level | Hardware Validation | Known Limitations & Outstanding Checks |
|---|---|---|---|---|---|---|---|
| **1** | **Schedutil Boot Default Guard** | `drivers/cpufreq/cpufreq.c` (`store_scaling_governor`) | `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Booted cold to `schedutil` on policies 0, 4, 7) | Remaps vendor init `sugov_ext` write; legitimate user switches preserved. |
| **2** | **Root Governor Freedom** | `drivers/cpufreq/cpufreq.c` | Removal of `CONFIG_EVONIX_GOVERNOR_ENFORCE` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Switched to `performance`/`schedutil` with 0 `-EPERM`) | User must have root context to write to `scaling_governor`. |
| **3** | **Decoupled Schedutil Rate Limiting** | `kernel/sched/cpufreq_schedutil.c` | `up_rate_limit_us=1000`, `down_rate_limit_us=4000` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Queried sysfs on policies 0, 4, 7) | Requires `CONFIG_EVONIX_SCHEDUTIL=y`. |
| **4** | **Dynamic Headroom Curve** | `kernel/sched/cpufreq_schedutil.c` (`evonix_map_util_perf`) | `headroom_mode=1` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Verified inlined in `get_next_freq`) | Optimized for 8× A725; not tailored for little cores (none present). |
| **5** | **UI Responsiveness Engine** | `kernel/sched/cpufreq_schedutil.c` | `CONFIG_EVONIX_SCHED_RESPONSIVE=y`, `ui_boost_pct=20` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Queried active in sysfs) | Respects thermal limits and policy max frequencies. |
| **6** | **Idle Fast Down Elimination** | `kernel/sched/cpufreq_schedutil.c` | `idle_fast_down=0` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Verified in sysfs) | Prevents PLL frequency thrashing across sub-ms idle periods. |
| **7** | **ADIOS v3.3.0 blk-mq Scheduler** | `block/adios.c` (2,064 lines) | `CONFIG_MQ_IOSCHED_ADIOS=y`, `adios_dispatch_request` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`sda`, `sdb` select `adios`, 8 MCQ queues active) | Linux 6.6 LTS API adapted. |
| **8** | **ADIOS Universal Default Selection** | `block/elevator.c` (`elevator_get_default`) | `CONFIG_MQ_IOSCHED_DEFAULT_ADIOS=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`sda` & `sdb` default to `[adios]`) | Overrides `BLK_MQ_F_NO_SCHED_BY_DEFAULT` safely on compatible queues. |
| **9** | **ADIOS Safe Multi-Tier Fallback** | `block/elevator.c` (`elevator_init_mq`) | `elevator_put()` error recovery | All 6 Variants | **[HOST-TEST VERIFIED]** | **Host Tested** (`evonix_adios_audit_test.py` PASSED) | Falls back cleanly to Kyber $\to$ mq-deadline $\to$ none on error. |
| **10** | **ADIOS Concurrency Lockless Probe** | `block/adios.c` (`dispatch_from_pq`) | Lockless `eval_adios_state` | All 6 Variants | **[BINARY VERIFIED]** | **Rodin Tested** (Cycled schedulers without hangs) | Probes atomic state bit before acquiring `pq_lock`. |
| **11** | **ADIOS Memory Ordering Hardening** | `block/adios.c` (`set_adios_state`, `get_adios_state`) | `smp_mb__after_atomic()`, `atomic_read_acquire()` | All 6 Variants | **[BINARY VERIFIED]** | **Rodin Tested** (UFS 4.0 storage stable under concurrent load) | Enforces cross-core visibility across 8 MCQ queues. |
| **12** | **Foreground Read Prioritization** | `block/adios.c` (`insert_request_post_stability`) | `CONFIG_EVONIX_IO_PRIO=y`, `read_priority=8` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`read_priority` active in sysfs on `sdc`) | Sync reads with `REQ_PRIO`/`REQ_META`/RT/BE $\le 2$ go to Tier-1 queue. |
| **13** | **ADIOS Anti-Starvation & Emergency Exemption** | `block/adios.c` (`dispatch_from_pq`) | `pq1_dispatch_count >= 8` yield | All 6 Variants | **[HOST-TEST VERIFIED]** | **Host Tested** (Audit test verified anti-starvation logic) | Tier-0 emergency head requests strictly exempt from quota. |
| **14** | **EVONIX Adaptive Memory Reclaim** | `mm/evonix_mm.c` (135 lines) | `CONFIG_EVONIX_MM_ADAPTIVE=y`, `evonix_mm_init` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Active in `/proc/sys/vm/`, cushion = 64 MB) | Proactively wakes kswapd when free memory drops below high+cushion. |
| **15** | **Adaptive Reclaim PM Lifecycle** | `mm/evonix_mm.c` (`evonix_mm_pm_notify`) | `system_freezable_power_efficient_wq`, PM notifier | All 6 Variants | **[BINARY VERIFIED]** | **Rodin Tested** (Device suspends and resumes cleanly) | Cancels work on `PM_SUSPEND_PREPARE`, resumes on `PM_POST_SUSPEND`. |
| **16** | **Stock Watermark Boost Preservation** | `mm/page_alloc.c` | Linux 6.6 stock watermark boost | All 6 Variants | **[HOST-TEST VERIFIED]** | **Host Tested** (`evonix_memory_phase3_test.py` PASSED) | Preserves buddy allocator compaction while preventing direct reclaim stalls. |
| **17** | **NTSync NT Synchronization Driver** | `drivers/misc/ntsync.c`, `include/uapi/linux/ntsync.h` | `CONFIG_NTSYNC=y`, `ntsync_char_ioctl` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`/dev/ntsync` present, major 10, minor 258) | Provides kernel-level mutexes/events/semaphores for Wine/Proton. |
| **18** | **Droidspaces Zero-KMI `task_struct` Mapping** | `include/linux/sched.h` | `ANDROID_KABI_USE(3, struct sysv_sem)` | All 6 Variants | **[BINARY VERIFIED]** | **Rodin Tested** (0 KMI violations, camera & Wi-Fi operational) | Re-maps SysV IPC into KABI reserve slots, preserving binary ABI. |
| **19** | **User Namespaces (`CONFIG_USER_NS`)** | `kernel/user_namespace.c` | `CONFIG_USER_NS=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Verified 26,926 max user namespaces on device) | Allows rootless container execution (Podman, Docker, Droidspaces). |
| **20** | **System V IPC (`CONFIG_SYSVIPC`)** | `ipc/` | `CONFIG_SYSVIPC=y`, `CONFIG_SYSVIPC_SYSCTL=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Tested live binary `shmget` + `shmat` + `semget`) | Full shared memory, semaphores, and message queues active. |
| **21** | **POSIX Message Queues** | `ipc/mqueue.c` | `CONFIG_POSIX_MQUEUE=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Sysfs active in `/proc/sys/fs/mqueue/`) | Required by modern Linux applications in container environments. |
| **22** | **Linux Namespaces (UTS, IPC, NET, PID)** | `kernel/` | `CONFIG_UTS_NS`, `CONFIG_IPC_NS`, `CONFIG_NET_NS`, `CONFIG_PID_NS` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Verified `unshare()` and `/proc/$$/ns/*`) | Full process, network, and hostname isolation for containers. |
| **23** | **Virtual Ethernet (veth) & MACVLAN** | `drivers/net/veth.c`, `drivers/net/macvlan.c` | `CONFIG_VETH=y`, `CONFIG_MACVLAN=y` | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Created & destroyed `veth0 <-> veth1` pair live) | Virtual network interfaces for container communication. |
| **24** | **CAKE Queue Discipline** | `net/sched/sch_cake.c` | `CONFIG_NET_SCH_CAKE=y` | All 6 Variants | **[CONFIG VERIFIED]** | **Host Tested** (Verified in all 6 `embedded.config`) | Eliminates bufferbloat on mobile and Wi-Fi networks. |
| **25** | **FQ-PIE & PIE Schedulers** | `net/sched/sch_pie.c` | `CONFIG_NET_SCH_FQ_PIE=y`, `CONFIG_NET_SCH_PIE=y` | All 6 Variants | **[CONFIG VERIFIED]** | **Host Tested** (Verified in all 6 `embedded.config`) | Fast packet queueing for latency-sensitive gaming. |
| **26** | **IP Set Packet Filtering** | `net/netfilter/ipset/` | `CONFIG_IP_SET=y` | All 6 Variants | **[CONFIG VERIFIED]** | **Host Tested** (Verified in all 6 `embedded.config`) | $O(1)$ hash table matching for firewall rules and container NAT. |
| **27** | **IPv6 NAT Masquerading** | `net/ipv6/netfilter/` | `CONFIG_IP6_NF_NAT=y` | All 6 Variants | **[CONFIG VERIFIED]** | **Host Tested** (Verified in all 6 `embedded.config`) | Native IPv6 network address translation for container bridge interfaces. |
| **28** | **Hop Limit / TTL Mangling** | `net/netfilter/xt_HL.c` | `CONFIG_NETFILTER_XT_TARGET_HL=y` | All 6 Variants | **[CONFIG VERIFIED]** | **Host Tested** (Verified in all 6 `embedded.config`) | Allows hop-limit modification for mobile tethering. |
| **29** | **KernelSU-Next v3.4.2** | `drivers/kernelsu/` | `CONFIG_KSU=y` (tag: `v3.4.2`) | KSU Variants | **[BINARY VERIFIED]** | **Host Tested** (Compiled cleanly into `vmlinux`) | Supported on `hyperos-ksu-susfs` and `coloros-ksu-susfs`. |
| **30** | **BakaSU Integration** | `drivers/kernelsu/` (BakaSU branch) | `CONFIG_BAKASU=y` | BakaSU Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`su` binary active, context `u:r:ksu:s0`) | Supported on `bakasu-hyperos-susfs` and `bakasu-coloros-susfs`. |
| **31** | **SUSFS v2.4.0 Integration** | `fs/susfs.c`, `include/linux/susfs.h` | `CONFIG_KSU_SUSFS=y`, `susfs_init` | Rooted Variants | **[BINARY VERIFIED]** | **Rodin Tested** (Stealth path and mount hiding active) | Supported on all 4 rooted variants. |
| **32** | **NoMount Path Redirection** | `fs/nomount/nomount.c` (2,234 lines) | `CONFIG_NOMOUNT=y`, `nomount_init` | Rooted Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (dmesg verified injection of Leica camera module) | In-kernel VFS redirection replacing overlay mounts. |
| **33** | **Pure Rootless Isolation** | Clean source tree | 0 KSU, 0 SUSFS, 0 NoMount | Normal Variants | **[BINARY VERIFIED]** | **Host Tested** (`nm` and `extract-ikconfig` verified 100% clean) | Guaranteed 0 root backdoors on `hyperos-normal` and `coloros-normal`. |
| **34** | **F2FS Atomic-Write & GC Backports** | `fs/f2fs/` (12 backported patches) | Linux 6.9/6.10 backports | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (UFS 4.0 `/data` partition mounted and healthy) | Eliminates atomic-write commit races and resize corruption. |
| **35** | **MediaTek OPP Stability Patches** | `drivers/opp/` | Supply parsing, overflow, lookup race | All 6 Variants | **[BINARY VERIFIED]** | **Rodin Tested** (OPP frequency tables loaded properly on all cores) | Prevents OPP table memory leaks and integer overflow. |
| **36** | **All-Big-Core Hardware Awareness** | Topology configuration & DTSI | 8× Cortex-A725 (MT6899) | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (`0-7` online, 0 offline, 3 frequency domains) | Policy 0: 4× 2.10 GHz, Policy 4: 3× 3.00 GHz, Policy 7: 1× 3.25 GHz. |
| **37** | **Clang r614150 + ThinLTO + AutoFDO** | Toolchain hermetic build | Clang 23.0.1, ThinLTO, AutoFDO | All 6 Variants | **[BINARY VERIFIED]** | **Host Tested** (All 6 images compiled and verified) | Maximum code density and compiler profile optimization. |
| **38** | **AnyKernel3 ROM Preflight Protection** | `tools/ak3-core.sh`, `anykernel.sh` | ROM getprop signature checks | All 6 Variants | **[RUNTIME VERIFIED]** | **Rodin Tested** (Flashed cleanly in OrangeFox Recovery) | Rejects cross-flashing between HyperOS and ColorOS environments. |

---

## 3. Physical Hardware Validation vs. Host Status

| Variant Slug | ROM Target | Root Framework | Source Git Commit | Image Size | ZIP SHA-256 | Automated Tests | Physical Hardware Boot Tested |
|---|---|---|---|---|---|---|---|
| **`hyperos-ksu-susfs`** | Xiaomi HyperOS 4 | KernelSU-Next + SUSFS + NoMount | `8fabe27817` | 37.62 MB | `41993353f8cd...` | 7/7 PASSED | **Host / Binary Verified** |
| **`bakasu-hyperos-susfs`** | Xiaomi HyperOS 4 | BakaSU + SUSFS v2.4.0 + NoMount | `b324ec6f40` | 37.49 MB | `69f22d70923b...` | 7/7 PASSED | **YES — Boot & Runtime Verified on Rodin** |
| **`hyperos-normal`** | Xiaomi HyperOS 4 | None (Pure Rootless GKI) | `b8c2acc398` | 37.24 MB | `eb8fef695047...` | 7/7 PASSED | **Host / Binary Verified** |
| **`coloros-ksu-susfs`** | ColorOS Port | KernelSU-Next + SUSFS + NoMount | `8473f2064f` | 38.08 MB | `ec111826ce51...` | 7/7 PASSED | **Host / Binary Verified** |
| **`bakasu-coloros-susfs`** | ColorOS Port | BakaSU + SUSFS v2.4.0 + NoMount | `806c240d45` | 38.02 MB | `5ed75d0e3a36...` | 7/7 PASSED | **Host / Binary Verified** |
| **`coloros-normal`** | ColorOS Port | None (Pure Rootless GKI) | `a17a9bb153` | 37.65 MB | `428ea74e899f...` | 7/7 PASSED | **Host / Binary Verified** |

---

## 4. Known Limitations & Outstanding Testing Scope

1. **Physical Flashing of Non-BakaSU Variants**: While all six variants compile cleanly with identical core EVONIX mechanisms and 0 KMI violations, only `EVONIX-V4.6-HyperOS-BakaSU-SUSFS-rodin.zip` has been flashed and runtime-tested on physical Rodin hardware. The remaining five variants are binary-verified and host-tested.
2. **ColorOS Port Physical Validation**: Tested against ColorOS ROM checks and build extraversions, but physical boot on a Rodin running a ColorOS port remains open for real-world user confirmation.
3. **Synthetic Performance Profiling**: While latency guard, proactive reclaim, and asymmetric rate-limiting mechanisms are verified active in the kernel, formal FPS stability and battery efficiency delta benchmarks should be gathered over multi-day usage cycles.
