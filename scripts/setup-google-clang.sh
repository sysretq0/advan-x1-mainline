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
PRUNE_FOR_KERNEL=${PRUNE_FOR_KERNEL:-0}
DEST="$TOOLCHAIN_ROOT/clang-$CLANG_VERSION"

prune_for_kernel() {
    local d="$1"
    echo "== pruning non-kernel AOSP clang assets in $d ==" >&2
    # Remove large non-kernel directories (NDK/sanitizer runtimes, LLDB python,
    # C++ headers, docs, cross-musl targets). Keep lib/clang/*/include!
    rm -rf \
        "$d/python3" \
        "$d/runtimes_ndk_cxx" \
        "$d/android_libc++" \
        "$d/share" \
        "$d/include" \
        "$d"/lib/clang/*/lib \
        "$d"/lib/cmake \
        "$d"/lib/python* \
        "$d"/lib/*-musl* \
        "$d"/lib/arm-* \
        "$d"/lib/riscv* \
        "$d"/lib/i386-* \
        "$d"/lib/aarch64-linux-android* \
        "$d"/lib/x86_64-linux-android* \
        "$d"/lib/*.a \
        "$d"/lib/liblldb*.so* \
        "$d"/lib/libclang.so* \
        "$d"/lib64/*.a \
        "$d"/lib64/liblldb*.so* \
        "$d"/lib64/libclang.so* 2>/dev/null || true

    # Remove standalone host tools unused by Linux Kbuild LLVM=1.
    rm -f \
        "$d"/bin/lldb* \
        "$d"/bin/clangd \
        "$d"/bin/clang-tidy \
        "$d"/bin/clang-format \
        "$d"/bin/clang-check \
        "$d"/bin/clang-query \
        "$d"/bin/clang-doc \
        "$d"/bin/clang-rename \
        "$d"/bin/clang-reorder-fields \
        "$d"/bin/clang-include-fixer \
        "$d"/bin/clang-move \
        "$d"/bin/clang-change-namespace \
        "$d"/bin/clang-pseudo \
        "$d"/bin/clang-repl \
        "$d"/bin/clang-scan-deps \
        "$d"/bin/clang-extdef-mapping \
        "$d"/bin/clang-offload-* \
        "$d"/bin/llvm-bolt* \
        "$d"/bin/perf2bolt \
        "$d"/bin/merge-fdata \
        "$d"/bin/dsymutil \
        "$d"/bin/llc \
        "$d"/bin/opt \
        "$d"/bin/lli \
        "$d"/bin/bugpoint \
        "$d"/bin/sancov \
        "$d"/bin/sanstats \
        "$d"/bin/scan-build* \
        "$d"/bin/scan-view \
        "$d"/bin/git-clang-format \
        "$d"/bin/c-index-test \
        "$d"/bin/diagtool \
        "$d"/bin/pp-trace \
        "$d"/bin/modularize \
        "$d"/bin/find-all-symbols \
        "$d"/bin/hmaptool \
        "$d"/bin/llvm-cov \
        "$d"/bin/llvm-profdata \
        "$d"/bin/llvm-profgen \
        "$d"/bin/llvm-xray \
        "$d"/bin/llvm-lto* \
        "$d"/bin/llvm-mca \
        "$d"/bin/llvm-exegesis \
        "$d"/bin/llvm-jitlink \
        "$d"/bin/llvm-rtdyld \
        "$d"/bin/llvm-dwarfdump \
        "$d"/bin/llvm-gsymutil \
        "$d"/bin/llvm-debuginfod* \
        "$d"/bin/llvm-ifs \
        "$d"/bin/llvm-tapi-diff \
        "$d"/bin/llvm-ml \
        "$d"/bin/llvm-rc \
        "$d"/bin/llvm-mt \
        "$d"/bin/llvm-cvtres \
        "$d"/bin/llvm-windres \
        "$d"/bin/llvm-dlltool \
        "$d"/bin/llvm-lib \
        "$d"/bin/llvm-pdbutil \
        "$d"/bin/lld-link \
        "$d"/bin/wasm-ld 2>/dev/null || true

    # Smoke-test every tool required by Linux ARCH=arm64 LLVM=1.
    "$d/bin/clang" --version >/dev/null
    "$d/bin/ld.lld" --version >/dev/null
    "$d/bin/llvm-ar" --version >/dev/null
    "$d/bin/llvm-objcopy" --version >/dev/null
    "$d/bin/llvm-strip" --version >/dev/null
    printf '#include <stdarg.h>\nint f(int a, ...) { va_list ap; va_start(ap, a); va_end(ap); return a; }\n' \
        | "$d/bin/clang" --target=aarch64-linux-gnu -c -x c - -o /dev/null
    touch "$d/.kernel-slim"
    echo "== pruned size: $(du -sh "$d" | cut -f1) ==" >&2
}

if [ -x "$DEST/bin/clang" ] && [ -x "$DEST/bin/ld.lld" ]; then
    if [ "$PRUNE_FOR_KERNEL" = "1" ] && [ ! -f "$DEST/.kernel-slim" ]; then
        prune_for_kernel "$DEST"
    fi
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
if [ "$PRUNE_FOR_KERNEL" = "1" ]; then
    prune_for_kernel "$DEST"
fi
echo "== installed $DEST ==" >&2
"$DEST/bin/clang" --version 2>&1 | head -n 2 >&2
echo "$DEST"
