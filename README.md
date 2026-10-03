# EVONIX Kernel

EVONIX is an Android 15 / Linux 6.6.142 kernel for Poco X7 Pro (`rodin`).
V4.0 provides separate enforcing HyperOS and ColorOS builds, with either
KernelSU Next + SUSFS or no built-in root.

## V4.0

- Cortex-A725 instruction scheduling with the existing arm64 ISA.
- Rodin EEVDF base slice of 500 microseconds; stock frequency governors retained.
- Power-efficient eligible workqueues, lazy RCU callbacks and deferrable KFENCE.
- Shared OEM charge-pause backend replacing the previous bypass implementation.
- Baseband-guard partition-write filtering with policy-aware credential tracking.
- KernelSU Next v3.4.0 and SUSFS v2.3.0 in KSU variants.
- Existing BBRv3/FQ and Kyber support retained.

See [CHANGELOG.md](CHANGELOG.md) for changes and validation scope.

## Choose your build

| ROM family | Built-in root | Branch |
| --- | --- | --- |
| HyperOS | KernelSU Next + SUSFS | `hyperos/ksu-susfs` |
| HyperOS | None | `hyperos/normal` |
| ColorOS port | KernelSU Next + SUSFS | `coloros/ksu-susfs` |
| ColorOS port | None | `coloros/normal` |

The default branch is `hyperos/ksu-susfs`. The permissive branch is excluded
from V4.0 and was not updated.

## Build and integration

This repository is the `common/` project in an Android Kleaf workspace,
not a standalone build environment. Follow
[BUILDING.md](Documentation/evonix/BUILDING.md), including the pinned
Clang 23 toolchain, ThinLTO and AutoFDO profile.

Release ZIPs replace the kernel Image through AnyKernel; they do not replace
vendor_boot, recovery or vendor modules. OEM bypass operation, supported
interfaces and measurement limits are documented in
[BYPASS_CHARGING.md](Documentation/evonix/BYPASS_CHARGING.md).
Charging policy restoration is a userspace responsibility.

## Security and testing

The four release branches enforce SELinux. Inherited vendor-module
version-check relaxation remains a security and compatibility limitation;
see [SECURITY.md](SECURITY.md). Baseband-guard does not eliminate all
possible privileged partition-write paths.

A successful compile is not a guarantee of ROM compatibility, better FPS,
lower temperature or lower battery consumption. Keep a known-good boot image
and a recovery path before flashing.

See [architecture](Documentation/evonix/ARCHITECTURE.md),
[branches](Documentation/evonix/BRANCHES.md) and [CONTRIBUTING.md](CONTRIBUTING.md).
The kernel is GPL-2.0 licensed; see [COPYING](COPYING).
