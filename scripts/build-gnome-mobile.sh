#!/usr/bin/env bash
# Build GNOME Mobile (mutter-mobile + gnome-shell-mobile) for arm64 Debian sid/forky
# using Google's pinned AOSP Clang (x86_64 host cross-compiling to aarch64-linux-gnu)
# against a Debian sid arm64 sysroot.
#
# Output in $OUT_DIR:
#   gnome-mobile-<ver>-arm64.tar.xz
#   install-gnome-mobile.sh
#   sha256sums.txt
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CLANG_DIR="${CLANG_DIR:?CLANG_DIR must point to full Google Clang toolchain}"
SYSROOT="${SYSROOT:?SYSROOT must point to Debian sid arm64 sysroot}"
WORK_DIR="${WORK_DIR:-$REPO/build-gnome-mobile}"
STAGE_DIR="${STAGE_DIR:-$WORK_DIR/stage}"
OUT_DIR="${OUT_DIR:-$WORK_DIR/out}"
JOBS="${JOBS:-$(nproc)}"

MUTTER_REPO="${MUTTER_REPO:-https://gitlab.gnome.org/World/MobileShell/mutter-mobile.git}"
SHELL_REPO="${SHELL_REPO:-https://gitlab.gnome.org/World/MobileShell/gnome-shell-mobile.git}"
MUTTER_TAG="${MUTTER_TAG:-51.1.mobile.23}"
SHELL_TAG="${SHELL_TAG:-51.1.mobile.23}"

[ -x "$CLANG_DIR/bin/clang" ] || { echo "FATAL: missing $CLANG_DIR/bin/clang" >&2; exit 1; }
[ -x "$CLANG_DIR/bin/ld.lld" ] || { echo "FATAL: missing $CLANG_DIR/bin/ld.lld" >&2; exit 1; }
[ -d "$SYSROOT/usr/lib/aarch64-linux-gnu" ] || { echo "FATAL: invalid arm64 sysroot $SYSROOT" >&2; exit 1; }

GCC_DIR="$(find "$SYSROOT/usr/lib/gcc/aarch64-linux-gnu" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
[ -n "$GCC_DIR" ] || { echo "FATAL: no aarch64 gcc runtime dir in $SYSROOT/usr/lib/gcc/aarch64-linux-gnu" >&2; exit 1; }
QEMU_AARCH64="$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)"
[ -n "$QEMU_AARCH64" ] || { echo "FATAL: neither qemu-aarch64-static nor qemu-aarch64 found" >&2; exit 1; }
echo "== Toolchain: $CLANG_DIR/bin/clang (qemu: $QEMU_AARCH64) =="
"$CLANG_DIR/bin/clang" --version | head -n 2
echo "== Sysroot: $SYSROOT (gcc-install-dir: $GCC_DIR) =="

mkdir -p "$WORK_DIR" "$STAGE_DIR" "$OUT_DIR"
WRAP_DIR="$WORK_DIR/wrappers"
rm -rf "$WRAP_DIR" "$STAGE_DIR"
mkdir -p "$WRAP_DIR" "$STAGE_DIR"

# Host x86_64 Google Clang targeting aarch64-linux-gnu Debian sid sysroot.
# Only pass linker flags (-fuse-ld=lld, -L, -Wl,-rpath-link) when linking so
# Meson compile-only probes (-c -Werror=unused-command-line-argument) succeed.
cat > "$WRAP_DIR/aarch64-clang" <<EOF
#!/bin/sh
linking=1
for arg in "\$@"; do
  case "\$arg" in
    -c|-E|-S|-M|-MM) linking=0; break ;;
  esac
done
if [ "\$linking" = "1" ]; then
  exec "$CLANG_DIR/bin/clang" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    -fuse-ld=lld \\
    -isystem "$STAGE_DIR/usr/include" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -L"$GCC_DIR" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" \\
    "\$@" \\
    -Wno-unused-command-line-argument
else
  exec "$CLANG_DIR/bin/clang" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    -isystem "$STAGE_DIR/usr/include" \\
    "\$@" \\
    -Wno-unused-command-line-argument
fi
EOF

cat > "$WRAP_DIR/aarch64-clang++" <<EOF
#!/bin/sh
linking=1
for arg in "\$@"; do
  case "\$arg" in
    -c|-E|-S|-M|-MM) linking=0; break ;;
  esac
done
if [ "\$linking" = "1" ]; then
  exec "$CLANG_DIR/bin/clang++" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    -fuse-ld=lld \\
    -isystem "$STAGE_DIR/usr/include" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -L"$GCC_DIR" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" \\
    "\$@" \\
    -Wno-unused-command-line-argument
else
  exec "$CLANG_DIR/bin/clang++" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    -isystem "$STAGE_DIR/usr/include" \\
    "\$@" \\
    -Wno-unused-command-line-argument
fi
EOF

cat > "$WRAP_DIR/aarch64-pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu/pkgconfig:$STAGE_DIR/usr/share/pkgconfig"
# Rewrite staged .pc prefixes (which already live outside SYSROOT) cleanly
exec pkg-config "\$@"
EOF

cat > "$WRAP_DIR/qemu-arm64-exec" <<EOF
#!/bin/sh
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/gnome-shell:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export GI_TYPELIB_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/girepository-1.0:$SYSROOT/usr/lib/aarch64-linux-gnu/girepository-1.0\${GI_TYPELIB_PATH:+:\$GI_TYPELIB_PATH}"
exec "$QEMU_AARCH64" -L "$SYSROOT" "\$@"
EOF

# Wrapper for sysroot Python/ELF codegen and GIR tools so host Ubuntu never
# clashes with Debian sid GLib/GI/Wayland versions.
# Note: qemu-aarch64 only executes ELF binaries, never #! scripts, so detect
# shebang scripts explicitly: run pure-Python GLib codegen natively on host
# python3 and run g-ir-scanner via sysroot's arm64 python3 under qemu-aarch64.
wrap_sysroot_bin() {
    local name="$1"
    local target="$SYSROOT/usr/bin/$name"
    [ -e "$target" ] || return 0
    if [ "$name" = "g-ir-scanner" ]; then
        cat > "$WRAP_DIR/$name" <<EOF
#!/bin/sh
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export GI_TYPELIB_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/girepository-1.0:$SYSROOT/usr/lib/aarch64-linux-gnu/girepository-1.0\${GI_TYPELIB_PATH:+:\$GI_TYPELIB_PATH}"
export GI_GIR_PATH="$STAGE_DIR/usr/share/gir-1.0:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/share/gir-1.0\${GI_GIR_PATH:+:\$GI_GIR_PATH}"
export XDG_DATA_DIRS="$STAGE_DIR/usr/share:$SYSROOT/usr/share"
export PYTHONPATH="$SYSROOT/usr/lib/aarch64-linux-gnu/gobject-introspection:$SYSROOT/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}"
export CC="$WRAP_DIR/aarch64-clang"
export PKG_CONFIG="$WRAP_DIR/aarch64-pkg-config"
exec "$QEMU_AARCH64" -L "$SYSROOT" "$SYSROOT/usr/bin/python3" "$target" "\$@"
EOF
    elif head -c 2 "$target" 2>/dev/null | grep -q '^#!'; then
        cat > "$WRAP_DIR/$name" <<EOF
#!/bin/sh
export PYTHONPATH="$SYSROOT/usr/share/glib-2.0:$SYSROOT/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}"
exec python3 "$target" "\$@"
EOF
    else
        cat > "$WRAP_DIR/$name" <<EOF
#!/bin/sh
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export GI_TYPELIB_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/girepository-1.0:$SYSROOT/usr/lib/aarch64-linux-gnu/girepository-1.0\${GI_TYPELIB_PATH:+:\$GI_TYPELIB_PATH}"
export GI_GIR_PATH="$STAGE_DIR/usr/share/gir-1.0:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/share/gir-1.0\${GI_GIR_PATH:+:\$GI_GIR_PATH}"
export XDG_DATA_DIRS="$STAGE_DIR/usr/share:$SYSROOT/usr/share"
export CC="$WRAP_DIR/aarch64-clang"
export PKG_CONFIG="$WRAP_DIR/aarch64-pkg-config"
exec "$QEMU_AARCH64" -L "$SYSROOT" "$target" "\$@"
EOF
    fi
    chmod +x "$WRAP_DIR/$name"
}

chmod +x "$WRAP_DIR/aarch64-clang" "$WRAP_DIR/aarch64-clang++" \
         "$WRAP_DIR/aarch64-pkg-config" "$WRAP_DIR/qemu-arm64-exec"

for tool in g-ir-scanner g-ir-compiler g-ir-generate wayland-scanner \
            glib-compile-resources glib-compile-schemas glib-mkenums \
            glib-genmarshal gdbus-codegen sassc msgfmt msgmerge xgettext \
            gjs cvt xkbcomp; do
    wrap_sysroot_bin "$tool"
done

# Smoke-test Google Clang cross-compilation + lld linking + qemu execution
echo "== Smoke-testing Google Clang -> arm64 sysroot =="
printf '#include <stdio.h>\nint main(void){ puts("google-clang-arm64-ok"); return 0; }\n' \
    | "$WRAP_DIR/aarch64-clang" -x c - -o "$WORK_DIR/smoke"
"$WRAP_DIR/qemu-arm64-exec" "$WORK_DIR/smoke"
rm -f "$WORK_DIR/smoke"

# Meson cross-file using Google Clang + lld + qemu-aarch64-static exe_wrapper
CROSS_FILE="$WORK_DIR/arm64-google-clang.ini"
cat > "$CROSS_FILE" <<EOF
[binaries]
c = '$WRAP_DIR/aarch64-clang'
cpp = '$WRAP_DIR/aarch64-clang++'
ar = '$CLANG_DIR/bin/llvm-ar'
strip = '$CLANG_DIR/bin/llvm-strip'
nm = '$CLANG_DIR/bin/llvm-nm'
objcopy = '$CLANG_DIR/bin/llvm-objcopy'
pkg-config = '$WRAP_DIR/aarch64-pkg-config'
pkgconfig = '$WRAP_DIR/aarch64-pkg-config'
exe_wrapper = '$WRAP_DIR/qemu-arm64-exec'
g-ir-scanner = '$WRAP_DIR/g-ir-scanner'
g-ir-compiler = '$WRAP_DIR/g-ir-compiler'
wayland-scanner = '$WRAP_DIR/wayland-scanner'
glib-compile-resources = '$WRAP_DIR/glib-compile-resources'
glib-compile-schemas = '$WRAP_DIR/glib-compile-schemas'
glib-mkenums = '$WRAP_DIR/glib-mkenums'
glib-genmarshal = '$WRAP_DIR/glib-genmarshal'
gdbus-codegen = '$WRAP_DIR/gdbus-codegen'
sassc = '$WRAP_DIR/sassc'
gjs = '$WRAP_DIR/gjs'

[built-in options]
c_args = ['-O2']
cpp_args = ['-O2']
werror = false

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = '$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig'
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

export PATH="$WRAP_DIR:$CLANG_DIR/bin:$PATH"
export QEMU_LD_PREFIX="$SYSROOT"
export GI_CROSS_LAUNCHER="$WRAP_DIR/qemu-arm64-exec"

clone_tag() {
    local url="$1" tag="$2" dst="$3"
    if [ ! -d "$dst/.git" ]; then
        git clone --depth 1 --branch "$tag" "$url" "$dst"
    fi
}

echo "== 1. Cloning mutter-mobile ($MUTTER_TAG) & gnome-shell-mobile ($SHELL_TAG) =="
clone_tag "$MUTTER_REPO" "$MUTTER_TAG" "$WORK_DIR/mutter-mobile"
clone_tag "$SHELL_REPO" "$SHELL_TAG" "$WORK_DIR/gnome-shell-mobile"

echo "== 2. Building mutter-mobile ($MUTTER_TAG) with Google Clang =="
rm -rf "$WORK_DIR/build-mutter"
meson setup "$WORK_DIR/build-mutter" "$WORK_DIR/mutter-mobile" \
    --cross-file "$CROSS_FILE" \
    -Dbuildtype=release \
    -Dwerror=false \
    -Dprefix=/usr \
    -Dlibdir=lib/aarch64-linux-gnu \
    -Dlibexecdir=libexec \
    -Dtests=disabled \
    -Dinstalled_tests=false \
    -Ddocs=false \
    -Dprofiler=false \
    -Dxwayland=true \
    -Ddevkit=disabled \
    -Dbash_completion=false
ninja -C "$WORK_DIR/build-mutter" -j"$JOBS"
DESTDIR="$STAGE_DIR" ninja -C "$WORK_DIR/build-mutter" install

# Mirror staged mutter-mobile headers/libs/pc/gir into sysroot so gnome-shell
# pkg-config (--sysroot) and g-ir-scanner resolve mutter 51.1.mobile.23 seamlessly.
echo "== 3. Syncing staged mutter-mobile into sysroot for gnome-shell build =="
cp -a "$STAGE_DIR/usr/." "$SYSROOT/usr/"

echo "== 4. Building gnome-shell-mobile ($SHELL_TAG) with Google Clang =="
rm -rf "$WORK_DIR/build-gnome-shell"
meson setup "$WORK_DIR/build-gnome-shell" "$WORK_DIR/gnome-shell-mobile" \
    --cross-file "$CROSS_FILE" \
    -Dbuildtype=release \
    -Dwerror=false \
    -Dprefix=/usr \
    -Dlibdir=lib/aarch64-linux-gnu \
    -Dlibexecdir=libexec \
    -Dtests=false \
    -Dman=false \
    -Dgtk_doc=false \
    -Dcamera_monitor=false \
    -Dnetworkmanager=true \
    -Dsystemd=true
ninja -C "$WORK_DIR/build-gnome-shell" -j"$JOBS"
DESTDIR="$STAGE_DIR" ninja -C "$WORK_DIR/build-gnome-shell" install

echo "== 5. Stripping staged ELF binaries/libraries with llvm-strip =="
find "$STAGE_DIR/usr" -type f \( -executable -o -name '*.so*' \) | while read -r f; do
    if file -b "$f" | grep -q 'ELF 64-bit LSB'; then
        "$CLANG_DIR/bin/llvm-strip" --strip-unneeded "$f" 2>/dev/null || true
    fi
done

# Drop build-only development artifacts from runtime tarball
rm -rf "$STAGE_DIR/usr/include" \
       "$STAGE_DIR/usr/lib/aarch64-linux-gnu/pkgconfig" \
       "$STAGE_DIR/usr/share/pkgconfig" \
       "$STAGE_DIR/usr/share/man" \
       "$STAGE_DIR/usr/share/gir-1.0"

echo "== 6. Generating host-direct rootfs installer script =="
cat > "$OUT_DIR/install-gnome-mobile.sh" <<'EOF'
#!/usr/bin/env bash
# Install CI-built GNOME Mobile (mutter-mobile + gnome-shell-mobile) directly
# onto the live Advan X1 Debian arm64 rootfs (host /) with sudo.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

TARBALL="${1:-$(dirname "$0")/gnome-mobile-51.1.mobile.23-arm64.tar.xz}"
TARGET_USER="${TARGET_USER:-diky}"

[ -f "$TARBALL" ] || { echo "FATAL: tarball not found: $TARBALL" >&2; exit 1; }

echo "=== 1. Ensure Debian GNOME base runtime packages (gdm3, gnome-shell deps) ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    gdm3 gnome-shell gnome-shell-common mutter mutter-common mutter-common-bin \
    gnome-console nautilus gnome-text-editor loupe gnome-calculator gnome-control-center \
    fonts-cantarell adwaita-icon-theme xwayland

echo "=== 2. Extract CI-built GNOME Mobile overlay onto / ==="
tar -xJf "$TARBALL" -C /

echo "=== 3. Write Advan X1 GNOME Mobile defaults & GDM / USB-NCM configs ==="
cat > /usr/share/glib-2.0/schemas/90_advan-x1-mobile.gschema.override << 'GSC'
[org.gnome.desktop.a11y.applications]
screen-keyboard-enabled=true

[org.gnome.desktop.session]
idle-delay=uint32 0

[org.gnome.settings-daemon.plugins.power]
sleep-inactive-battery-type='nothing'
sleep-inactive-ac-type='nothing'
ambient-enabled=false

[org.gnome.shell]
favorite-apps=['org.gnome.Console.desktop', 'org.gnome.Nautilus.desktop', 'org.gnome.TextEditor.desktop', 'org.gnome.Loupe.desktop', 'org.gnome.Calculator.desktop', 'org.gnome.Settings.desktop']
GSC

mkdir -p /etc/gdm3
cat > /etc/gdm3/daemon.conf << GDC
[daemon]
WaylandEnable=true
AutomaticLoginEnable=true
AutomaticLogin=${TARGET_USER}

[security]

[debug]
GDC

ln -sf /usr/lib/systemd/system/graphical.target /etc/systemd/system/default.target
ln -sf /usr/lib/systemd/system/gdm.service /etc/systemd/system/display-manager.service

mkdir -p /etc/systemd/network /etc/NetworkManager/conf.d
cat > /etc/systemd/network/10-usb0.network << 'NET'
[Match]
Name=usb0

[Network]
Address=10.15.19.82/24
ConfigureWithoutCarrier=yes
NET

cat > /etc/NetworkManager/conf.d/10-unmanaged-usb0.conf << 'NM'
[keyfile]
unmanaged-devices=interface-name:usb0
NM

echo "=== 4. Compile schemas, refresh caches, hold mobile packages, verify ==="
if id "$TARGET_USER" >/dev/null 2>&1; then
    usermod -aG audio,video,render,input,netdev,sudo "$TARGET_USER"
fi
ldconfig
glib-compile-schemas --strict /usr/share/glib-2.0/schemas
gtk4-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
apt-mark hold mutter libmutter-51-0 gir1.2-mutter-51 mutter-common mutter-common-bin gnome-shell gnome-shell-common || true

mutter --version
gnome-shell --version
MISSING=$(ldd /usr/bin/gnome-shell /usr/bin/mutter /usr/lib/aarch64-linux-gnu/libmutter-51.so.0 /usr/lib/aarch64-linux-gnu/gnome-shell/libshell-51.so /usr/lib/aarch64-linux-gnu/gnome-shell/libst-51.so 2>/dev/null | grep "not found" || true)
if [ -n "$MISSING" ]; then
    echo "ERROR: Missing shared libs:" >&2
    echo "$MISSING" >&2
    exit 1
fi
echo "GNOME_MOBILE_INSTALL_OK"
EOF
chmod +x "$OUT_DIR/install-gnome-mobile.sh"

TARBALL="gnome-mobile-${MUTTER_TAG}-arm64.tar.xz"
echo "== 7. Packing $OUT_DIR/$TARBALL =="
XZ_OPT="-9e -T0" tar -C "$STAGE_DIR" -cJf "$OUT_DIR/$TARBALL" usr
(
    cd "$OUT_DIR"
    sha256sum "$TARBALL" install-gnome-mobile.sh > sha256sums.txt
    ls -lh "$TARBALL" install-gnome-mobile.sh
    cat sha256sums.txt
)
echo "== GNOME Mobile build complete =="
