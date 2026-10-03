# EVONIX V4.0 architecture

EVONIX layers Rodin-specific build tuning and ROM compatibility on Android 15
Linux 6.6.142. The four enforcing release branches share the same tuning and
OEM charge-pause backend; ColorOS-specific interfaces remain in ColorOS branches.

## Implementation map

| Area | Paths | Role |
| --- | --- | --- |
| Build tuning | `arch/arm64/Kconfig`, `arch/arm64/Makefile`, `arch/arm64/configs/evonix.config` | Cortex-A725 scheduling, ThinLTO, AutoFDO and release defaults |
| Scheduler | `kernel/sched/fair.c` | Rodin base slice; existing EEVDF and frequency governors retained |
| OEM charge pause | `drivers/misc/evonix_oem_bypass.c`, `fs/sysfs/evonix_supply.c` | Stock navigation charge-pause requests and observed diagnostics |
| ColorOS compatibility | `drivers/misc/evonix_cos/` | Vendor-facing power, display and other compatibility interfaces |
| Networking | `net/ipv4/` | Existing BBRv3/FQ integration |
| Partition-write filtering | `security/baseband-guard/` | LSM credential ancestry and covered block-write filtering |
| Vendor modules | `kernel/module/` | Inherited module-version relaxation; see security limitations |

The older `drivers/misc/evonix_rodin/` controller is not enabled in these
release branches. Its source presence does not imply active CPU, thermal or
charging policy. Kyber is available; userspace and individual queues determine
the active I/O scheduler.

## Runtime boundaries

Eligible power-efficient workqueues can consolidate work; ordinary and
latency-sensitive queues retain their policy. Lazy RCU batches ordinary
callbacks while urgent paths remain available. Deferrable KFENCE avoids waking
an idle CPU solely for a sampling timer.

The kernel does not restore app preferences or replace ROM charging policy
with an independent polling controller. Userspace explicitly requests OEM
charge pause and restores saved choices. Read
[BYPASS_CHARGING.md](BYPASS_CHARGING.md) for ownership, unsupported cases
and measurement limits.

Baseband-guard uses its own standard LSM credential blob and policy-generation
aware SELinux SID cache. It avoids stale global block-device identity caching.
It is not blanket protection against all privileged storage access.

Stock thermal protection remains. No claim of higher FPS, lower idle drain,
lower temperature or universal vendor-module ABI compatibility follows from
these configuration choices. See [SECURITY.md](../../SECURITY.md).
