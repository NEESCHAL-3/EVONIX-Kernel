# Install EVONIX V4.6

For POCO X7 Pro / Redmi Turbo 4 (`rodin`). Download release packages from the
[V4.6 release](https://github.com/NEESCHAL-3/EVONIX-Kernel/releases/tag/v4.6).

## Choose the right ZIP

| Your ROM | Edition | Package filename | SHA-256 Checksum |
| --- | --- | --- | --- |
| HyperOS | KernelSU Next + SUSFS | `EVONIX-V4.6-HyperOS-KSU-SUSFS-rodin.zip` | `41993353f8cd32be283e07e5f1201d693f3f999e64b33446af682637f3f776f8` |
| HyperOS | BakaSU + SUSFS | `EVONIX-V4.6-HyperOS-BakaSU-SUSFS-rodin.zip` | `69f22d70923b3a53ec37b8eea3af0d2fe4789dcccf5334df4ab407b241989fbf` |
| HyperOS | Normal (no built-in root) | `EVONIX-V4.6-HyperOS-Normal-rodin.zip` | `eb8fef695047218fd772eb60e5198a87b3e7bac5c4109bb00378a8494ebda458` |
| ColorOS port | KernelSU Next + SUSFS | `EVONIX-V4.6-ColorOS-KSU-SUSFS-rodin.zip` | `ec111826ce51663b147491cb5645d641d20392166ba90b67f78bd2dc10d38242` |
| ColorOS port | BakaSU + SUSFS | `EVONIX-V4.6-ColorOS-BakaSU-SUSFS-rodin.zip` | `5ed75d0e3a362dae6759d2d4bad5b5ae7ac78fb9792cd92a7d9bb63fb4a7d26e` |
| ColorOS port | Normal (no built-in root) | `EVONIX-V4.6-ColorOS-Normal-rodin.zip` | `428ea74e899f3e6634b3f89e8ae3b0a3e9e48562613f9fca39eba27bc328295e` |

Normal, BakaSU, and KernelSU Next editions include the same core kernel optimizations.
Match the ZIP to your ROM family; do not use the ColorOS ZIP on HyperOS or vice versa.

## Flash

1. Keep a verified backup of your current working boot image and a method to restore it.
2. Download the matching ZIP and verify its SHA-256 checksum against `SHA256SUMS`.
3. Flash the ZIP using a compatible custom recovery (e.g. TWRP / OrangeFox) or an AnyKernel-capable kernel flashing application. Do not attempt to install the ZIP as a Magisk or KernelSU module.
4. Reboot into Android. Root edition users can install the corresponding KernelSU Next or BakaSU manager application.

These AnyKernel3 packages replace the kernel Image on the boot partition. They do not modify ROM system partitions, recovery, or vendor modules. No user data format is required.

## Bypass charging

The built-in bypass charging backend is shared by both ROM families. Use Rodin Essential or a compatible privileged ROM service to control charging bypass state. The request starts disabled after boot; the controlling service restores saved user selections.

See [bypass charging](BYPASS_CHARGING.md) for the interface specification and sysfs controls.

## Restore your previous kernel

Use your recovery or fastboot to restore your boot partition backup or reinstall the stock boot image from your installed ROM build. Do not format data as a kernel rollback step.
