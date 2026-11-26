#!/usr/bin/env bash
# Build all Advan X1 MT6789 userspace tools for arm64 Debian sid/forky using
# Google's pinned AOSP Clang + lld against a Debian sid arm64 sysroot:
#   - ccci_fsd + ccci-fsd-launch.sh + ccci-fsd.service
#   - ccci_rpcd
#   - libmm-plugin-mtk-ccci.so + udev rules + ModemManager systemd drop-in
#   - mtk-wwan-connect + mtk-wwan-connect.service
#   - ALSA UCM2 profiles (mt6789-mt6366)
#
# Output in $OUT_DIR:
#   advan-x1-userspace-tools-arm64.tar.xz
#   install-userspace-tools.sh
#   sha256sums.txt
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CLANG_DIR="${CLANG_DIR:?CLANG_DIR must point to full Google Clang toolchain}"
SYSROOT="${SYSROOT:?SYSROOT must point to Debian sid arm64 sysroot}"
WORK_DIR="${WORK_DIR:-$REPO/build-userspace-tools}"
STAGE_DIR="${STAGE_DIR:-$WORK_DIR/stage}"
OUT_DIR="${OUT_DIR:-$WORK_DIR/out}"

[ -x "$CLANG_DIR/bin/clang" ] || { echo "FATAL: missing $CLANG_DIR/bin/clang" >&2; exit 1; }
[ -x "$CLANG_DIR/bin/ld.lld" ] || { echo "FATAL: missing $CLANG_DIR/bin/ld.lld" >&2; exit 1; }
[ -d "$SYSROOT/usr/lib/aarch64-linux-gnu" ] || { echo "FATAL: invalid arm64 sysroot $SYSROOT" >&2; exit 1; }

GCC_DIR="$(find "$SYSROOT/usr/lib/gcc/aarch64-linux-gnu" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
[ -n "$GCC_DIR" ] || { echo "FATAL: no aarch64 gcc runtime dir in $SYSROOT" >&2; exit 1; }
QEMU_AARCH64="$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)"
[ -n "$QEMU_AARCH64" ] || { echo "FATAL: neither qemu-aarch64-static nor qemu-aarch64 found" >&2; exit 1; }

rm -rf "$STAGE_DIR" "$OUT_DIR"
mkdir -p "$WORK_DIR" "$STAGE_DIR" "$OUT_DIR" \
    "$STAGE_DIR/usr/local/sbin" \
    "$STAGE_DIR/etc/systemd/system" \
    "$STAGE_DIR/etc/systemd/system/ModemManager.service.d" \
    "$STAGE_DIR/etc/udev/rules.d" \
    "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366" \
    "$STAGE_DIR/usr/share/alsa/ucm2/conf.d/mt6789-mt6366"

CLANG_AARCH64=(
    "$CLANG_DIR/bin/clang"
    --target=aarch64-linux-gnu
    --sysroot="$SYSROOT"
    --gcc-install-dir="$GCC_DIR"
    -fuse-ld=lld
    -Wno-unused-command-line-argument
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu"
    -L"$GCC_DIR"
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu"
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu"
)

echo "== 1. Building ccci_fsd (Google Clang arm64) =="
"${CLANG_AARCH64[@]}" -O2 -Wall -Wextra -Werror -std=c11 \
    "$REPO/tools/ccci_fsd/ccci_fsd.c" \
    -o "$STAGE_DIR/usr/local/sbin/ccci_fsd"
"$CLANG_DIR/bin/llvm-strip" --strip-all "$STAGE_DIR/usr/local/sbin/ccci_fsd"
install -m 0755 "$REPO/tools/ccci_fsd/ccci-fsd-launch.sh" "$STAGE_DIR/usr/local/sbin/ccci-fsd-launch.sh"
install -m 0644 "$REPO/tools/ccci_fsd/ccci-fsd.service" "$STAGE_DIR/etc/systemd/system/ccci-fsd.service"

echo "== 2. Building ccci_rpcd (Google Clang arm64) =="
"${CLANG_AARCH64[@]}" -O2 -Wall -Wextra -Werror -std=c11 \
    "$REPO/tools/ccci_rpcd/ccci_rpcd.c" \
    -o "$STAGE_DIR/usr/local/sbin/ccci_rpcd"
"$CLANG_DIR/bin/llvm-strip" --strip-all "$STAGE_DIR/usr/local/sbin/ccci_rpcd"

echo "== 3. Building ModemManager mtk-ccci plugin (Google Clang arm64) =="
MM_WORK="$WORK_DIR/mm-src"
rm -rf "$MM_WORK"
mkdir -p "$MM_WORK/gen"

# Extract Debian sid modemmanager source headers via apt-get source inside sysroot
MM_SRC_HOST="$SYSROOT/tmp/mm-src-extract"
sudo rm -rf "$MM_SRC_HOST"
sudo mkdir -p "$MM_SRC_HOST"
sudo chroot "$SYSROOT" /bin/bash -c '
    set -e
    cd /tmp/mm-src-extract
    apt-get -o Acquire::Retries=3 update -qq
    apt-get -o Acquire::Retries=3 source -q modemmanager
'
MM_SRC="$(find "$MM_SRC_HOST" -mindepth 1 -maxdepth 1 -type d -name 'modemmanager-*' | head -n 1)"
[ -n "$MM_SRC" ] || { echo "FATAL: could not locate extracted modemmanager source" >&2; exit 1; }
echo "  ModemManager source: $MM_SRC"

cat > "$MM_WORK/gen/config.h" << 'EOF'
#pragma once
#define WITH_UDEV 1
#define WITH_POLKIT 1
#define WITH_SUSPEND_RESUME 1
#define WITH_SYSTEMD_JOURNAL 1
#define WITH_QMI 1
#define WITH_MBIM 1
#define WITH_QRTR 1
EOF

# Generate mm-daemon-enums-types.h using sysroot glib-mkenums
QEMU_LD_PREFIX="$SYSROOT" "$QEMU_AARCH64" -L "$SYSROOT" \
    "$SYSROOT/usr/bin/glib-mkenums" \
    --template "$MM_SRC/build-aux/mm-enums-template.h" \
    --fhead "#ifndef __MM_DAEMON_ENUMS_TYPES_H__\n#define __MM_DAEMON_ENUMS_TYPES_H__\n#include <glib-object.h>\nG_BEGIN_DECLS\n" \
    --ftail "G_END_DECLS\n#endif\n" \
    "$MM_SRC/src/mm-port.h" \
    "$MM_SRC/src/mm-port-serial-at.h" \
    "$MM_SRC/src/mm-base-bearer.h" \
    "$MM_SRC/src/mm-iface-modem-3gpp.h" \
    > "$MM_WORK/gen/mm-daemon-enums-types.h" 2>/dev/null || true

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
GLIB_CFLAGS="$(pkg-config --cflags glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0 gudev-1.0)"
GLIB_LIBS="$(pkg-config --libs glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0)"

# shellcheck disable=SC2086
"${CLANG_AARCH64[@]}" \
    -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -fPIC -shared -fvisibility=hidden \
    -DMM_MODULE_NAME=\"mtk-ccci\" \
    -I"$MM_WORK/gen" \
    -I"$REPO/tools/modemmanager-mtk-ccci" \
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
    "$REPO/tools/modemmanager-mtk-ccci/mm-plugin-mtk-ccci.c" \
    "$REPO/tools/modemmanager-mtk-ccci/mm-modem-helpers-mtk-ccci.c" \
    -o "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so" \
    -Wl,--as-needed $GLIB_LIBS -lmm-glib

"$CLANG_DIR/bin/llvm-strip" --strip-unneeded \
    "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so"
"$CLANG_DIR/bin/llvm-nm" -D --defined-only \
    "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so"

install -m 0644 "$REPO/tools/modemmanager-mtk-ccci/77-mm-mtk-ccci.rules" \
    "$STAGE_DIR/etc/udev/rules.d/77-mm-mtk-ccci.rules"
install -m 0644 "$REPO/tools/modemmanager-mtk-ccci/ModemManager-mtk-ccci.conf" \
    "$STAGE_DIR/etc/systemd/system/ModemManager.service.d/10-mtk-ccci.conf"
sudo rm -rf "$MM_SRC_HOST"

echo "== 4. Staging mtk-wwan-connect & ALSA UCM2 profiles =="
install -m 0755 "$REPO/tools/mtk-wwan-connect/mtk-wwan-connect.py" \
    "$STAGE_DIR/usr/local/sbin/mtk-wwan-connect"
install -m 0644 "$REPO/tools/mtk-wwan-connect/mtk-wwan-connect.service" \
    "$STAGE_DIR/etc/systemd/system/mtk-wwan-connect.service"

install -m 0644 "$REPO/tools/ucm2/MediaTek/mt6789-mt6366/mt6789-mt6366.conf" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/mt6789-mt6366.conf"
install -m 0644 "$REPO/tools/ucm2/MediaTek/mt6789-mt6366/HiFi.conf" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/HiFi.conf"
ln -sf ../../MediaTek/mt6789-mt6366/mt6789-mt6366.conf \
    "$STAGE_DIR/usr/share/alsa/ucm2/conf.d/mt6789-mt6366/mt6789-mt6366.conf"

echo "== 5. Generating host-direct installer script =="
cat > "$OUT_DIR/install-userspace-tools.sh" <<'EOF'
#!/usr/bin/env bash
# Install CI-built Advan X1 CCCI, ModemManager plugin, WWAN helper, and ALSA UCM2
# profiles directly onto the live Debian arm64 rootfs (host /) with sudo.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

TARBALL="${1:-$(dirname "$0")/advan-x1-userspace-tools-arm64.tar.xz}"
[ -f "$TARBALL" ] || { echo "FATAL: tarball not found: $TARBALL" >&2; exit 1; }

echo "=== 1. Extract Advan X1 userspace tools onto / ==="
tar -xJf "$TARBALL" -C /

echo "=== 2. Verify binaries & shared library linkage ==="
file /usr/local/sbin/ccci_fsd /usr/local/sbin/ccci_rpcd \
     /usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so || true
MISSING=$(ldd /usr/local/sbin/ccci_fsd /usr/local/sbin/ccci_rpcd 2>/dev/null | grep "not found" || true)
if [ -n "$MISSING" ]; then
    echo "ERROR: Missing shared libs:" >&2
    echo "$MISSING" >&2
    exit 1
fi

echo "=== 3. Enable ccci-fsd.service & reload udev/systemd if running ==="
mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/ccci-fsd.service \
       /etc/systemd/system/multi-user.target.wants/ccci-fsd.service
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    udevadm control --reload-rules || true
fi
echo "USERSPACE_TOOLS_INSTALL_OK"
EOF
chmod +x "$OUT_DIR/install-userspace-tools.sh"

TARBALL="advan-x1-userspace-tools-arm64.tar.xz"
echo "== 6. Packing $OUT_DIR/$TARBALL =="
XZ_OPT="-9e -T0" tar -C "$STAGE_DIR" -cJf "$OUT_DIR/$TARBALL" etc usr
(
    cd "$OUT_DIR"
    sha256sum "$TARBALL" install-userspace-tools.sh > sha256sums.txt
    ls -lh "$TARBALL" install-userspace-tools.sh
    cat sha256sums.txt
)
echo "== Advan X1 userspace tools build complete =="
