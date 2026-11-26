#!/usr/bin/env bash
# Build all Advan X1 MT6789 userspace tools for arm64 Debian sid/forky using
# Google's pinned AOSP Clang + lld against a Debian sid arm64 sysroot, and
# package them with all device-specific systemd/udev/UCM2/network/GNOME
# defaults into a real Debian package: advan-x1-support_<ver>_arm64.deb.
#
# Compiled with Google Clang:
#   - /usr/local/sbin/ccci_fsd
#   - /usr/local/sbin/ccci_rpcd
#   - /usr/local/sbin/ccci_audd
#   - /usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so
#
# Output in $OUT_DIR:
#   advan-x1-support_<ver>_arm64.deb
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
PKG_VER="${PKG_VER:-1.0.$(git -C "$REPO" rev-parse --short=7 HEAD 2>/dev/null || echo dev)}"

[ -x "$CLANG_DIR/bin/clang" ] || { echo "FATAL: missing $CLANG_DIR/bin/clang" >&2; exit 1; }
[ -x "$CLANG_DIR/bin/ld.lld" ] || { echo "FATAL: missing $CLANG_DIR/bin/ld.lld" >&2; exit 1; }
[ -d "$SYSROOT/usr/lib/aarch64-linux-gnu" ] || { echo "FATAL: invalid arm64 sysroot $SYSROOT" >&2; exit 1; }

GCC_DIR="$(find "$SYSROOT/usr/lib/gcc/aarch64-linux-gnu" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
[ -n "$GCC_DIR" ] || { echo "FATAL: no aarch64 gcc runtime dir in $SYSROOT" >&2; exit 1; }
QEMU_AARCH64="$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)"
[ -n "$QEMU_AARCH64" ] || { echo "FATAL: neither qemu-aarch64-static nor qemu-aarch64 found" >&2; exit 1; }

rm -rf "$STAGE_DIR" "$OUT_DIR"
mkdir -p "$WORK_DIR" "$STAGE_DIR" "$OUT_DIR" \
    "$STAGE_DIR/DEBIAN" \
    "$STAGE_DIR/usr/local/sbin" \
    "$STAGE_DIR/usr/local/bin" \
    "$STAGE_DIR/etc/systemd/system" \
    "$STAGE_DIR/etc/systemd/system/ModemManager.service.d" \
    "$STAGE_DIR/etc/udev/rules.d" \
    "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366" \
    "$STAGE_DIR/usr/share/alsa/ucm2/conf.d/mt6789-mt6366" \
    "$STAGE_DIR/etc/pipewire/pipewire-pulse.conf.d" \
    "$STAGE_DIR/etc/wireplumber/wireplumber.conf.d"

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

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
GLIB_CFLAGS="$(pkg-config --cflags glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0 gmodule-2.0 gudev-1.0)"
GLIB_LIBS="$(pkg-config --libs glib-2.0 gio-2.0 gobject-2.0 gmodule-2.0)"

echo "== 2b. Building ccci_audd (Google Clang arm64) =="
# shellcheck disable=SC2086
"${CLANG_AARCH64[@]}" -O2 -Wall -Wextra -Werror -std=c11 \
    $GLIB_CFLAGS \
    "$REPO/tools/ccci_audd/ccci_audd.c" \
    -o "$STAGE_DIR/usr/local/sbin/ccci_audd" \
    -Wl,--as-needed $GLIB_LIBS
"$CLANG_DIR/bin/llvm-strip" --strip-all "$STAGE_DIR/usr/local/sbin/ccci_audd"
install -m 0644 "$REPO/tools/ccci_audd/ccci-audd.service" \
    "$STAGE_DIR/etc/systemd/system/ccci-audd.service"

echo "== 3. Building ModemManager mtk-ccci plugin (Google Clang arm64) =="
MM_WORK="$WORK_DIR/mm-src"
rm -rf "$MM_WORK"
mkdir -p "$MM_WORK/gen"

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

python3 "$SYSROOT/usr/bin/glib-mkenums" \
    --template "$MM_SRC/build-aux/mm-enums-template.h" \
    --fhead "#ifndef __MM_DAEMON_ENUMS_TYPES_H__\n#define __MM_DAEMON_ENUMS_TYPES_H__\n#include <glib-object.h>\nG_BEGIN_DECLS\n" \
    --ftail "G_END_DECLS\n#endif\n" \
    "$MM_SRC/src/mm-port.h" \
    "$MM_SRC/src/mm-port-serial-at.h" \
    "$MM_SRC/src/mm-base-bearer.h" \
    "$MM_SRC/src/mm-iface-modem-3gpp.h" \
    "$MM_SRC/src/mm-iface-modem-messaging.h" \
    > "$MM_WORK/gen/mm-daemon-enums-types.h" 2>/dev/null || true

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

echo "== 4. Staging mtk-wwan-connect, ALSA UCM2, and rootfs/advan-x1-support configs =="
install -m 0755 "$REPO/tools/mtk-wwan-connect/mtk-wwan-connect.py" \
    "$STAGE_DIR/usr/local/sbin/mtk-wwan-connect"
install -m 0644 "$REPO/tools/mtk-wwan-connect/mtk-wwan-connect.service" \
    "$STAGE_DIR/etc/systemd/system/mtk-wwan-connect.service"

install -m 0644 "$REPO/tools/ucm2/MediaTek/mt6789-mt6366/mt6789-mt6366.conf" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/mt6789-mt6366.conf"
install -m 0644 "$REPO/tools/ucm2/MediaTek/mt6789-mt6366/HiFi.conf" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/HiFi.conf"
install -m 0644 "$REPO/tools/ucm2/MediaTek/mt6789-mt6366/VoiceCall.conf" \
    "$STAGE_DIR/usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/VoiceCall.conf"
ln -sf ../../MediaTek/mt6789-mt6366/mt6789-mt6366.conf \
    "$STAGE_DIR/usr/share/alsa/ucm2/conf.d/mt6789-mt6366/mt6789-mt6366.conf"

install -m 0644 "$REPO/tools/pipewire/pipewire-pulse/50-mt6789-latency.conf" \
    "$STAGE_DIR/etc/pipewire/pipewire-pulse.conf.d/50-mt6789-latency.conf"
install -m 0644 "$REPO/tools/pipewire/wireplumber/50-mt6789-alsa.conf" \
    "$STAGE_DIR/etc/wireplumber/wireplumber.conf.d/50-mt6789-alsa.conf"

if [ -d "$REPO/rootfs/advan-x1-support" ]; then
    cp -a "$REPO/rootfs/advan-x1-support/." "$STAGE_DIR/"
fi

# Enforce strict clean permissions across all packaged /etc files and scripts
find "$STAGE_DIR/etc" -type d -exec chmod 0755 {} +
find "$STAGE_DIR/etc" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE_DIR/usr/local/bin/"* "$STAGE_DIR/usr/local/sbin/"* 2>/dev/null || true

echo "== 5. Static verification gates =="
# Gate A: Ensure legacy advan-power-screen.service is NEVER packaged
if find "$STAGE_DIR" -name '*advan-power-screen*' | grep -q .; then
    echo "FATAL: forbidden legacy advan-power-screen found in stage!" >&2
    exit 1
fi
# Gate B: Ensure no world-writable files under /etc
if find "$STAGE_DIR/etc" -perm -0002 | grep -q .; then
    echo "FATAL: world-writable file under /etc!" >&2
    exit 1
fi
# Gate C: Verify dynamic library resolution against target Debian sid arm64 sysroot
for bin in \
    "$STAGE_DIR/usr/local/sbin/ccci_fsd" \
    "$STAGE_DIR/usr/local/sbin/ccci_rpcd" \
    "$STAGE_DIR/usr/local/sbin/ccci_audd" \
    "$STAGE_DIR/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so"; do
    [ -f "$bin" ] || { echo "FATAL: missing expected binary $bin" >&2; exit 1; }
    echo "  ldd check: $(basename "$bin")"
    LDD_OUT="$(QEMU_LD_PREFIX="$SYSROOT" "$QEMU_AARCH64" -L "$SYSROOT" \
        "$SYSROOT/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" --list "$bin")"
    echo "$LDD_OUT"
    if echo "$LDD_OUT" | grep -q "not found"; then
        echo "FATAL: unresolved shared library in $bin" >&2
        exit 1
    fi
done

echo "== 6. Building Debian package advan-x1-support_${PKG_VER}_arm64.deb =="
cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: advan-x1-support
Version: ${PKG_VER}
Section: admin
Priority: optional
Architecture: arm64
Maintainer: sysretq0 <sysretq0@proton.me>
Depends: libc6, libglib2.0-0t64 | libglib2.0-0, libmm-glib0, util-linux, python3, alsa-ucm-conf, mobile-broadband-provider-info
Recommends: modemmanager, network-manager, pipewire-audio | pipewire, systemd-zram-generator
Description: Advan X1 (MediaTek MT6789) mainline Linux userspace support
 Self-contained device support package built with Google Clang:
  - ccci_fsd & ccci_rpcd read-only NVRAM overlay modem daemons
  - ccci_audd Gen95 voice speech mailbox & Hostless_Speech PCM2 bridge
  - ModemManager mtk-ccci plugin + udev rules + service drop-in
  - mtk-wwan-connect standalone fallback helper (active when ModemManager is absent)
  - USB ConfigFS composite gadget (NCM usb0 + ACM ttyGS0) + networkd config
  - ALSA UCM2 HiFi + Voice Call profiles for mt6789-mt6366 (FS1599 + MT6366)
  - PipeWire / WirePlumber MT6789 AFE latency & headroom drop-ins
  - Clean 0644 systemd journald/logind/zram/usb0/udev and GNOME Mobile defaults
EOF

cat > "$STAGE_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# Remove legacy bring-up services and debug helpers if upgrading an older rootfs
rm -f /etc/systemd/system/multi-user.target.wants/advan-power-screen.service \
      /etc/systemd/system/advan-power-screen.service \
      /usr/local/bin/advan-power-screen.py \
      /etc/systemd/network/20-usb0.network \
      /etc/systemd/system/multi-user.target.wants/boot-diag-dump.service \
      /etc/systemd/system/boot-diag-dump.service \
      /usr/local/bin/boot-diag-dump.sh \
      /usr/local/bin/test-touch.sh

mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/ccci-fsd.service \
       /etc/systemd/system/multi-user.target.wants/ccci-fsd.service
ln -sf /etc/systemd/system/ccci-audd.service \
       /etc/systemd/system/multi-user.target.wants/ccci-audd.service
ln -sf /etc/systemd/system/advan-x1-usb-gadget.service \
       /etc/systemd/system/multi-user.target.wants/advan-x1-usb-gadget.service
ln -sf /etc/systemd/system/mtk-wwan-connect.service \
       /etc/systemd/system/multi-user.target.wants/mtk-wwan-connect.service

# Enable ModemManager at boot when installed so cellular data/voice/SMS come up automatically
if [ -f /usr/lib/systemd/system/ModemManager.service ]; then
    ln -sf /usr/lib/systemd/system/ModemManager.service \
           /etc/systemd/system/multi-user.target.wants/ModemManager.service
elif [ -f /lib/systemd/system/ModemManager.service ]; then
    ln -sf /lib/systemd/system/ModemManager.service \
           /etc/systemd/system/multi-user.target.wants/ModemManager.service
fi

# Enable systemd-networkd so /etc/systemd/network/10-usb0.network configures usb0
if [ -f /usr/lib/systemd/system/systemd-networkd.service ]; then
    ln -sf /usr/lib/systemd/system/systemd-networkd.service \
           /etc/systemd/system/multi-user.target.wants/systemd-networkd.service
elif [ -f /lib/systemd/system/systemd-networkd.service ]; then
    ln -sf /lib/systemd/system/systemd-networkd.service \
           /etc/systemd/system/multi-user.target.wants/systemd-networkd.service
fi

if command -v glib-compile-schemas >/dev/null 2>&1 && [ -d /usr/share/glib-2.0/schemas ]; then
    glib-compile-schemas /usr/share/glib-2.0/schemas || true
fi
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    udevadm control --reload-rules || true
    systemctl try-restart ModemManager.service || true
    if [ -e /dev/ccci_aud ]; then
        systemctl restart ccci-audd.service || true
    fi
fi
exit 0
EOF
chmod 0755 "$STAGE_DIR/DEBIAN/postinst"

cat > "$STAGE_DIR/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
case "$1" in
    purge|remove)
        rm -f /etc/systemd/system/multi-user.target.wants/ccci-fsd.service \
              /etc/systemd/system/multi-user.target.wants/ccci-audd.service \
              /etc/systemd/system/multi-user.target.wants/advan-x1-usb-gadget.service \
              /etc/systemd/system/multi-user.target.wants/mtk-wwan-connect.service
        if [ -d /run/systemd/system ]; then
            systemctl daemon-reload || true
        fi
        ;;
esac
exit 0
EOF
chmod 0755 "$STAGE_DIR/DEBIAN/postrm"

DEB_NAME="advan-x1-support_${PKG_VER}_arm64.deb"
dpkg-deb -Zxz --build --root-owner-group "$STAGE_DIR" "$OUT_DIR/$DEB_NAME"
dpkg-deb --info "$OUT_DIR/$DEB_NAME"
dpkg-deb --contents "$OUT_DIR/$DEB_NAME"

# Gate D: Verify the .deb unpacks, configures (exercising postinst), and purges cleanly inside the Debian sid arm64 sysroot!
echo "== 7. Testing .deb installation & postinst inside Debian sid arm64 sysroot =="
sudo cp "$OUT_DIR/$DEB_NAME" "$SYSROOT/tmp/$DEB_NAME"
sudo chroot "$SYSROOT" /bin/bash -c "
    set -euo pipefail
    dpkg --unpack --force-depends '/tmp/$DEB_NAME'
    dpkg --configure --force-depends advan-x1-support
    test -L /etc/systemd/system/multi-user.target.wants/ccci-fsd.service
    test -L /etc/systemd/system/multi-user.target.wants/ccci-audd.service
    test -L /etc/systemd/system/multi-user.target.wants/advan-x1-usb-gadget.service
    test -L /etc/systemd/system/multi-user.target.wants/mtk-wwan-connect.service
    test -x /usr/local/sbin/ccci_audd
    test -x /usr/local/sbin/advan-x1-usb-gadget.sh
    test -f /etc/pipewire/pipewire-pulse.conf.d/50-mt6789-latency.conf
    test -f /etc/wireplumber/wireplumber.conf.d/50-mt6789-alsa.conf
    test -f /etc/systemd/zram-generator.conf
    dpkg --audit || true
    dpkg --purge advan-x1-support
    rm -f '/tmp/$DEB_NAME'
"

echo "== 8. Generating host-direct installer script & tarball =="
cat > "$OUT_DIR/install-userspace-tools.sh" <<EOF
#!/usr/bin/env bash
# Install CI-built advan-x1-support Debian package directly onto host / (rootfs).
set -euo pipefail
if [ "\$(id -u)" -ne 0 ]; then
    exec sudo -E "\$0" "\$@"
fi
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
DEB="\${1:-\$(ls -1 "\$DIR"/advan-x1-support_*_arm64.deb | head -n 1)}"
[ -f "\$DEB" ] || { echo "FATAL: advan-x1-support .deb not found" >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --reinstall "\$DEB"
echo "USERSPACE_TOOLS_INSTALL_OK"
EOF
chmod +x "$OUT_DIR/install-userspace-tools.sh"

TARBALL="advan-x1-userspace-tools-arm64.tar.xz"
XZ_OPT="-9e -T0" tar -C "$STAGE_DIR" --exclude='./DEBIAN' -cJf "$OUT_DIR/$TARBALL" etc usr
(
    cd "$OUT_DIR"
    sha256sum "$DEB_NAME" "$TARBALL" install-userspace-tools.sh > sha256sums.txt
    ls -lh "$DEB_NAME" "$TARBALL" install-userspace-tools.sh
    cat sha256sums.txt
)
echo "== Advan X1 userspace support package build complete =="
