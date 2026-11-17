#!/bin/bash
# Fetch latest LNXB bundle artifact from GitHub Actions CI
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-$REPO/build}
mkdir -p "$OUT"

echo "== Fetching latest bundle from GitHub Actions (sysretq0/advan-x1-mainline) =="
gh run download --repo sysretq0/advan-x1-mainline --name advan-x1-mainline-bundle --dir "$OUT"

if [ -f "$OUT/bundle.tar.xz" ]; then
    echo "== Unpacking bundle.tar.xz =="
    tar -xJf "$OUT/bundle.tar.xz" -C "$OUT"
    echo "== Unpacked artifacts in $OUT: =="
    ls -lh "$OUT/bundle.img" "$OUT/Image" "$OUT/mt6789-advan-x1.dtb"
    cd "$OUT" && sha256sum -c sha256sums.txt
fi
