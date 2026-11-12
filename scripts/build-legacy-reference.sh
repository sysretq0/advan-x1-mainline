#!/bin/bash
# Advan X1 mainline exp2 build — kernel Image + device DTB, clang toolchain
# Native aarch64 host (no CROSS_COMPILE). Kernel: mainline 067c5c24 (7.4.0-rc2).
# Toolchain: /opt/llvm-r30 (Android clang 21, aarch64-linux-gnu), LLVM=1, ccache, -j3, no LTO.
set -euo pipefail
K=/re/workspaces/kernel/linux-mainline
D=/re/workspaces/devices/advan_x1/linux
OUT=$D/build
CLANG=/opt/llvm-r30/bin
JOBS=${JOBS:-6}
export PATH="$CLANG:$PATH"
mkdir -p "$OUT"

cd "$K"
echo "== config: defconfig + frag =="
make defconfig O="$OUT" >/dev/null
scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" \
    "$D/advan_x1-exp1.configfrag" "$D/advan_x1-exp2.configfrag" \
    "$D/advan_x1-exp3.configfrag" >/dev/null
(cd "$OUT" && make -j$JOBS olddefconfig LLVM=1 >/dev/null)
# Verify every fragment symbol actually took effect
MISSING=0
for frag in "$D/advan_x1-exp1.configfrag" "$D/advan_x1-exp2.configfrag" "$D/advan_x1-exp3.configfrag"; do
while read -r line; do
    case "$line" in CONFIG_*=y)
        sym=$(echo "$line" | cut -d= -f1)
        val=$(grep -E "^$sym=" "$OUT/.config" | head -1 | cut -d= -f2)
        if [ "$val" != "y" ]; then echo "FRAG MISS: $line (got $val)"; MISSING=1; fi ;;
    esac
done < <(grep -E '^CONFIG_[A-Z0-9_]+=y' "$frag")
done
[ "$MISSING" = 0 ] || { echo "FATAL: fragment symbols missing"; exit 1; }
grep -E "^CONFIG_(CC_IS_CLANG|LD_IS_LLD|LTO_NONE)=" "$OUT/.config" || true
echo "config ok"

echo "== build Image (native aarch64, -$JOBS, ccache, LLVM=1) =="
make -j$JOBS O="$OUT" LLVM=1 CC="ccache $CLANG/clang" Image

echo "== dtb: clang cpp + dtc standalone =="
PDTB=$(mktemp /tmp/opencode/advan_x1.XXXX.dts)
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$K/include" -I "$K/include/dt-bindings" \
    "$D/advan_x1-linux.dts" -o "$PDTB"
"$OUT/scripts/dtc/dtc" -I dts -O dtb -o "$OUT/advan_x1-linux.dtb" "$PDTB"
rm -f "$PDTB"
dtc -I dtb -O dts -o /dev/null "$OUT/advan_x1-linux.dtb" && echo "dtb roundtrip ok"

sha256sum "$OUT/arch/arm64/boot/Image" "$OUT/advan_x1-linux.dtb"
echo "== build done =="
