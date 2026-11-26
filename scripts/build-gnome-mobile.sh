#!/usr/bin/env bash
# Build GNOME Mobile (mutter-mobile + gnome-shell-mobile) for arm64 Debian sid/forky
# using Google's pinned AOSP Clang (x86_64 host cross-compiling to aarch64-linux-gnu)
# against a Debian sid arm64 sysroot.
#
# Output in $OUT_DIR:
#   advan-x1-gnome-mobile_<ver>_arm64.deb  (uses dpkg-divert — clean apt/dpkg integration)
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
    "\$@" \\
    -isystem "$STAGE_DIR/usr/include" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -L"$GCC_DIR" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" \\
    -Wno-unused-command-line-argument
else
  exec "$CLANG_DIR/bin/clang" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    "\$@" \\
    -isystem "$STAGE_DIR/usr/include" \\
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
    "\$@" \\
    -isystem "$STAGE_DIR/usr/include" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -L"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -L"$GCC_DIR" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51" \\
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" \\
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" \\
    -Wno-unused-command-line-argument
else
  exec "$CLANG_DIR/bin/clang++" \\
    --target=aarch64-linux-gnu \\
    --sysroot="$SYSROOT" \\
    --gcc-install-dir="$GCC_DIR" \\
    "\$@" \\
    -isystem "$STAGE_DIR/usr/include" \\
    -Wno-unused-command-line-argument
fi
EOF

cat > "$WRAP_DIR/aarch64-pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_PATH="$STAGE_DIR/usr/lib/aarch64-linux-gnu/pkgconfig:$STAGE_DIR/usr/share/pkgconfig"
out="\$(pkg-config "\$@")"
rc=\$?
[ \$rc -eq 0 ] || exit \$rc
var=""
prev=""
for arg in "\$@"; do
  if [ "\$prev" = "--variable" ]; then
    var="\$arg"
    break
  fi
  case "\$arg" in
    --variable=*) var="\${arg#--variable=}"; break ;;
    --variable) prev="--variable" ;;
  esac
done
case "\$var" in
  typelibdir|xwayland|systemduserunitdir|systemdsystemunitdir|udevdir|libexecdir|keybindings_dir|gnome_keybindings_keysdir)
    printf '%s\n' "\${out#"$SYSROOT"}"
    ;;
  *)
    printf '%s\n' "\$out"
    ;;
esac
EOF

cat > "$WRAP_DIR/qemu-arm64-exec" <<EOF
#!/bin/sh
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="\${LD_LIBRARY_PATH:+\$LD_LIBRARY_PATH:}$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/gnome-shell:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu"
export GI_TYPELIB_PATH="\${GI_TYPELIB_PATH:+\$GI_TYPELIB_PATH:}$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/girepository-1.0:$SYSROOT/usr/lib/aarch64-linux-gnu/girepository-1.0"
exec "$QEMU_AARCH64" -L "$SYSROOT" "\$@"
EOF

# Target-aware ldd wrapper so g-ir-scanner's resolve_shlibs() resolves arm64 ELF
# binaries cleanly via the sysroot's aarch64 dynamic loader instead of host x86_64 ldd.
cat > "$WRAP_DIR/ldd" <<EOF
#!/bin/sh
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="\${LD_LIBRARY_PATH:+\$LD_LIBRARY_PATH:}$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/gnome-shell:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu"
exec "$QEMU_AARCH64" -L "$SYSROOT" "$SYSROOT/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" --list "\$@"
EOF
chmod +x "$WRAP_DIR/ldd"

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
export PATH="$WRAP_DIR:\$PATH"
export QEMU_LD_PREFIX="$SYSROOT"
export LD_LIBRARY_PATH="\${LD_LIBRARY_PATH:+\$LD_LIBRARY_PATH:}$STAGE_DIR/usr/lib/aarch64-linux-gnu:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/lib/aarch64-linux-gnu:$SYSROOT/lib/aarch64-linux-gnu"
export GI_TYPELIB_PATH="\${GI_TYPELIB_PATH:+\$GI_TYPELIB_PATH:}$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$STAGE_DIR/usr/lib/aarch64-linux-gnu/girepository-1.0:$SYSROOT/usr/lib/aarch64-linux-gnu/girepository-1.0"
export GI_GIR_PATH="\${GI_GIR_PATH:+\$GI_GIR_PATH:}$STAGE_DIR/usr/share/gir-1.0:$STAGE_DIR/usr/lib/aarch64-linux-gnu/mutter-51:$SYSROOT/usr/share/gir-1.0"
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

echo "== 1b. Patching gnome-shell-mobile lock/wake & DSI brightness bugs =="
python3 - "$WORK_DIR/gnome-shell-mobile" <<'PYEOF'
import pathlib, sys

root = pathlib.Path(sys.argv[1])

# 1) screenShield.js:
#    - Mutter 51 does not export Meta.PowerSaveMode in Meta-51.typelib
#      (power_save_mode is raw gint, META_POWER_SAVE_ON == 0).
#    - Mobile UnlockDialog implements cancelUnlock(), not cancel().
ss_path = root / "js/ui/screenShield.js"
ss = ss_path.read_text()
orig_ss = ss
ss = ss.replace(
    "Meta.PowerSaveMode.ON",
    "(Meta.PowerSaveMode?.ON ?? 0)",
)
ss = ss.replace(
    "this._dialog.cancel();",
    "(this._dialog.cancel ?? this._dialog.cancelUnlock)?.call(this._dialog);",
)
ss = ss.replace(
    "this._dialog?.cancel();",
    "(this._dialog?.cancel ?? this._dialog?.cancelUnlock)?.call(this._dialog);",
)
assert ss != orig_ss, "screenShield.js patch matched nothing"
assert "Meta.PowerSaveMode.ON" not in ss, "Meta.PowerSaveMode.ON still present"
assert "this._dialog.cancel()" not in ss and "this._dialog?.cancel()" not in ss, "unguarded _dialog.cancel() still present in screenShield.js"
ss_path.write_text(ss)

# 2) brightnessManager.js:
#    - DSI panels without EDID return null for vendor/product/serial, which
#      makes GLib.Variant('a(sssssd)', ...) throw Argument string may not be null.
bm_path = root / "js/misc/brightnessManager.js"
bm = bm_path.read_text()
orig_bm = bm
for prop, expr in [
    ("vendor: monitor.get_vendor(),", "vendor: monitor.get_vendor() ?? '',"),
    ("product: monitor.get_product(),", "product: monitor.get_product() ?? '',"),
    ("serial: monitor.get_serial(),", "serial: monitor.get_serial() ?? '',"),
    ("colorMode: monitor.get_color_mode_string(),", "colorMode: monitor.get_color_mode_string() ?? '',"),
    ("connector: monitor.get_connector(),", "connector: monitor.get_connector() ?? '',"),
]:
    assert prop in bm, f"expected {prop!r} in brightnessManager.js"
    bm = bm.replace(prop, expr, 1)
assert bm != orig_bm
bm_path.write_text(bm)
print("Patched screenShield.js and brightnessManager.js cleanly")
PYEOF

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
    -Dxwayland_path=/usr/bin/Xwayland \
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

# Drop build-only development artifacts from runtime package
rm -rf "$STAGE_DIR/usr/include" \
       "$STAGE_DIR/usr/lib/aarch64-linux-gnu/pkgconfig" \
       "$STAGE_DIR/usr/share/pkgconfig" \
       "$STAGE_DIR/usr/share/man" \
       "$STAGE_DIR/usr/share/gir-1.0"

# Stock Debian gnome-settings-daemon does not define the pmOS 'toggle-blank' enum
# value for power-button-action; strip that unsupported stanza so
# glib-compile-schemas --strict stays 100% clean and power-button-action stays 'suspend'.
MOBILE_OVERRIDE="$STAGE_DIR/usr/share/glib-2.0/schemas/10_org.gnome.shell.mobile.gschema.override"
if [ -f "$MOBILE_OVERRIDE" ]; then
    python3 - "$MOBILE_OVERRIDE" <<'PYEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
lines = p.read_text().splitlines()
out = []
skip = False
for line in lines:
    if line.strip().startswith('[org.gnome.settings-daemon.plugins.power'):
        skip = True
        continue
    if skip and line.strip().startswith('['):
        skip = False
    if not skip:
        out.append(line)
p.write_text('\n'.join(out).rstrip() + '\n')
PYEOF
fi

# Rewrite build-time wrapper & meson-venv paths in installed text/script/dbus files
# to target paths (e.g. Meson embeds find_program('gjs') into D-Bus .service files
# and its own venv python3 into gnome-shell-*-tool shebangs).
while IFS= read -r -d '' f; do
    if grep -qF "$WRAP_DIR/gjs" "$f" 2>/dev/null; then
        sed -i "s|$WRAP_DIR/gjs|/usr/bin/gjs|g" "$f"
    fi
    if head -n 1 "$f" 2>/dev/null | grep -q '^#!/home/runner/.*python3'; then
        sed -i '1s|^#!/home/runner/.*python3.*|#!/usr/bin/python3|' "$f"
    fi
done < <(find "$STAGE_DIR/usr" -type f -print0)

echo "== 6. Static gate A: verify ZERO leaked CI/sysroot paths in staged files =="
LEAK_FOUND=0
while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'ELF 64-bit LSB'; then
        HITS="$({ readelf -d "$f" 2>/dev/null; strings "$f" 2>/dev/null; } | grep -F -e "$SYSROOT" -e "$STAGE_DIR" -e "$WORK_DIR" -e "/home/runner" || true)"
        if [ -n "$HITS" ]; then
            echo "FATAL: leaked host/sysroot path in ELF $f:" >&2
            echo "$HITS" | head -n 10 >&2
            LEAK_FOUND=1
        fi
    else
        THITS="$(grep -nIF -e "$SYSROOT" -e "$STAGE_DIR" -e "$WORK_DIR" -e "/home/runner" "$f" 2>/dev/null || true)"
        if [ -n "$THITS" ]; then
            echo "FATAL: leaked host/sysroot path in text file $f:" >&2
            echo "$THITS" | head -n 10 >&2
            LEAK_FOUND=1
        fi
    fi
done < <(find "$STAGE_DIR/usr" -type f -print0)
[ "$LEAK_FOUND" -eq 0 ] || exit 1
echo "  PASS: zero leaked sysroot/CI paths in ALL ELF binaries and text files"

echo "== 7. Packaging advan-x1-gnome-mobile_${MUTTER_TAG}_arm64.deb (with dpkg-divert) =="
PKG_STAGE="$WORK_DIR/deb-stage"
rm -rf "$PKG_STAGE"
mkdir -p "$PKG_STAGE/DEBIAN"
cp -a "$STAGE_DIR/usr" "$PKG_STAGE/"

DIVERT_LIST="$WORK_DIR/diverts.list"
(cd "$PKG_STAGE" && find usr \( -type f -o -type l \) | sort | sed 's|^|/|') > "$DIVERT_LIST"
echo "  Diverting $(wc -l < "$DIVERT_LIST") target paths cleanly via dpkg-divert"

cat > "$PKG_STAGE/DEBIAN/control" <<EOF
Package: advan-x1-gnome-mobile
Version: ${MUTTER_TAG}
Section: x11
Priority: optional
Architecture: arm64
Maintainer: sysretq0 <324514544+sysretq0@users.noreply.github.com>
Depends: gdm3, gnome-session, gnome-shell, gnome-shell-common, mutter, mutter-common, mutter-common-bin, xwayland, gir1.2-modemmanager-1.0
Recommends: ibus, rtkit, gnome-console, nautilus, gnome-text-editor, loupe, gnome-calculator, gnome-control-center, fonts-cantarell, adwaita-icon-theme, advan-x1-support
Description: GNOME Mobile (mutter-mobile + gnome-shell-mobile) for Advan X1
 Built with Google AOSP Clang targeting Debian sid/forky arm64.
 Uses dpkg-divert into /usr/lib/advan-x1-gnome-mobile-diverted so stock
 Debian mutter/gnome-shell packages stay 100% consistent with dpkg/apt,
 survive apt upgrades cleanly, and restore cleanly on package removal.
EOF

{
    cat <<'PRE_HEAD'
#!/bin/sh
set -e
DIVERT_ROOT="/usr/lib/advan-x1-gnome-mobile-diverted"
case "$1" in
    install|upgrade)
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            owner="$(dpkg-divert --listpackage "$f" 2>/dev/null || true)"
            if [ "$owner" != "advan-x1-gnome-mobile" ]; then
                dest="${DIVERT_ROOT}${f}"
                mkdir -p "$(dirname "$dest")"
                dpkg-divert --package advan-x1-gnome-mobile --add --rename --divert "$dest" "$f" >/dev/null
            fi
        done <<'DIVERT_EOF'
PRE_HEAD
    cat "$DIVERT_LIST"
    cat <<'PRE_TAIL'
DIVERT_EOF
        ;;
esac
exit 0
PRE_TAIL
} > "$PKG_STAGE/DEBIAN/preinst"
chmod 0755 "$PKG_STAGE/DEBIAN/preinst"

cat > "$PKG_STAGE/DEBIAN/postinst" <<'POST_EOF'
#!/bin/sh
set -e
ldconfig
if command -v glib-compile-schemas >/dev/null 2>&1 && [ -d /usr/share/glib-2.0/schemas ]; then
    glib-compile-schemas /usr/share/glib-2.0/schemas
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1 && [ -d /usr/share/icons/hicolor ]; then
    gtk4-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
POST_EOF
chmod 0755 "$PKG_STAGE/DEBIAN/postinst"

{
    cat <<'POSTRM_HEAD'
#!/bin/sh
set -e
DIVERT_ROOT="/usr/lib/advan-x1-gnome-mobile-diverted"
case "$1" in
    remove|abort-install|disappear)
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            owner="$(dpkg-divert --listpackage "$f" 2>/dev/null || true)"
            if [ "$owner" = "advan-x1-gnome-mobile" ]; then
                dest="${DIVERT_ROOT}${f}"
                dpkg-divert --package advan-x1-gnome-mobile --remove --rename --divert "$dest" "$f" >/dev/null
            fi
        done <<'DIVERT_EOF'
POSTRM_HEAD
    cat "$DIVERT_LIST"
    cat <<'POSTRM_TAIL'
DIVERT_EOF
        rm -rf "$DIVERT_ROOT"
        ldconfig || true
        if command -v glib-compile-schemas >/dev/null 2>&1 && [ -d /usr/share/glib-2.0/schemas ]; then
            glib-compile-schemas /usr/share/glib-2.0/schemas || true
        fi
        ;;
esac
exit 0
POSTRM_TAIL
} > "$PKG_STAGE/DEBIAN/postrm"
chmod 0755 "$PKG_STAGE/DEBIAN/postrm"

DEB_NAME="advan-x1-gnome-mobile_${MUTTER_TAG}_arm64.deb"
dpkg-deb -Zxz --build --root-owner-group "$PKG_STAGE" "$OUT_DIR/$DEB_NAME"
dpkg-deb --info "$OUT_DIR/$DEB_NAME"

echo "== 8. Static gate B: install .deb in arm64 chroot & verify runtime WITHOUT LD_LIBRARY_PATH =="
sudo cp "$OUT_DIR/$DEB_NAME" "$SYSROOT/tmp/$DEB_NAME"
sudo env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin chroot "$SYSROOT" /bin/bash -c "
    set -euo pipefail
    dpkg --unpack --force-depends '/tmp/$DEB_NAME'
    dpkg --configure --force-depends advan-x1-gnome-mobile
    echo '  Checking mutter --version & gnome-shell --version (no LD_LIBRARY_PATH):'
    MVER=\$(mutter --version)
    SVER=\$(gnome-shell --version)
    echo \"    \$MVER\"
    echo \"    \$SVER\"
    echo \"\$MVER\" | grep -q '${MUTTER_TAG}'
    echo \"\$SVER\" | grep -q '${MUTTER_TAG}'
    MISSING=\$(ldd \
        /usr/bin/gnome-shell \
        /usr/bin/mutter \
        /usr/lib/aarch64-linux-gnu/libmutter-51.so.0 \
        /usr/lib/aarch64-linux-gnu/gnome-shell/libshell-51.so \
        /usr/lib/aarch64-linux-gnu/gnome-shell/libst-51.so \
        2>&1 | grep 'not found' || true)
    if [ -n \"\$MISSING\" ]; then
        echo 'FATAL: unresolved shared libs in chroot:' >&2
        echo \"\$MISSING\" >&2
        exit 1
    fi
    echo '  Testing clean purge & dpkg-divert restoration:'
    dpkg --purge advan-x1-gnome-mobile
    LEFT=\$(dpkg-divert --list 'advan-x1-gnome-mobile' || true)
    if [ -n \"\$LEFT\" ]; then
        echo 'FATAL: leftover diversions after purge:' >&2
        echo \"\$LEFT\" >&2
        exit 1
    fi
    rm -f '/tmp/$DEB_NAME'
"
echo "  PASS: .deb install, runtime version/ldd (no LD_LIBRARY_PATH), and purge/divert restore all green!"

echo "== 9. Generating host-direct rootfs installer script =="
cat > "$OUT_DIR/install-gnome-mobile.sh" <<'EOF'
#!/usr/bin/env bash
# Install CI-built advan-x1-gnome-mobile .deb (dpkg-divert) directly
# onto the live Advan X1 Debian arm64 rootfs (host /) with sudo.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
DEB="${1:-$(ls -1 "$DIR"/advan-x1-gnome-mobile_*_arm64.deb | head -n 1)}"
TARGET_USER="${TARGET_USER:-diky}"

[ -f "$DEB" ] || { echo "FATAL: advan-x1-gnome-mobile .deb not found" >&2; exit 1; }

echo "=== 1. Install advan-x1-gnome-mobile .deb + GNOME apps via apt ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    "$DEB" \
    ibus rtkit \
    gnome-console nautilus gnome-text-editor loupe gnome-calculator gnome-control-center \
    fonts-cantarell adwaita-icon-theme

echo "=== 2. Configure GDM3 Wayland, graphical.target, and user groups ==="
mkdir -p /etc/gdm3
cat > /etc/gdm3/daemon.conf << 'GDC'
[daemon]
WaylandEnable=true

[security]

[debug]
GDC
chmod 0644 /etc/gdm3/daemon.conf

ln -sf /usr/lib/systemd/system/graphical.target /etc/systemd/system/default.target
ln -sf /usr/lib/systemd/system/gdm.service /etc/systemd/system/display-manager.service

if id "$TARGET_USER" >/dev/null 2>&1; then
    for g in audio video render input netdev sudo; do
        if getent group "$g" >/dev/null; then
            usermod -aG "$g" "$TARGET_USER"
        fi
    done
fi

echo "=== 3. Verify versions, dpkg status, and ldd ==="
dpkg -s advan-x1-gnome-mobile | grep -E '^(Package|Status|Version):'
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

(
    cd "$OUT_DIR"
    sha256sum "$DEB_NAME" install-gnome-mobile.sh > sha256sums.txt
    ls -lh "$DEB_NAME" install-gnome-mobile.sh
    cat sha256sums.txt
)
echo "== GNOME Mobile .deb build complete =="
