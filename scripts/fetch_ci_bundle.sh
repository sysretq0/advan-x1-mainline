#!/bin/bash
# Fetch latest U-Boot FIT + boot.scr artifact bundle from GitHub Actions CI
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-$REPO/build}
mkdir -p "$OUT"

echo "== Fetching latest U-Boot boot bundle from GitHub Actions (sysretq0/advan-x1-mainline) =="
rm -f "$OUT/uboot-bundle.tar.xz" "$OUT/bundle.tar.xz"
if gh run download --repo sysretq0/advan-x1-mainline --name advan-x1-mainline-uboot --dir "$OUT" 2>/dev/null; then
    ARCHIVE="$OUT/uboot-bundle.tar.xz"
else
    gh run download --repo sysretq0/advan-x1-mainline --name advan-x1-mainline-bundle --dir "$OUT"
    ARCHIVE="$OUT/bundle.tar.xz"
fi

if [ -f "$ARCHIVE" ]; then
    echo "== Unpacking $(basename "$ARCHIVE") =="
    tar -xJf "$ARCHIVE" -C "$OUT"
    echo "== Unpacked artifacts in $OUT: =="
    ls -lh "$OUT"/advan-x1.itb "$OUT"/boot.scr "$OUT"/uEnv.txt "$OUT"/mt6789-advan-x1.dtb 2>/dev/null || ls -lh "$OUT"
    if [ -f "$OUT/sha256sums.txt" ]; then
        (cd "$OUT" && sha256sum -c sha256sums.txt)
    fi
fi
