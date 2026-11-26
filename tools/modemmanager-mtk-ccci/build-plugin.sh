#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build libmm-plugin-mtk-ccci.so against the target Debian arm64 ModemManager
# internal headers.
#
# Note: CI packages this plugin inside advan-x1-support.deb via
# scripts/build-userspace-tools.sh (Google Clang + Debian sid arm64 sysroot).
# This helper supports local native arm64 builds (default on-device) or
# cross-compilation when CC and SYSROOT are provided.
#
# Environment overrides:
#   MM_SRC   - path to extracted Debian modemmanager source tree (e.g. from `apt-get source modemmanager`)
#   SYSROOT  - target arm64 sysroot (default: / on aarch64 hosts; required for cross-builds)
#   OUT_DIR  - build output directory (default: <script_dir>/build)
#   CC       - compiler (default: clang or gcc on aarch64; aarch64-linux-gnu-gcc when cross-compiling)
#   NM       - symbol lister (default: llvm-nm or nm)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOST_ARCH="$(uname -m)"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/build}"
MM_SRC="${MM_SRC:-}"

if [ -z "$MM_SRC" ]; then
    MM_SRC="$(find "$OUT_DIR" "$SCRIPT_DIR" -maxdepth 2 -mindepth 1 -type d -name 'modemmanager-*' 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [ -z "$MM_SRC" ] || [ ! -d "$MM_SRC/src" ]; then
    echo "FATAL: MM_SRC must point to an extracted Debian modemmanager source tree" >&2
    echo "  (e.g. mkdir -p '$OUT_DIR' && (cd '$OUT_DIR' && apt-get source modemmanager))" >&2
    exit 1
fi

if [ "$HOST_ARCH" = "aarch64" ]; then
    SYSROOT="${SYSROOT:-/}"
    CC="${CC:-$(command -v clang || command -v gcc)}"
else
    SYSROOT="${SYSROOT:?SYSROOT must point to a Debian arm64 sysroot when cross-compiling from $HOST_ARCH}"
    CC="${CC:-aarch64-linux-gnu-gcc}"
fi
NM="${NM:-$(command -v llvm-nm || command -v nm)}"

[ -d "$SYSROOT/usr/include/ModemManager" ] || {
    echo "FATAL: missing $SYSROOT/usr/include/ModemManager (install modemmanager-dev in sysroot)" >&2
    exit 1
}

mkdir -p "$OUT_DIR/gen"

# Generate ABI-matched config.h for Debian modemmanager packages
cat > "$OUT_DIR/gen/config.h" << 'EOF'
#pragma once
#define WITH_UDEV 1
#define WITH_POLKIT 1
#define WITH_SUSPEND_RESUME 1
#define WITH_SYSTEMD_JOURNAL 1
#define WITH_QMI 1
#define WITH_MBIM 1
#define WITH_QRTR 1
EOF

# Generate mm-daemon-enums-types.h required by internal MM headers
MKENUMS="$(command -v glib-mkenums || echo "$SYSROOT/usr/bin/glib-mkenums")"
python3 "$MKENUMS" \
    --template "$MM_SRC/build-aux/mm-enums-template.h" \
    --fhead "#ifndef __MM_DAEMON_ENUMS_TYPES_H__\n#define __MM_DAEMON_ENUMS_TYPES_H__\n#include <glib-object.h>\nG_BEGIN_DECLS\n" \
    --ftail "G_END_DECLS\n#endif\n" \
    "$MM_SRC/src/mm-port.h" \
    "$MM_SRC/src/mm-port-serial-at.h" \
    "$MM_SRC/src/mm-base-bearer.h" \
    "$MM_SRC/src/mm-iface-modem-3gpp.h" \
    > "$OUT_DIR/gen/mm-daemon-enums-types.h" 2>/dev/null || true

if [ "$SYSROOT" != "/" ]; then
    export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
    export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
fi
GLIB_CFLAGS="$(pkg-config --cflags glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0 gudev-1.0)"
GLIB_LIBS="$(pkg-config --libs glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0)"

SYSROOT_FLAGS=()
if [ "$SYSROOT" != "/" ]; then
    SYSROOT_FLAGS+=(--sysroot="$SYSROOT" -L"$SYSROOT/usr/lib/aarch64-linux-gnu")
fi

# shellcheck disable=SC2086
"$CC" "${SYSROOT_FLAGS[@]}" \
    -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -fPIC -shared -fvisibility=hidden \
    -DMM_MODULE_NAME=\"mtk-ccci\" \
    -I"$OUT_DIR/gen" \
    -I"$SCRIPT_DIR" \
    -I"$SYSROOT/usr/include/ModemManager" \
    -I"$SYSROOT/usr/include/libmm-glib" \
    -I"$SYSROOT/usr/include/libqmi-glib" \
    -I"$SYSROOT/usr/include/libmbim-glib" \
    -I"$SYSROOT/usr/include/libqrtr-glib" \
    -I"$MM_SRC/src" \
    -I"$MM_SRC/src/plugins" \
    -I"$MM_SRC/src/kerneldevice" \
    -I"$MM_SRC/libmm-glib" \
    -I"$MM_SRC/libmm-glib/generated" \
    -I"$MM_SRC" \
    $GLIB_CFLAGS \
    "$SCRIPT_DIR/mm-plugin-mtk-ccci.c" \
    "$SCRIPT_DIR/mm-modem-helpers-mtk-ccci.c" \
    -o "$OUT_DIR/libmm-plugin-mtk-ccci.so" \
    -Wl,--as-needed $GLIB_LIBS -lmm-glib

ls -lh "$OUT_DIR/libmm-plugin-mtk-ccci.so"
"$NM" -D --defined-only "$OUT_DIR/libmm-plugin-mtk-ccci.so"
