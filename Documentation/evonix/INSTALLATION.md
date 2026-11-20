# Install EVONIX V4.6

For POCO X7 Pro / Redmi Turbo 4 (`rodin`). Download release packages from the
[V4.6 release](https://github.com/NEESCHAL-3/EVONIX-Kernel/releases/tag/v4.6).

## Choose the right ZIP

| ROM | Edition | Package filename |
| --- | --- | --- |
| HyperOS | KernelSU Next + SUSFS | `EVONIX-V4.6-HyperOS-KSU-SUSFS-rodin.zip` |
| HyperOS | BakaSU + SUSFS | `EVONIX-V4.6-HyperOS-BakaSU-SUSFS-rodin.zip` |
| HyperOS | Normal (no built-in root) | `EVONIX-V4.6-HyperOS-Normal-rodin.zip` |
| ColorOS port | KernelSU Next + SUSFS | `EVONIX-V4.6-ColorOS-KSU-SUSFS-rodin.zip` |
| ColorOS port | BakaSU + SUSFS | `EVONIX-V4.6-ColorOS-BakaSU-SUSFS-rodin.zip` |
| ColorOS port | Normal (no built-in root) | `EVONIX-V4.6-ColorOS-Normal-rodin.zip` |

Normal, BakaSU, and KernelSU Next editions include the same core kernel optimizations.
Match the ZIP to your ROM family; do not use the ColorOS ZIP on HyperOS or vice versa.

## Flash

1. Keep a verified backup of your current working boot image and a method to restore it.
2. Download the matching ZIP for your ROM and root configuration.
3. Flash the ZIP using a compatible custom recovery (e.g. TWRP / OrangeFox) or an AnyKernel-capable kernel flashing application. Do not attempt to install the ZIP as a Magisk or KernelSU module.
4. Reboot into Android. Root edition users can install or open the corresponding KernelSU Next or BakaSU manager application.

These AnyKernel3 packages replace the kernel Image on the boot partition. They do not modify ROM system partitions, recovery, or vendor modules. No user data format is required.

## Bypass charging

The built-in bypass charging backend is shared by both ROM families. Use Rodin Essential or a compatible privileged ROM service to control charging bypass state. The request starts disabled after boot; the controlling service restores saved user selections.

See [bypass charging](BYPASS_CHARGING.md) for the interface specification and sysfs controls.

## Restore your previous kernel

Use your recovery or fastboot to restore your boot partition backup or reinstall the stock boot image from your installed ROM build. Do not format data as a kernel rollback step.
