#!/bin/bash
# Build the ADVAN X1 initramfs:
#   - Real static C /init binary (initramfs/src/init.c -> /init, PID 1)
#   - Static BusyBox rescue-only toolbox (getty + sh for rd.break/noswitch and
#     boot failures; never used on a successful boot)
#   - Out-of-tree .ko modules (staged in /lib/modules/extra/, loaded via
#     SYS_finit_module in /init) + optional targeted in-tree module dirs
#
# Output: $OUT/initramfs.cpio.gz  (ready for tools/pack_uboot_boot.py --initramfs)
#
# Env knobs:
#   K, WORK, OUT, JOBS       same as scripts/build.sh
#   INITRAMFS_ROOT           staging dir (default $OUT/initramfs-root)
#   BUSYBOX_ROOT             prebuilt BusyBox install tree; built if absent
#   REUSE_DIST_BUSYBOX=1     extract static BusyBox from dist/initramfs.cpio.gz
#                            if $BUSYBOX_ROOT is absent (fast offline local build)
#   EXTRA_KMODS              space-separated external .ko paths to stage in
#                            /lib/modules/extra/
#   INITRAMFS_MOD_DIRS       optional space-separated kernel subdirs to build
#                            as targeted =m modules (never builds all defconfig =m)
#   KVER                     override kernel release (auto from kernelrelease)
#   NO_O=1                   kernel builds in-tree (CI layout, no O= argument)
#   KMAKE_CC, KMAKE_HOSTCC   optional CC / HOSTCC values (e.g. "ccache clang")
#   KMAKE_EXTRA              extra make vars for the kernel
#   BUSYBOX_ONLY=1           build/populate $BUSYBOX_ROOT only and exit (CI fail-fast)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K=${K:-$REPO/../linux}
WORK=${WORK:-$REPO/../linux-worktree}
OUT=${OUT:-$REPO/build}
JOBS=${JOBS:-1}
ROOTFS=${INITRAMFS_ROOT:-$OUT/initramfs-root}
CROSS=${CROSS:-aarch64-linux-gnu-}
BUSYBOX_VER=${BUSYBOX_VER:-1.38.0}

export PATH="$REPO/scripts/bin:$PATH"
if [ "${BUSYBOX_ONLY:-0}" != "1" ]; then
    . "$REPO/scripts/toolchain-env.sh"
fi
mkdir -p "$OUT"

OARG=(O="$OUT")
KCFG="$OUT/.config"
if [ "${NO_O:-0}" = "1" ]; then
    OARG=()
    KCFG="$WORK/.config"
fi
kmake() {
    local extra=()
    [ -n "${KMAKE_CC:-}" ] && extra+=(CC="$KMAKE_CC")
    [ -n "${KMAKE_HOSTCC:-}" ] && extra+=(HOSTCC="$KMAKE_HOSTCC")
    make -C "$WORK" "${OARG[@]}" ARCH=arm64 LLVM=1 "${extra[@]}" ${KMAKE_EXTRA:-} "$@"
}

strip_ko() {
    local ko="$1"
    if command -v llvm-strip >/dev/null 2>&1; then
        llvm-strip --strip-unneeded "$ko"
    elif command -v "${CROSS}strip" >/dev/null 2>&1; then
        "${CROSS}strip" --strip-unneeded "$ko"
    fi
}

# ---------------------------------------------------------------- BusyBox ---
# BusyBox is included strictly as a rescue/debug toolbox (getty, sh, i2cget);
# PID 1 is our static C binary compiled from initramfs/src/init.c below.
BUSYBOX_ROOT=${BUSYBOX_ROOT:-$OUT/busybox-install}
if [ ! -x "$BUSYBOX_ROOT/bin/busybox" ]; then
    if [ "${REUSE_DIST_BUSYBOX:-0}" = "1" ] && [ -f "$REPO/dist/initramfs.cpio.gz" ]; then
        echo "== extract static BusyBox from dist/initramfs.cpio.gz =="
        rm -rf "$BUSYBOX_ROOT"
        mkdir -p "$BUSYBOX_ROOT"
        (
            cd "$BUSYBOX_ROOT"
            zcat "$REPO/dist/initramfs.cpio.gz" | cpio -idmu --quiet \
                'bin/*' 'sbin/*' 'usr/*' './bin/*' './sbin/*' './usr/*' 2>/dev/null || true
        )
    fi
fi

if [ ! -x "$BUSYBOX_ROOT/bin/busybox" ]; then
    echo "== build static BusyBox $BUSYBOX_VER (aarch64) =="
    BB_BUILD="$OUT/busybox-$BUSYBOX_VER"
    mkdir -p "$BB_BUILD"
    if [ ! -d "$BB_BUILD/busybox-$BUSYBOX_VER" ]; then
        curl -fsSL --connect-timeout 20 --max-time 180 --retry 3 \
            "https://busybox.net/downloads/busybox-$BUSYBOX_VER.tar.bz2" \
            | tar -xj -C "$BB_BUILD"
    fi
    (
        cd "$BB_BUILD/busybox-$BUSYBOX_VER"
        make defconfig >/dev/null
        sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
        sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config
        # BusyBox kconfig has no olddefconfig target; disable pipefail only in
        # this subshell so yes's SIGPIPE (141) is ignored while real oldconfig
        # errors still fail the build.
        (set +o pipefail; yes "" | make oldconfig >/dev/null)
        make -j"$JOBS" CROSS_COMPILE="$CROSS" install CONFIG_PREFIX="$BUSYBOX_ROOT" >/dev/null
    )
    ls -lh "$BUSYBOX_ROOT/bin/busybox"
fi

if [ "${BUSYBOX_ONLY:-0}" = "1" ]; then
    echo "== BusyBox ready ($BUSYBOX_ROOT/bin/busybox) =="
    exit 0
fi

# ------------------------------------------------------------- assemble ----
echo "== assemble $ROOTFS =="
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{dev,proc,sys,run,tmp,sysroot,etc,lib/modules/extra}

cp -a "$BUSYBOX_ROOT"/. "$ROOTFS"/
# Remove BusyBox's linuxrc symlink if present; PID 1 is our static C /init.
rm -f "$ROOTFS/linuxrc" "$ROOTFS/init"

# Copy runtime assets (never ship initramfs/src/ inside the cpio).
for _sub in etc lib; do
    if [ -d "$REPO/initramfs/$_sub" ]; then
        cp -a "$REPO/initramfs/$_sub"/. "$ROOTFS/$_sub"/
    fi
done

# ------------------------------------------------------- static C /init ----
echo "== build static C /init (aarch64) =="
make -C "$REPO/initramfs/src" CROSS="$CROSS" OUT="$ROOTFS" all
chmod 0755 "$ROOTFS/init"
ls -lh "$ROOTFS/init"

# ------------------------------------------------------- kernel modules ----
# Curated =m drivers only. Never `make modules`/`modules_install` on arm64
# defconfig: that pulls 850+ unrelated =m modules and blows the 32 MiB Space 1
# budget. Instead, build ALL active curated targets in a single native Kbuild
# single-module pass (`make dir/mod1.ko dir/mod2.ko ...`) so modpost resolves
# cross-module symbols cleanly against vmlinux.o with zero out-of-tree taint.
MOD_MANIFEST=${MOD_MANIFEST:-$REPO/config/initramfs-modules.txt}
KVER=""
if [ -f "$KCFG" ] && [ -f "$MOD_MANIFEST" ]; then
    KO_TARGETS=()
    while read -r _ko_rel _sym _rest; do
        case "$_ko_rel" in ''|'#'*) continue ;; esac
        if grep -qx "$_sym=m" "$KCFG"; then
            KO_TARGETS+=("$_ko_rel")
        fi
    done < "$MOD_MANIFEST"

    if [ "${#KO_TARGETS[@]}" -gt 0 ] || [ -n "${INITRAMFS_MOD_DIRS:-}" ]; then
        KVER=${KVER:-$(kmake -s kernelrelease 2>/dev/null \
            | grep -E '^[0-9]+\.' | tail -n 1)}
        [ -n "$KVER" ] || { echo "FATAL: cannot determine kernelrelease" >&2; exit 1; }
        MOD_DEST="$ROOTFS/lib/modules/$KVER/extra"
        mkdir -p "$MOD_DEST"
        KOBJ_ROOT="$OUT"
        [ "${NO_O:-0}" = "1" ] && KOBJ_ROOT="$WORK"

        if [ "${#KO_TARGETS[@]}" -gt 0 ]; then
            echo "== curated kmods for $KVER (${#KO_TARGETS[@]} modules) =="
            kmake -j"$JOBS" "${KO_TARGETS[@]}"
        fi

        STAGED_ORDER=()
        if [ "${#KO_TARGETS[@]}" -gt 0 ]; then
            for _ko_rel in "${KO_TARGETS[@]}"; do
                _src="$KOBJ_ROOT/$_ko_rel"
                [ -f "$_src" ] || { echo "FATAL: expected $_src not built" >&2; exit 1; }
                _base=$(basename "$_src")
                cp -f "$_src" "$MOD_DEST/$_base"
                strip_ko "$MOD_DEST/$_base"
                STAGED_ORDER+=("extra/$_base")
                echo "  staged $_base ($(du -h "$MOD_DEST/$_base" | cut -f1))"
            done
        fi

        for _dir in ${INITRAMFS_MOD_DIRS:-}; do
            kmake -j"$JOBS" M="$_dir" modules
            MOD_OUT="$KOBJ_ROOT/$_dir"
            [ -d "$MOD_OUT" ] || MOD_OUT="$WORK/$_dir"
            while IFS= read -r -d '' _ko; do
                _base=$(basename "$_ko")
                cp -f "$_ko" "$MOD_DEST/$_base"
                strip_ko "$MOD_DEST/$_base"
                STAGED_ORDER+=("extra/$_base")
                echo "  staged $_base ($(du -h "$MOD_DEST/$_base" | cut -f1))"
            done < <(find "$MOD_OUT" -maxdepth 2 -name '*.ko' -print0)
        done

        # /lib/modules/extra symlinks provide dependency-ordered fallback if
        # host depmod is unavailable during local builds.
        mkdir -p "$ROOTFS/lib/modules/extra"
        for _ko in "$MOD_DEST"/*.ko; do
            [ -e "$_ko" ] || continue
            ln -sf "../$KVER/extra/$(basename "$_ko")" \
                  "$ROOTFS/lib/modules/extra/$(basename "$_ko")"
        done

        # Populate kmod metadata files required by depmod so modules.dep and
        # modules.alias build cleanly without missing-file warnings.
        MOD_ROOT="$ROOTFS/lib/modules/$KVER"
        if [ "${#STAGED_ORDER[@]}" -gt 0 ]; then
            printf '%s\n' "${STAGED_ORDER[@]}" > "$MOD_ROOT/modules.order"
        else
            : > "$MOD_ROOT/modules.order"
        fi
        for _meta in modules.builtin modules.builtin.modinfo; do
            if [ -f "$KOBJ_ROOT/$_meta" ]; then
                cp -f "$KOBJ_ROOT/$_meta" "$MOD_ROOT/$_meta"
            else
                : > "$MOD_ROOT/$_meta"
            fi
        done

        if command -v depmod >/dev/null 2>&1; then
            DEPMOD_ARGS=(-a -b "$ROOTFS")
            [ -f "$KOBJ_ROOT/System.map" ] && DEPMOD_ARGS+=(-F "$KOBJ_ROOT/System.map")
            depmod "${DEPMOD_ARGS[@]}" "$KVER"
            [ -s "$MOD_ROOT/modules.dep" ] || { echo "FATAL: depmod produced empty modules.dep" >&2; exit 1; }
            echo "  modules.dep generated ($(wc -l < "$MOD_ROOT/modules.dep") entries)"
        else
            echo "  WARN: no host depmod; /init uses /etc/modules explicit order" >&2
        fi
    fi
fi

if [ -n "${EXTRA_KMODS:-}" ]; then
    for _ko in $EXTRA_KMODS; do
        [ -f "$_ko" ] || { echo "FATAL: EXTRA_KMODS missing: $_ko" >&2; exit 1; }
        _base=$(basename "$_ko")
        cp -f "$_ko" "$ROOTFS/lib/modules/extra/$_base"
        strip_ko "$ROOTFS/lib/modules/extra/$_base"
        echo "  staged /lib/modules/extra/$_base"
    done
fi

# --------------------------------------------------------------- package ----
echo "== cpio =="
(cd "$ROOTFS" && find . | sort | cpio -H newc -o --quiet | gzip -9 > "$OUT/initramfs.cpio.gz")
ls -lh "$OUT/initramfs.cpio.gz"
echo "== initramfs done =="
