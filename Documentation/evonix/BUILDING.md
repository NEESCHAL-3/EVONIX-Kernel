# Building EVONIX V4.5

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
git fetch evonix hyperos/ksu-susfs
git switch -c evonix-hyperos-ksu --track evonix/hyperos/ksu-susfs
git submodule update --init KernelSU-Next
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
EVONIX_JOBS=8 bash build-evonix.sh ../out/evonix-v4.5
```

V4.5 uses O2 with ThinLTO and AutoFDO.
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

The build verifier reads the delivered Image's embedded configuration and
rejects built-in swap drivers that conflict with the ROM's existing modules.

Before device testing, retain a known-good boot image and recovery path.
Record ROM, slot, branch commit, Image checksum and actual runtime results.
Successful compilation alone is not proof of compatibility or performance gains.
