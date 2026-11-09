#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${OUT_DIR:?Set OUT_DIR to an empty build directory}"
llvm=${LLVM_PREFIX:-1}
jobs=${JOBS:-4}
mkdir -p "$OUT_DIR"
out=$(cd "$OUT_DIR" && pwd)
[ -z "$(ls -A "$out")" ] || { echo 'The output directory must be empty.' >&2; exit 1; }
export KBUILD_BUILD_USER=${KBUILD_BUILD_USER:-Neeschal}
export KBUILD_BUILD_HOST=${KBUILD_BUILD_HOST:-code}
export KBUILD_BUILD_VERSION=${KBUILD_BUILD_VERSION:-1}
python3 - "$source_root" "$out" <<'PY'
from pathlib import Path
import re, sys
root, out = map(Path, sys.argv[1:])
symbols=set()
for p in (root/'android').glob('abi_gki_aarch64*'):
    if not p.is_file() or p.suffix not in ('', '.hybridswap') or not p.read_bytes().lstrip().startswith(b'[abi_symbol_list]'):
        continue
    for line in p.read_text().splitlines():
        line=line.split('#',1)[0].strip()
        if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',line):
            symbols.add(line)
assert symbols, 'No GKI export whitelist found'
(out/'abi_symbollist.raw').write_text('\n'.join(sorted(symbols))+'\n')

PY
make -C "$source_root" O="$out" ARCH=arm64 LLVM="$llvm" LLVM_IAS=1 evonix_rodin_hybridswap_defconfig
# Build the native storage backend and display notifier into the boot kernel.
make -C "$source_root" O="$out" ARCH=arm64 LLVM="$llvm" LLVM_IAS=1 -j"$jobs" Image
sha256sum "$out/arch/arm64/boot/Image"
