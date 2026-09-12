#!/usr/bin/env bash
set -euo pipefail

COMMON_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(dirname "$COMMON_DIR")"

BAZEL="$WORKSPACE/tools/bazel"
CLANG23="$WORKSPACE/prebuilts/clang/host/linux-x86/clang-r614150"
PROFILE="$COMMON_DIR/android/gki/aarch64/afdo/kernel.afdo"

EXPECTED_AFDO_SHA256="b27ba364e664c5d83daae4cece90dffe95b6388bc2c214c2b2e1a5f35b0654b2"

DIST="${1:-$WORKSPACE/out/evonix}"

die() {
    echo
    echo "ERROR: $*" >&2
    exit 1
}

echo "============================================"
echo " EVONIX Kernel Build"
echo " Linux 6.6.142"
echo " Clang 23 + ThinLTO + AutoFDO"
echo "============================================"
echo

# This script must be run from the full Kleaf workspace copy:
#
#   evonix-workspace/
#     common/               <- EVONIX source
#     tools/bazel
#     prebuilts/...
#
[[ -x "$BAZEL" ]] ||
    die "Kleaf workspace not found. Run this from evonix-workspace/common."

[[ -x "$CLANG23/bin/clang" ]] ||
    die "clang-r614150 is missing: $CLANG23"

[[ -x "$CLANG23/bin/ld.lld" ]] ||
    die "LLD from clang-r614150 is missing."

echo "===== TOOLCHAIN ====="

CLANG_VERSION="$("$CLANG23/bin/clang" --version | head -n 1)"
LLD_VERSION="$("$CLANG23/bin/ld.lld" --version | head -n 1)"

echo "$CLANG_VERSION"
echo "$LLD_VERSION"

grep -q 'clang version 23\.0\.1' <<<"$CLANG_VERSION" ||
    die "Wrong Clang version. EVONIX requires Clang 23.0.1."

grep -q '16311247' <<<"$CLANG_VERSION" ||
    die "Wrong Android Clang build. EVONIX requires build 16311247 / r614150."

grep -q 'LLD 23\.0\.1' <<<"$LLD_VERSION" ||
    die "Wrong LLD version. EVONIX requires LLD 23.0.1."

echo
echo "===== KLEAF COMPATIBILITY ====="

grep -qx 'CLANG_VERSION=r510928' "$COMMON_DIR/build.config.constants" ||
    die "build.config.constants must stay r510928 for this Android15 Kleaf generation."

echo "Kleaf bootstrap metadata: r510928"
echo "EVONIX kernel compiler:    r614150"

echo
echo "===== THINLTO ====="

CONFIG="$COMMON_DIR/arch/arm64/configs/evonix.config"

grep -qx 'CONFIG_LTO=y' "$CONFIG" ||
    die "CONFIG_LTO=y missing."

grep -qx 'CONFIG_LTO_CLANG=y' "$CONFIG" ||
    die "CONFIG_LTO_CLANG=y missing."

grep -qx 'CONFIG_LTO_CLANG_THIN=y' "$CONFIG" ||
    die "CONFIG_LTO_CLANG_THIN=y missing."

if grep -qx 'CONFIG_LTO_NONE=y' "$CONFIG"; then
    die "CONFIG_LTO_NONE=y is enabled."
fi

echo "ThinLTO source configuration: OK"

echo
echo "===== AUTOFDO ====="

[[ -f "$PROFILE" ]] ||
    die "AutoFDO profile is missing."

AFDO_SHA256="$(sha256sum "$PROFILE" | awk '{print $1}')"

echo "Profile SHA256: $AFDO_SHA256"

[[ "$AFDO_SHA256" == "$EXPECTED_AFDO_SHA256" ]] ||
    die "AutoFDO profile hash does not match the known 6.6.142 profile."

awk '
    /"kernel_aarch64": \{/ { inside=1 }
    /"kernel_aarch64_16k": \{/ { inside=0 }
    inside { print }
' "$COMMON_DIR/BUILD.bazel" |
grep -q 'clang_autofdo_profile.*android/gki/aarch64/afdo/kernel.afdo' ||
    die "AutoFDO is not wired into the normal kernel_aarch64 target."

echo "Google 6.6.142 AutoFDO profile: OK"

echo
echo "===== BUILD ====="
echo "Output: $DIST"
echo

mkdir -p "$DIST"

cd "$WORKSPACE"

"$BAZEL" run \
    --config=fast \
    --lto=thin \
    --user_clang_toolchain="$CLANG23" \
    //common:kernel_aarch64_dist -- \
    --dist_dir="$DIST"

echo
echo "===== VERIFY OUTPUT ====="

[[ -f "$DIST/Image" ]] ||
    die "Build completed without an Image in the dist directory."

[[ -f "$DIST/vmlinux" ]] ||
    die "Build completed without vmlinux in the dist directory."

CFG="$(
    find "$WORKSPACE/out/bazel/output_user_root" \
        -path '*kernel_aarch64_config/out_dir/.config' \
        -type f \
        -printf '%T@ %p\n' 2>/dev/null |
    sort -nr |
    head -n 1 |
    cut -d' ' -f2-
)"

[[ -n "$CFG" && -f "$CFG" ]] ||
    die "Could not locate generated kernel .config."

echo "Generated config: $CFG"
echo

for option in \
    CONFIG_LTO=y \
    CONFIG_LTO_CLANG=y \
    CONFIG_LTO_CLANG_THIN=y \
    CONFIG_AUTOFDO_CLANG=y
do
    grep -qx "$option" "$CFG" ||
        die "Final kernel config is missing: $option"
done

if grep -qx 'CONFIG_LTO_NONE=y' "$CFG"; then
    die "Final kernel unexpectedly contains CONFIG_LTO_NONE=y."
fi

KERNEL_VERSION="$(strings "$DIST/Image" | grep -m1 'Linux version' || true)"

grep -q 'clang version 23\.0\.1' <<<"$KERNEL_VERSION" ||
    die "Built Image does not report Clang 23.0.1."

grep -q 'r614150' <<<"$KERNEL_VERSION" ||
    die "Built Image does not report r614150."

echo "Final config:"
grep -E \
'^(CONFIG_LTO|CONFIG_LTO_CLANG|CONFIG_LTO_CLANG_THIN|CONFIG_LTO_NONE|CONFIG_AUTOFDO_CLANG)=' \
"$CFG" || true

echo
echo "Kernel:"
echo "$KERNEL_VERSION"

echo
echo "Image SHA256:"
sha256sum "$DIST/Image"

echo
echo "============================================"
echo " EVONIX BUILD SUCCESS"
echo " Clang 23.0.1 r614150"
echo " ThinLTO"
echo " AutoFDO 6.6.142"
echo "============================================"
