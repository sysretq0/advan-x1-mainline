#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build libmm-plugin-mtk-ccci.so against Debian ModemManager 1.24.2 headers
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MM_SRC="${MM_SRC:-/work/mainline/mm-build/modemmanager-1.24.2}"
SYSROOT="${SYSROOT:-/work/mainline/mm-build/sysroot}"
OUT_DIR="${OUT_DIR:-/work/mainline/mm-build/out}"

mkdir -p "$OUT_DIR/gen"

# Generate ABI-matched config.h for Debian modemmanager 1.24.2-2+b1
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

# Generate mm-daemon-enums-types.h if required by internal headers
glib-mkenums \
  --template "$MM_SRC/build-aux/mm-enums-template.h" \
  --fhead "#ifndef __MM_DAEMON_ENUMS_TYPES_H__\n#define __MM_DAEMON_ENUMS_TYPES_H__\n#include <glib-object.h>\nG_BEGIN_DECLS\n" \
  --ftail "G_END_DECLS\n#endif\n" \
  "$MM_SRC/src/mm-port.h" \
  "$MM_SRC/src/mm-port-serial-at.h" \
  "$MM_SRC/src/mm-base-bearer.h" \
  "$MM_SRC/src/mm-iface-modem-3gpp.h" \
  > "$OUT_DIR/gen/mm-daemon-enums-types.h" 2>/dev/null || true

CFLAGS=(
  -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC -shared
  -fvisibility=hidden
  -DMM_MODULE_NAME=\"mtk-ccci\"
  -I"$OUT_DIR/gen"
  -I"$SCRIPT_DIR"
  -I"$SYSROOT/usr/include/ModemManager"
  -I"$SYSROOT/usr/include/libmm-glib"
  -I"$SYSROOT/usr/include/libqmi-glib"
  -I"$SYSROOT/usr/include/libmbim-glib"
  -I"$SYSROOT/usr/include/libqrtr-glib"
  -I"$MM_SRC/src"
  -I"$MM_SRC/src/plugins"
  -I"$MM_SRC/src/kerneldevice"
  -I"$MM_SRC/libmm-glib"
  -I"$MM_SRC/libmm-glib/generated"
  -I"$MM_SRC"
  $(pkg-config --cflags glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0 gudev-1.0)
)

LDFLAGS=(
  -Wl,--as-needed
  -L"$SYSROOT/usr/lib/aarch64-linux-gnu"
  $(pkg-config --libs glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0)
  -lmm-glib
)

gcc "${CFLAGS[@]}" \
  "$SCRIPT_DIR/mm-plugin-mtk-ccci.c" \
  "$SCRIPT_DIR/mm-modem-helpers-mtk-ccci.c" \
  -o "$OUT_DIR/libmm-plugin-mtk-ccci.so" \
  "${LDFLAGS[@]}"

ls -lh "$OUT_DIR/libmm-plugin-mtk-ccci.so"
llvm-nm -D --defined-only "$OUT_DIR/libmm-plugin-mtk-ccci.so"
