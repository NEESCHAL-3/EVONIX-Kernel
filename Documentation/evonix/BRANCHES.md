# Branch and tag policy

## V4.5 release branches

| Branch | ROM family | Root | SELinux |
| --- | --- | --- | --- |
| `hyperos/ksu-susfs` | HyperOS | KernelSU Next + SUSFS | Enforcing |
| `hyperos/normal` | HyperOS | None built in | Enforcing |
| `coloros/ksu-susfs` | ColorOS port | KernelSU Next + SUSFS | Enforcing |
| `coloros/normal` | ColorOS port | None built in | Enforcing |

The repository default is `hyperos/ksu-susfs`. Shared changes are ported
without dropping family-specific compatibility code or enabling root on Normal.

`coloros/ksu-susfs-permissive` is deliberately unchanged and excluded from
the V4.5 release. Historical branches and tags are retained as checkpoints,
not interchangeable release channels.

## Release provenance

The `v4.5` tag identifies the HyperOS KernelSU Next release source, including
the release documentation. Each ZIP has its own compiled branch commit:

| ZIP edition | Compiled source commit |
| --- | --- |
| HyperOS KernelSU Next | `d07794a160afa444641993bbab069a201ec4c9f3` |
| HyperOS Normal | `109a7afc1cdbdd3e83f81c797b40a5b7724e8a36` |
| ColorOS KernelSU Next | `eb8d3bb003ca0505251efea7526e5135181694af` |
| ColorOS Normal | `2a5baf123f430e5f089fc3935a7e236fe12dd456` |

Later release-documentation commits do not change the compiled kernel sources.
The previous `v4.0` tag remains an unchanged historical release.
A tag on one branch does not imply that all four variants have identical trees.

Published tags are immutable. Advance branches with signed-off commits; never
force-push history to tidy old messages. Keep experiments separate and state
whether a build was merely compiled or actually tested on a device.
