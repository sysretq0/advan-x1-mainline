#!/bin/bash
# Source from build scripts to put Google's pinned Clang first on PATH (x86_64 hosts).
# Google's prebuilts are linux-x86 binaries, so non-x86_64 hosts (e.g. aarch64)
# automatically default to GOOGLE_CLANG=0 (host LLVM).
#
# GOOGLE_CLANG=1  -> fetch/use AOSP clang (x86_64 host only; default on x86_64).
# GOOGLE_CLANG=0  -> use the host LLVM toolchain (default on non-x86_64).
# CLANG_VERSION   -> pinned AOSP clang revision (default r614150).
REPO_TOOLCHAIN_ENV="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_ARCH="$(uname -m)"

if [ -z "${GOOGLE_CLANG:-}" ]; then
    if [ "$HOST_ARCH" = "x86_64" ]; then
        GOOGLE_CLANG=1
    else
        GOOGLE_CLANG=0
    fi
fi
export GOOGLE_CLANG

if [ "$GOOGLE_CLANG" != "0" ]; then
    CLANG_VERSION=${CLANG_VERSION:-r614150}
    CLANG_DIR=$("$REPO_TOOLCHAIN_ENV/setup-google-clang.sh")
    export PATH="$CLANG_DIR/bin:$PATH"
    export CLANG_DIR CLANG_VERSION
    echo "toolchain: $CLANG_DIR/bin/clang ($(clang --version 2>/dev/null | head -n 1))"
else
    echo "toolchain: host clang [$HOST_ARCH] ($(clang --version 2>/dev/null | head -n 1))"
fi
