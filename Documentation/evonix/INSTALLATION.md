# Install EVONIX V4.5

For POCO X7 Pro / Redmi Turbo 4 (`rodin`). Download from the
[V4.5 release](https://github.com/NEESCHAL-3/EVONIX-Kernel/releases/tag/v4.5).

## Choose the right ZIP

| Your ROM | Edition | File |
| --- | --- | --- |
| HyperOS | KernelSU Next + SUSFS | `EVONIX-HOS-V4.5-KSU-rodin-20261013.zip` |
| HyperOS | Normal, no built-in root | `EVONIX-HOS-V4.5-Normal-rodin-20261013.zip` |
| ColorOS port | KernelSU Next + SUSFS | `EVONIX-COS-V4.5-KSU-rodin-20261013.zip` |
| ColorOS port | Normal, no built-in root | `EVONIX-COS-V4.5-Normal-rodin-20261013.zip` |

Normal and KernelSU Next editions include the same shared kernel improvements.
Match the ZIP to your ROM family; do not use the ColorOS ZIP on HyperOS or vice versa.

## Flash

1. Keep a backup of your current working boot image and a way to restore it.
2. Download the matching ZIP and `SHA256SUMS`. Check the ZIP's SHA-256 against
   the entry with the same filename.
3. Flash the ZIP using a compatible custom recovery or an AnyKernel-capable
   kernel-flashing app. Do not install it as a Magisk or KernelSU module.
4. Reboot to Android. KernelSU Next users can use the matching manager app.

These ZIPs replace the kernel Image, not the ROM, recovery or vendor modules.
No data format is required to install the kernel. Keep your boot backup until
you are satisfied with the installation.

## Bypass charging

The built-in backend is shared by both ROM families. Use Rodin Essential or a
compatible privileged ROM service to control it. The request starts off after
boot; the controlling service restores saved selections.

See [bypass charging](BYPASS_CHARGING.md) for the interface and operation.

## Restore your previous kernel

Use your recovery or the appropriate flashing method to restore the matching
boot backup or reinstall your known-working kernel. Use boot files from your
own installed ROM, not an unrelated ROM package. Do not format data as a
kernel rollback step.
