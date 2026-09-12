#!/usr/bin/env bash
set -euo pipefail

COMMON_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Resolve the full Kleaf workspace.
#
# Supported layouts:
#
#   ~/evonix-workspace/common/setup-clang23.sh
#   ~/EVONIX-Kernel/setup-clang23.sh + ~/evonix-workspace
#
# EVONIX_WORKSPACE may also explicitly override the location.
if [[ -n "${EVONIX_WORKSPACE:-}" ]]; then
    WORKSPACE="$(cd -- "$EVONIX_WORKSPACE" && pwd)"
elif [[ -x "$(dirname "$COMMON_DIR")/tools/bazel" ]]; then
    WORKSPACE="$(dirname "$COMMON_DIR")"
elif [[ -x "$HOME/evonix-workspace/tools/bazel" ]]; then
    WORKSPACE="$HOME/evonix-workspace"
else
    echo "ERROR: Could not locate EVONIX Kleaf workspace." >&2
    echo "Set EVONIX_WORKSPACE=/path/to/evonix-workspace and retry." >&2
    exit 1
fi

TOOLCHAIN_COMMIT="28337064939c0b4bb5804cddf35fb03d860edcf6"
TOOLCHAIN_DIR="clang-r614150"
TOOLCHAIN_URL="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86"

CACHE="$WORKSPACE/.evonix-toolchains/android-clang"
TARGET="$WORKSPACE/prebuilts/clang/host/linux-x86/$TOOLCHAIN_DIR"

die() {
    echo
    echo "ERROR: $*" >&2
    exit 1
}

echo "============================================"
echo " EVONIX Clang 23 Setup"
echo " r614150 / Clang 23.0.1"
echo " Android build 16311247"
echo "============================================"
echo

[[ -d "$WORKSPACE/prebuilts/clang/host/linux-x86" ]] ||
    die "Clang prebuilts directory missing from workspace: $WORKSPACE"

echo "Workspace: $WORKSPACE"

verify_toolchain() {
    local clang="$TARGET/bin/clang"
    local lld="$TARGET/bin/ld.lld"
    local cv lv

    [[ -x "$clang" ]] || return 1
    [[ -x "$lld" ]] || return 1

    cv="$("$clang" --version | head -n1)"
    lv="$("$lld" --version | head -n1)"

    grep -q 'clang version 23\.0\.1' <<<"$cv" || return 1
    grep -q '16311247' <<<"$cv" || return 1
    grep -q 'r614150' <<<"$cv" || return 1
    grep -q 'LLD 23\.0\.1' <<<"$lv" || return 1

    echo "$cv"
    echo "$lv"
}

if verify_toolchain >/dev/null 2>&1; then
    echo "Exact EVONIX Clang 23 toolchain is already installed:"
    verify_toolchain
    exit 0
fi

echo "===== PREPARE CACHE ====="

rm -rf "$CACHE"
mkdir -p "$(dirname "$CACHE")"

git init "$CACHE"
git -C "$CACHE" remote add origin "$TOOLCHAIN_URL"

echo
echo "===== FETCH PINNED GOOGLE PREBUILT ====="
echo "Commit: $TOOLCHAIN_COMMIT"

git -C "$CACHE" fetch \
    --depth=1 \
    --filter=blob:none \
    origin "$TOOLCHAIN_COMMIT"

git -C "$CACHE" sparse-checkout init --cone
git -C "$CACHE" sparse-checkout set "$TOOLCHAIN_DIR"
git -C "$CACHE" checkout --detach FETCH_HEAD

ACTUAL_COMMIT="$(git -C "$CACHE" rev-parse HEAD)"

[[ "$ACTUAL_COMMIT" == "$TOOLCHAIN_COMMIT" ]] ||
    die "Fetched unexpected toolchain commit: $ACTUAL_COMMIT"

[[ -x "$CACHE/$TOOLCHAIN_DIR/bin/clang" ]] ||
    die "$TOOLCHAIN_DIR was not downloaded correctly."

echo
echo "===== INSTALL ====="

rm -rf "$TARGET"

# Hardlink when cache and workspace are on the same filesystem.
# Fall back to a normal copy otherwise.
if cp -al "$CACHE/$TOOLCHAIN_DIR" "$TARGET" 2>/dev/null; then
    echo "Installed using hardlinks."
else
    echo "Hardlink unavailable; copying toolchain."
    cp -a "$CACHE/$TOOLCHAIN_DIR" "$TARGET"
fi

echo
echo "===== VERIFY ====="

verify_toolchain ||
    die "Installed toolchain failed EVONIX version verification."

echo
echo "Pinned Google prebuilt commit:"
echo "$ACTUAL_COMMIT"

echo
echo "Toolchain path:"
echo "$TARGET"

echo
echo "============================================"
echo " EVONIX CLANG 23 READY"
echo "============================================"
