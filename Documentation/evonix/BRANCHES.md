# Branch and tag policy

## V4.6 release branches

| Branch | ROM family | Root configuration | SELinux |
| --- | --- | --- | --- |
| `hyperos/ksu-susfs` | HyperOS | KernelSU-Next v3.4.2 + SUSFS v2.4.0 + NoMount | Enforcing |
| `bakasu/hyperos-susfs` | HyperOS | BakaSU + SUSFS v2.4.0 + NoMount | Enforcing |
| `hyperos/normal` | HyperOS | None built in (Pure standard GKI) | Enforcing |
| `coloros/ksu-susfs` | ColorOS port | KernelSU-Next v3.4.2 + SUSFS v2.4.0 + NoMount | Enforcing |
| `bakasu/coloros-susfs` | ColorOS port | BakaSU + SUSFS v2.4.0 + NoMount | Enforcing |
| `coloros/normal` | ColorOS port | None built in (Pure standard GKI) | Enforcing |

The default repository branch is `hyperos/ksu-susfs`. Shared core improvements are maintained across all six branches while preserving ROM-specific optimizations and root frameworks.

`coloros/ksu-susfs-permissive` is deliberately unchanged and excluded from the V4.6 release. Historical branches and tags are retained as checkpoints, not interchangeable release channels.

## Release provenance

The `v4.6` tag identifies the official EVONIX Kernel V4.6 release. Each flashable AnyKernel3 ZIP is built from its corresponding release commit:

| ZIP edition | Compiled source commit | Release SHA-256 |
| --- | --- | --- |
| HyperOS KernelSU Next + SUSFS | `8fabe278170b5` | `41993353f8cd32be283e07e5f1201d693f3f999e64b33446af682637f3f776f8` |
| HyperOS BakaSU + SUSFS | `b324ec6f40983` | `69f22d70923b3a53ec37b8eea3af0d2fe4789dcccf5334df4ab407b241989fbf` |
| HyperOS Normal | `b8c2acc398b07` | `eb8fef695047218fd772eb60e5198a87b3e7bac5c4109bb00378a8494ebda458` |
| ColorOS KernelSU Next + SUSFS | `8473f2064f962` | `ec111826ce51663b147491cb5645d641d20392166ba90b67f78bd2dc10d38242` |
| ColorOS BakaSU + SUSFS | `806c240d45b75` | `5ed75d0e3a362dae6759d2d4bad5b5ae7ac78fb9792cd92a7d9bb63fb4a7d26e` |
| ColorOS Normal | `a17a9bb153581` | `428ea74e899f3e6634b3f89e8ae3b0a3e9e48562613f9fca39eba27bc328295e` |

Release-documentation commits advance branch HEADs but do not change compiled kernel sources or binary integrity. Historical `v4.0` and `v4.5` tags remain immutable.
