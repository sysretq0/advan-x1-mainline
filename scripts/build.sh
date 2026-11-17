#!/bin/bash
# Advan X1 port — clean build pipeline (repo redo).
# Pristine mainline checkout + patches/ series + config frags + DTs -> Image, DTB, LNXB bundle.
# Kernel tree: /work/mainline/linux (stays pristine; patches apply on worktree).
# Toolchain: native aarch64 clang 21 (LLVM=1).
# Resource limit: JOBS=1 to prevent OOM on 3.2 GB RAM host.
# Usage:
#   ./scripts/build.sh           -> Full local build (Image + DTB + bundle.img)
#   ./scripts/build.sh check     -> Fast local subsystem check (drivers/clk/mediatek/)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K=${K:-/work/mainline/linux}
WORK=${WORK:-/work/mainline/linux-worktree}
OUT=${OUT:-$REPO/build}
JOBS=${JOBS:-1}
BASE=${BASE:-master}
BRANCH=port-series
MODE=${1:-full}

export PATH="$REPO/scripts/bin:$PATH"
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
mkdir -p "$OUT"

cd "$K"
echo "== patch series on $BRANCH =="
if [ ! -d "$WORK/.git" ]; then
    git worktree remove --force "$WORK" 2>/dev/null || true
    rm -rf "$WORK"
    git worktree add --detach "$WORK" "$BASE"
else
    cd "$WORK"
    git reset --hard "$BASE"
    git clean -fdx
fi
cd "$WORK"

for p in "$REPO"/patches/*.patch; do
    [ -e "$p" ] || { echo "no patches"; break; }
    echo "  apply $(basename "$p")"
    git apply --whitespace=nowarn "$p"
done

echo "== config: defconfig + frags =="
make defconfig O="$OUT" LLVM=1 >/dev/null
scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" "$REPO"/config/*.configfrag >/dev/null
make -j"$JOBS" O="$OUT" olddefconfig LLVM=1 >/dev/null

# verify critical symbols land
MISSING=0
while read -r line; do
    case "$line" in CONFIG_*=y)
        sym=${line%%=*}
        val=$(grep -E "^$sym=" "$OUT/.config" | head -1 | cut -d= -f2 || true)
        [ "$val" = "y" ] || { echo "FRAG MISS: $line (got $val)"; MISSING=1; } ;;
    esac
done < <(grep -E '^CONFIG_[A-Z0-9_]+=y' "$REPO"/config/advan-x1-p1.configfrag | grep -v 'CONFIG_ARCH_')

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
PDTB=$(mktemp /tmp/advan_x1.XXXX.dts)
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$WORK/include" -I "$WORK/include/dt-bindings" -I "$WORK/arch/arm64/boot/dts" \
    "$REPO/dts/mt6789-advan-x1.dts" -o "$PDTB"
"$OUT/scripts/dtc/dtc" -I dts -O dtb -o "$OUT/mt6789-advan-x1.dtb" "$PDTB"
rm -f "$PDTB"
dtc -I dtb -O dts -o /dev/null "$OUT/mt6789-advan-x1.dtb" && echo "dtb roundtrip ok"

echo "== LNXB bundle =="
PACK_TOOL="${REPO}/tools/pack_linux_bundle.py"
if [ ! -f "$PACK_TOOL" ]; then
    PACK_TOOL="/work/bootloader/loki-loader/tools/pack_linux_bundle.py"
fi
INITRD=${INITRD:-}
python3 "$PACK_TOOL" \
    --kernel "$OUT/arch/arm64/boot/Image" --dtb "$OUT/mt6789-advan-x1.dtb" \
    ${INITRD:+--initramfs "$INITRD"} --out "$OUT/bundle.img"
sha256sum "$OUT/arch/arm64/boot/Image" "$OUT/mt6789-advan-x1.dtb" "$OUT/bundle.img"
echo "== build done =="
