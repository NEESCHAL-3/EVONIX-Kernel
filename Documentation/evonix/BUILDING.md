# Building EVONIX V4.6

## Workspace

Use a 64-bit Linux host with Git, Python 3, repo, and standard Android kernel build dependencies.

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

Choose the desired branch from [BRANCHES.md](BRANCHES.md) when building alternative editions.

## Toolchain and build

The kernel is compiled with Android Clang 23.0.1 (build r614150 / 16311247) located at `prebuilts/clang/host/linux-x86/clang-r614150`.
Do not change `build.config.constants` from r510928, which serves as the Kleaf bootstrap compiler identity.

```bash
cd common
EVONIX_JOBS=8 bash build-evonix.sh ../out/evonix-v4.6
```

EVONIX V4.6 uses O2 optimization with ThinLTO and AutoFDO profile data. The build script validates the compiler toolchain, ThinLTO configuration, default governor settings, and the bundled AutoFDO profile before invoking Kleaf.

## Verification

Ensure a zero exit status from the Kleaf build and distribution phases. Validate the generated Image embedded configuration (`extract-ikconfig`), release string, and KMI compliance.

The boot-only AnyKernel3 package delivers the standalone kernel Image and does not overwrite vendor modules. In accordance with Android GKI standards, zRAM and ZSMALLOC remain modular (`CONFIG_ZRAM=m`, `CONFIG_ZSMALLOC=m`) to prevent symbol collisions with installed vendor modules.
