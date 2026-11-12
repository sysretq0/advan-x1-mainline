#!/bin/bash
# Advan X1 port — clean build pipeline (repo redo).
# Pristine mainline checkout + patches/ series + config frags + DTs -> Image, DTB, LNXB bundle.
# Kernel tree: /re/workspaces/kernel/linux-mainline (stays pristine; patches apply on branch).
# Toolchain: /opt/llvm-r30 (Android clang 21), native aarch64, LLVM=1, ccache, no LTO.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K=${K:-/re/workspaces/kernel/linux-mainline}
WORK=${WORK:-/tmp/opencode/mt6789-build-tree}
OUT="$REPO/build"
CLANG=/opt/llvm-r30/bin
JOBS=${JOBS:-6}
BASE=${BASE:-origin/master}
BRANCH=port-series

export PATH="$CLANG:$PATH"
mkdir -p "$OUT"

cd "$K"
echo "== patch series on $BRANCH =="
git worktree add --detach "$WORK" "$BASE" 2>/dev/null || true
cd "$WORK"
# reset worktree branch to base, then apply every patch in order
git checkout -B "$BRANCH" "$BASE" >/dev/null
for p in "$REPO"/patches/*.patch; do
    [ -e "$p" ] || { echo "no patches"; break; }
    echo "  apply $(basename "$p")"
    git apply --whitespace=nowarn "$p" || git apply --whitespace=nowarn --3way "$p"
done

echo "== config: defconfig + frags =="
make defconfig O="$OUT" >/dev/null
scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" "$REPO"/config/*.configfrag >/dev/null
(make -j"$JOBS" O="$OUT" olddefconfig LLVM=1 >/dev/null)
# every fragment symbol must actually land
MISSING=0
for frag in "$REPO"/config/*.configfrag; do
while read -r line; do
    case "$line" in CONFIG_*=y)
        sym=${line%%=*}
        val=$(grep -E "^$sym=" "$OUT/.config" | head -1 | cut -d= -f2)
        [ "$val" = "y" ] || { echo "FRAG MISS: $line (got $val)"; MISSING=1; } ;;
    esac
done < <(grep -E '^CONFIG_[A-Z0-9_]+=y' "$frag")
done
[ "$MISSING" = 0 ] || { echo "FATAL: fragment symbols missing"; exit 1; }
echo "config ok"

echo "== Image (-$JOBS, ccache, LLVM=1) =="
make -j"$JOBS" O="$OUT" LLVM=1 CC="ccache $CLANG/clang" Image

echo "== dtb =="
PDTB=$(mktemp /tmp/opencode/advan_x1.XXXX.dts)
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$WORK/include" -I "$WORK/include/dt-bindings" -I "$WORK/arch/arm64/boot/dts" \
    "$REPO/dts/mt6789-advan-x1.dts" -o "$PDTB"
"$OUT/scripts/dtc/dtc" -I dts -O dtb -o "$OUT/mt6789-advan-x1.dtb" "$PDTB"
rm -f "$PDTB"
dtc -I dtb -O dts -o /dev/null "$OUT/mt6789-advan-x1.dtb" && echo "dtb roundtrip ok"

echo "== LNXB bundle =="
INITRD=${INITRD:-}
python3 /re/workspaces/loki-loader/tools/pack_linux_bundle.py \
    --kernel "$OUT/arch/arm64/boot/Image" --dtb "$OUT/mt6789-advan-x1.dtb" \
    ${INITRD:+--initramfs "$INITRD"} --out "$OUT/bundle.img"
sha256sum "$OUT/arch/arm64/boot/Image" "$OUT/mt6789-advan-x1.dtb" "$OUT/bundle.img"
echo "== build done =="
