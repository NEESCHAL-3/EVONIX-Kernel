# EVONIX Kernel

EVONIX is an Android GKI-based kernel for the **POCO X7 Pro / Redmi Turbo 4 (rodin)**, maintained by **NEESCHAL**. It provides separate HyperOS and ColorOS builds with rooted and rootless editions.

| | |
| --- | --- |
| Current series | V4.6 |
| Kernel | Linux 6.6.144 |
| Platform | MediaTek Dimensity 8400-Ultra (MT6899), 8× Cortex-A725 |
| Toolchain | Android Clang r614150 (23.0.1), ThinLTO, AutoFDO |

## Features

- **CPU and scheduling:** EVONIX Enhanced Schedutil, configurable UI responsiveness, EEVDF and EAS improvements.
- **Storage:** ADIOS v3.3.0, foreground read prioritization and selected F2FS reliability fixes.
- **Memory:** Adaptive reclaim, suspend-aware background work and existing MGLRU improvements.
- **Compatibility:** NTSync, Linux container prerequisites, BBRv3 and additional network queue disciplines and filtering options.
- **Device integration:** Existing charging-bypass support, Baseband-guard and ROM-specific MediaTek/OPlus integrations.
- **Root options:** KernelSU-Next or BakaSU with SUSFS and NoMount, alongside separate builds without built-in root.

See [CHANGELOG.md](CHANGELOG.md) for version history and detailed changes.

## Editions

| ROM | Edition | Branch |
| --- | --- | --- |
| HyperOS | KernelSU-Next + SUSFS + NoMount | `hyperos/ksu-susfs` |
| HyperOS | BakaSU + SUSFS + NoMount | `bakasu/hyperos-susfs` |
| HyperOS | Normal (no built-in root) | `hyperos/normal` |
| ColorOS | KernelSU-Next + SUSFS + NoMount | `coloros/ksu-susfs` |
| ColorOS | BakaSU + SUSFS + NoMount | `bakasu/coloros-susfs` |
| ColorOS | Normal (no built-in root) | `coloros/normal` |

Choose a build matching your installed ROM and preferred root configuration. The permissive development branch is not part of the six-edition release.

## Downloads and installation

Published builds are available under [GitHub Releases](https://github.com/NEESCHAL-3/EVONIX-Kernel/releases).

Before installing, back up your known-working boot image, verify the release ZIP checksum and confirm a recovery path. Use a compatible AnyKernel installer or custom recovery; do not install the ZIP as a root module or flash a package intended for another ROM.

See the [installation guide](Documentation/evonix/INSTALLATION.md) and the release-specific notes for compatibility details.

## Building

This repository contains the kernel `common/` source tree for an Android Kleaf/Bazel workspace; it is not a standalone build environment. Build with the documented toolchain and configuration, then verify the kernel image, embedded configuration and KMI results.

See [building instructions](Documentation/evonix/BUILDING.md) and [branch policy](Documentation/evonix/BRANCHES.md).

## Validation and security

All six V4.6 editions have reported successful host builds and KMI checks. Physical boot and selected runtime checks have been reported for the **HyperOS BakaSU** edition; the other editions have not been individually boot-tested in the supplied release evidence.

Feature availability does not establish a measured increase in FPS, battery life or thermal efficiency. Consult [SECURITY.md](SECURITY.md) for inherited security and vendor-compatibility limitations.

Licensed under GPL-2.0. See [COPYING](COPYING).
