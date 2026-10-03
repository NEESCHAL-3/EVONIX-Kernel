# Changelog

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
