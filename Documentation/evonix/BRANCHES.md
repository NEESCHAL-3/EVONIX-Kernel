# Branch and tag policy

## V4.6.1 release branches

| Branch | ROM family | Root configuration | SELinux |
| --- | --- | --- | --- |
| `hyperos/ksu-susfs` | HyperOS | KernelSU-Next v3.4.2 + SUSFS v2.4.0 + NoMount | Enforcing |
| `bakasu/hyperos-susfs` | HyperOS | BakaSU + SUSFS v2.4.0 + NoMount | Enforcing |
| `hyperos/normal` | HyperOS | None built in (Pure standard GKI) | Enforcing |
| `coloros/ksu-susfs` | ColorOS port | KernelSU-Next v3.4.2 + SUSFS v2.4.0 + NoMount | Enforcing |
| `bakasu/coloros-susfs` | ColorOS port | BakaSU + SUSFS v2.4.0 + NoMount | Enforcing |
| `coloros/normal` | ColorOS port | None built in (Pure standard GKI) | Enforcing |

The default repository branch is `hyperos/ksu-susfs`. Shared core improvements are maintained across all six branches while preserving ROM-specific optimizations and root frameworks.

`coloros/ksu-susfs-permissive` is deliberately unchanged and excluded from the V4.6.1 release. Historical branches and tags are retained as checkpoints, not interchangeable release channels.

## Release provenance

The `v4.6.1` tag identifies the official EVONIX Kernel V4.6.1 release. Each flashable AnyKernel3 ZIP corresponds to its compiled release commit:

| ZIP edition | Compiled source commit | Release Archive |
| --- | --- | --- |
| HyperOS KernelSU Next + SUSFS | `2229359ad159d` | `EVONIX-V4.6.1-HyperOS-KSU-SUSFS-rodin.zip` |
| HyperOS BakaSU + SUSFS | `03101d5f4211b` | `EVONIX-V4.6.1-HyperOS-BakaSU-SUSFS-rodin.zip` |
| HyperOS Normal | `cba2b6944d668` | `EVONIX-V4.6.1-HyperOS-Normal-rodin.zip` |
| ColorOS KernelSU Next + SUSFS | `aeea369e5e930` | `EVONIX-V4.6.1-ColorOS-KSU-SUSFS-rodin.zip` |
| ColorOS BakaSU + SUSFS | `0b18a85099923` | `EVONIX-V4.6.1-ColorOS-BakaSU-SUSFS-rodin.zip` |
| ColorOS Normal | `1954eae3edc99` | `EVONIX-V4.6.1-ColorOS-Normal-rodin.zip` |

Release-documentation commits advance branch HEADs but do not change compiled kernel sources or binary integrity. Historical `v4.0`, `v4.5`, and `v4.6` tags remain immutable.
