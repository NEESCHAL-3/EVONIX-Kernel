# Branch and tag policy

## V4.0 release branches

| Branch | ROM family | Root | SELinux |
| --- | --- | --- | --- |
| `hyperos/ksu-susfs` | HyperOS | KernelSU Next + SUSFS | Enforcing |
| `hyperos/normal` | HyperOS | None built in | Enforcing |
| `coloros/ksu-susfs` | ColorOS port | KernelSU Next + SUSFS | Enforcing |
| `coloros/normal` | ColorOS port | None built in | Enforcing |

The repository default is `hyperos/ksu-susfs`. Shared changes are ported
without dropping family-specific compatibility code or enabling root on Normal.

`coloros/ksu-susfs-permissive` is deliberately unchanged and excluded from
the V4.0 release. Historical branches and tags are retained as checkpoints,
not interchangeable release channels.

## Release provenance

The `v4.0` tag identifies the default-branch release source. Each ZIP has its
own branch commit recorded with the release artifacts. A tag on one branch
does not imply that other branch tips have identical trees.

Published tags are immutable. Advance branches with signed-off commits; never
force-push history to tidy old messages. Keep experiments separate and state
whether a build was merely compiled or actually tested on a device.
