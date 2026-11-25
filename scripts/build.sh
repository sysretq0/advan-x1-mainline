#!/bin/bash
# Advan X1 port — clean build pipeline (U-Boot FIT + boot.scr).
# Pristine mainline checkout + patches/ series + config frags + DTs ->
#   Image, DTB, SHA-256 FIT (advan-x1.itb), boot.scr, uEnv.txt (<=32 MiB single or 2-space split).
# Kernel tree: /work/mainline/linux (stays pristine; patches apply on worktree).
# Toolchain: Google's pinned AOSP clang (default clang-r614150, from the
# android-mainline build config) on PATH; LLVM=1. GOOGLE_CLANG=0 falls back
# to the host LLVM install. CI caches the toolchain under toolchain/.
# Resource limit: JOBS=1 to prevent OOM on 3.2 GB RAM host.
# Usage:
#   ./scripts/build.sh           -> Full local build (Image + DTB + U-Boot FIT/boot.scr)
#   ./scripts/build.sh check     -> Fast local subsystem check (drivers/clk/mediatek/)
# Env knobs:
#   COMPRESSION=gzip|lzma|lz4|none  (default: gzip, ~22.7 MiB FIT)
#   BOOT_MODE=auto|single|split     (default: auto, splits automatically if > MAX_SIZE)
#   MAX_SIZE=32M                    (default: 32M Space 1 budget; 48M for linux_boot)
#   PAYLOAD_LABEL=linux_boot        (default: linux_boot Space 2 GPT label)
#   INITRD=/path/to/initramfs.cpio.gz
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K=${K:-/work/mainline/linux}
WORK=${WORK:-/work/mainline/linux-worktree}
OUT=${OUT:-$REPO/build}
JOBS=${JOBS:-1}
BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}
BRANCH=port-series
MODE=${1:-full}

export PATH="$REPO/scripts/bin:$PATH"

# Google-clang style: resolve clang/ld.lld/llvm-* from PATH with LLVM=1.
. "$REPO/scripts/toolchain-env.sh"

# Host LLVM fallback symlinks (only when GOOGLE_CLANG=0).
if [ "${GOOGLE_CLANG:-0}" = "0" ]; then
    BIN="$REPO/scripts/bin"
    mkdir -p "$BIN"
    for tool in ar nm objcopy objdump readelf strip as size strings; do
        if [ ! -e "$BIN/llvm-$tool" ] && [ -f "/usr/bin/llvm-$tool-21" ]; then
            ln -s "/usr/bin/llvm-$tool-21" "$BIN/llvm-$tool" 2>/dev/null || true
        fi
    done
    if [ ! -e "$BIN/ld.lld" ] && [ -f "/usr/bin/ld.lld-21" ]; then
        ln -s "/usr/bin/ld.lld-21" "$BIN/ld.lld" 2>/dev/null || true
    fi
fi
mkdir -p "$OUT"

cd "$K"
echo "== patch series on $BRANCH =="
if [ ! -e "$WORK/.git" ]; then
    git worktree remove --force "$WORK" 2>/dev/null || true
    rm -rf "$WORK"
    git worktree add --detach "$WORK" "$BASE"
else
    cd "$WORK"
    git reset --hard "$BASE"
    git clean -fdx
fi
cd "$WORK"

for p in "$REPO"/patches/*/*.patch "$REPO"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "  apply ${p#"$REPO"/}"
    git apply --whitespace=nowarn "$p"
done

if [ "${INCLUDE_EXTRA:-0}" = "1" ]; then
    for p in "$REPO"/patches-extra/*.patch; do
        [ -e "$p" ] || continue
        echo "  apply ${p#"$REPO"/}"
        git apply --whitespace=nowarn "$p"
    done
fi

echo "== config: defconfig + frags =="
make defconfig O="$OUT" LLVM=1 >/dev/null
EXTRA_FRAGS=()
if [ "${INCLUDE_EXTRA:-0}" = "1" ] && compgen -G "$REPO/config/extra/*.configfrag" >/dev/null; then
    EXTRA_FRAGS=("$REPO"/config/extra/*.configfrag)
fi
scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" "$REPO"/config/*.configfrag "${EXTRA_FRAGS[@]}" >/dev/null
make -j"$JOBS" O="$OUT" olddefconfig LLVM=1 >/dev/null

# verify critical symbols land (both =y and =m)
MISSING=0
VERIFY_FRAGS=("$REPO/config/advan-x1-p1.configfrag" "${EXTRA_FRAGS[@]}")
while read -r line; do
    case "$line" in
        CONFIG_*=y|CONFIG_*=m)
            sym=${line%%=*}
            want=${line#*=}
            val=$(grep -E "^$sym=" "$OUT/.config" | head -1 | cut -d= -f2 || true)
            # If an extra fragment explicitly overrides a base symbol, honor the final frag.
            final=$(grep -hE "^$sym=" "${VERIFY_FRAGS[@]}" | tail -1 | cut -d= -f2 || true)
            [ -n "$final" ] && want="$final"
            if [ "$want" = "n" ]; then
                [ -z "$val" ] || { echo "FRAG MISS: $sym=n (got $val)"; MISSING=1; }
            else
                [ "$val" = "$want" ] || { echo "FRAG MISS: $sym=$want (got $val)"; MISSING=1; }
            fi
            ;;
    esac
done < <(grep -hE '^CONFIG_[A-Z0-9_]+=(y|m)' "${VERIFY_FRAGS[@]}" | grep -v 'CONFIG_ARCH_')

[ "$MISSING" = 0 ] || { echo "FATAL: fragment symbols missing"; exit 1; }
echo "config ok"

if [ "$MODE" = "check" ] || [ "$MODE" = "subsystem" ]; then
    echo "== Subsystem check: drivers/clk/mediatek/ (-j$JOBS, LLVM=1) =="
    make -j"$JOBS" O="$OUT" LLVM=1 drivers/clk/mediatek/
    echo "== Subsystem compile check OK =="
    exit 0
fi

echo "== Image (-j$JOBS, LLVM=1) =="
make -j"$JOBS" O="$OUT" LLVM=1 Image

echo "== dtb =="
PDTB="$OUT/advan_x1.dts.tmp"
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$WORK/include" -I "$WORK/include/dt-bindings" -I "$WORK/arch/arm64/boot/dts" \
    "$WORK/arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts" -o "$PDTB"
"$OUT/scripts/dtc/dtc" -I dts -O dtb -o "$OUT/mt6789-advan-x1.dtb" "$PDTB"
rm -f "$PDTB"
dtc -I dtb -O dts -o /dev/null "$OUT/mt6789-advan-x1.dtb" && echo "dtb roundtrip ok"

echo "== initramfs (static C /init + BusyBox rescue + kmods) =="
INITRD=${INITRD:-}
if [ -z "$INITRD" ] && [ "${USE_DIST_INITRD:-0}" != "1" ]; then
    INCLUDE_EXTRA="${INCLUDE_EXTRA:-0}" "$REPO/scripts/build-initramfs.sh"
    INITRD="$OUT/initramfs.cpio.gz"
elif [ -z "$INITRD" ] && [ -f "$REPO/dist/initramfs.cpio.gz" ]; then
    echo "WARNING: using stale dist/initramfs.cpio.gz (no static C /init)"
    INITRD="$REPO/dist/initramfs.cpio.gz"
fi
COMPRESSION=${COMPRESSION:-gzip}
BOOT_MODE=${BOOT_MODE:-auto}
MAX_SIZE=${MAX_SIZE:-32M}
PAYLOAD_LABEL=${PAYLOAD_LABEL:-linux_boot}
PYTHON_BIN=${PYTHON_BIN:-python3}
[ -x /work/.venv/bin/python ] && PYTHON_BIN=/work/.venv/bin/python

"$PYTHON_BIN" "$REPO/tools/pack_uboot_boot.py" \
    --kernel "$OUT/arch/arm64/boot/Image" \
    --dtb "$OUT/mt6789-advan-x1.dtb" \
    ${INITRD:+--initramfs "$INITRD"} \
    --compression "$COMPRESSION" \
    --mode "$BOOT_MODE" \
    --max-size "$MAX_SIZE" \
    --payload-label "$PAYLOAD_LABEL" \
    --make-fat-image \
    --out-dir "$OUT/uboot"

sha256sum "$OUT/arch/arm64/boot/Image" "$OUT/mt6789-advan-x1.dtb" \
    "$OUT/uboot/advan-x1.itb" "$OUT/uboot/boot.scr" "$OUT/uboot/uEnv.txt"
echo "== build done =="
