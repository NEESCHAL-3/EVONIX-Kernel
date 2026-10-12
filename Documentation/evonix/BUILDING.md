# Building EVONIX V4.5 — ColorOS

## Workspace

Use a 64-bit Linux host with Git, Python 3, repo and standard Android kernel
build dependencies. Allow substantial disk space for the manifest and outputs.

```bash
mkdir evonix-workspace
cd evonix-workspace
repo init -u https://android.googlesource.com/kernel/manifest -b common-android15-6.6-lts
repo sync -c -j8
cd common
git remote add evonix https://github.com/NEESCHAL-3/EVONIX-Kernel.git
git fetch evonix coloros/normal
git switch -c evonix-coloros-normal --track evonix/coloros/normal
cd ..
```

Choose the matching branch from [BRANCHES.md](BRANCHES.md) instead when needed.
Use a new local branch name if the example name already exists. For an existing
remote, use `git remote set-url` instead of adding it again.

## Toolchain and build

From `common/`, follow the repository's `setup-clang23.sh` toolchain setup.
The build requires Android Clang 23.0.1, build 16311247 / r614150, located at
`prebuilts/clang/host/linux-x86/clang-r614150` in the workspace.
Do not change `build.config.constants` from r510928: that is the Kleaf
bootstrap identity, not the selected kernel compiler.

```bash
cd common
EVONIX_JOBS=8 bash build-evonix.sh ../out/evonix-coloros-normal-v4.5
```

The current candidate selects O2, with ThinLTO and AutoFDO retained. The O3
candidate reached Android startup but failed the networking BPF loader's
trivial program-load probe with EINVAL, triggering `reboot,bpfloader-failed`.
That log establishes the boot failure, not its underlying compiler cause.
That experiment is historical HyperOS evidence, not a ColorOS test result.
ColorOS retains O2 and requires its own build and device verification.
The script validates the compiler, ThinLTO, selected defaults and the bundled
AutoFDO profile before invoking Kleaf. Eight jobs are a starting point,
not a requirement for every host; reduce this on memory-constrained machines.

## Verify

Require a zero exit status and completed distribution step. Inspect the
embedded kernel configuration, release identity, KMI results and output checksums.
Confirm SELinux enforcement configuration, the correct KSU/Normal selection,
and absence of accidental permissive options. Keep generated artifacts outside Git.

The distribution includes kernel images, symbols, ABI data and SBOM outputs.
A boot-only release ZIP does not install rebuilt vendor modules. Keep zRAM and
ZSMALLOC modular (`CONFIG_ZRAM=m`, `CONFIG_ZSMALLOC=m`) and absent from
`modules.builtin`; the ROM continues loading its existing modules. Do not add
replacement `.ko` files or DLKM images to the ZIP. Module-only source fixes are
not delivered by this Image-only package.

The October 11 built-in candidate was rejected after its boot log reported
`zsmalloc: exports duplicate symbol zs_compact (owned by kernel)`, immediately
followed by an init-exit panic. Inspect the vendor ramdisk's first-stage module
dependencies as well as later ROM loaders; later error-tolerant modprobe scripts
do not establish first-stage compatibility. The current build verifier reads
the delivered Image's embedded config and rejects built-in swap drivers.

Before device testing, retain a known-good boot image and recovery path.
Record ROM, slot, branch commit, Image checksum and actual runtime results.
Successful compilation alone is not proof of compatibility or performance gains.

## Native ColorOS integration

Both ColorOS variants retain the merged MT6899 OPlus SchedAssist, FrameBoost,
CPUFreq health, project/device information, MIDAS, memory helpers, HealthInfo Lite
and Waker Identify integration. The common scheduler and CPUFreq bridges must
remain alongside the HyperOS-derived common-kernel fixes; do not replace those
files wholesale with HyperOS copies. The Image verifier requires the native
feature configuration and rejects active Phoenix watchdog/recovery modes.

Normal remains root-free. The KernelSU variant uses Next v3.4.1 and SUSFS v2.3.0.
The permissive ColorOS branch is not part of this update.
