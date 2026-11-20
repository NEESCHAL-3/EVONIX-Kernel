# EVONIX Kernel

EVONIX is a custom Android kernel for the **POCO X7 Pro / Redmi Turbo 4 (rodin)**, built on Android GKI with dedicated **HyperOS** and **ColorOS** editions.

| Specification | Details |
| --- | --- |
| Device | POCO X7 Pro / Redmi Turbo 4 (`rodin`) |
| Processor | MediaTek Dimensity 8400-Ultra (MT6899) |
| CPU architecture | 8× Arm Cortex-A725 |
| Kernel base | Linux 6.6 / Android GKI |
| Toolchain | Android Clang r614150, ThinLTO, AutoFDO |

## Features

- **CPU and scheduling:** EVONIX Enhanced Schedutil, EEVDF and EAS tuning, and configurable UI responsiveness.
- **Storage:** ADIOS I/O scheduler, foreground read prioritization and F2FS reliability improvements.
- **Memory:** Adaptive memory reclaim and improved memory-management behavior.
- **Compatibility:** NTSync, Linux container prerequisites and expanded networking support.
- **Device integration:** HyperOS and ColorOS platform support, including existing charging-bypass functionality.
- **Root options:** KernelSU-Next and BakaSU editions with SUSFS and NoMount, plus editions without built-in root.

For detailed changes, see the [changelog](CHANGELOG.md).

## Editions

| ROM | Edition | Source branch |
| --- | --- | --- |
| HyperOS | KernelSU-Next + SUSFS + NoMount | [`hyperos/ksu-susfs`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/hyperos/ksu-susfs) |
| HyperOS | BakaSU + SUSFS + NoMount | [`bakasu/hyperos-susfs`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/bakasu/hyperos-susfs) |
| HyperOS | Normal (rootless) | [`hyperos/normal`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/hyperos/normal) |
| ColorOS | KernelSU-Next + SUSFS + NoMount | [`coloros/ksu-susfs`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/coloros/ksu-susfs) |
| ColorOS | BakaSU + SUSFS + NoMount | [`bakasu/coloros-susfs`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/bakasu/coloros-susfs) |
| ColorOS | Normal (rootless) | [`coloros/normal`](https://github.com/NEESCHAL-3/EVONIX-Kernel/tree/coloros/normal) |

Select the package that matches your installed ROM and desired root configuration.

## Downloads

Download published kernel packages from [GitHub Releases](https://github.com/NEESCHAL-3/EVONIX-Kernel/releases).

Before flashing, verify the release checksum and keep a known-working boot backup. Follow the [installation guide](Documentation/evonix/INSTALLATION.md) for supported flashing methods.

## Building from source

This repository contains the kernel source used within an Android Kleaf/Bazel workspace.

- [Build instructions](Documentation/evonix/BUILDING.md)
- [Branch information](Documentation/evonix/BRANCHES.md)
- [Architecture and integration](Documentation/evonix/ARCHITECTURE.md)

## Documentation

- [Changelog](CHANGELOG.md)
- [Bypass charging](Documentation/evonix/BYPASS_CHARGING.md)
- [Security](SECURITY.md)
- [Contributing](CONTRIBUTING.md)

## License

Licensed under GPL-2.0. See [COPYING](COPYING).
