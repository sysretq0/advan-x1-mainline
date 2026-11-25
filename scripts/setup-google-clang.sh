#!/bin/bash
# Fetch and cache Google's prebuilt Clang for kernel builds.
#
# Pinned revision mirrors AOSP android-mainline bazel/constants.scl:
#   CLANG_VERSION="r614150"
# Source of truth:
#   https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/bazel/constants.scl
#
# Toolchain directory shape (AOSP prebuilts/clang/host/linux-x86/clang-r*):
#   clang-r614150/bin/clang          compiler driver
#                  /ld.lld, /llvm-ar, /llvm-nm, /llvm-objcopy,
#                  /llvm-objdump, /llvm-readelf, /llvm-strip
#                  /clang++ , /clang-cl, ...
#               lib/, lib64/         shared LLVM libraries
#               lib/clang/<ver>/     builtin headers + runtime (resource dir)
# Prepending <toolchain>/bin to PATH is all a kernel make needs with LLVM=1;
# no --gcc-toolchain is required when CROSS_COMPILE=aarch64-linux-gnu- is set.
#
# Prints the absolute toolchain directory on stdout; progress goes to stderr.
set -euo pipefail
HOST_ARCH="$(uname -m)"
if [ "$HOST_ARCH" != "x86_64" ]; then
    echo "FATAL: AOSP prebuilt clang is linux-x86 only (host is $HOST_ARCH). Set GOOGLE_CLANG=0." >&2
    exit 1
fi
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CLANG_VERSION=${CLANG_VERSION:-r614150}
AOSP_BRANCH=${AOSP_BRANCH:-main-kernel}
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-$REPO/../toolchain}
DEST="$TOOLCHAIN_ROOT/clang-$CLANG_VERSION"

if [ -x "$DEST/bin/clang" ] && [ -x "$DEST/bin/ld.lld" ]; then
    echo "$DEST"
    exit 0
fi

URL="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/$AOSP_BRANCH/clang-$CLANG_VERSION.tar.gz"
echo "== fetching Google clang-$CLANG_VERSION ($AOSP_BRANCH) ==" >&2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL --connect-timeout 20 --max-time 900 --retry 3 "$URL" -o "$TMP/clang.tar.gz"
mkdir -p "$TMP/unpack"
tar -xzf "$TMP/clang.tar.gz" -C "$TMP/unpack"
if [ -d "$TMP/unpack/clang-$CLANG_VERSION" ]; then
    SRC="$TMP/unpack/clang-$CLANG_VERSION"
else
    SRC="$TMP/unpack"
fi
[ -x "$SRC/bin/clang" ] || { echo "FATAL: no bin/clang in $URL" >&2; exit 1; }
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
mv "$SRC" "$DEST"
echo "== installed $DEST ==" >&2
"$DEST/bin/clang" --version 2>&1 | head -n 2 >&2
echo "$DEST"
