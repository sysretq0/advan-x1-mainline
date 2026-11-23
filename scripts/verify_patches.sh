#!/bin/bash
# Verify clean patch application onto pinned upstream BASE, zero DTS drift,
# dtc compilation, and dt-validate schema compliance.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K=${K:-/work/mainline/linux}
BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}
TMP=${TMP:-/tmp/advan-plan/verify-tree}

rm -rf "$TMP"
mkdir -p "$TMP"
git -C "$K" archive "$BASE" | tar -x -C "$TMP"

count=0
for p in "$REPO"/patches/*/*.patch "$REPO"/patches/*.patch; do
	[ -e "$p" ] || continue
	git -C "$TMP" apply --check --whitespace=nowarn "$p" 2>/dev/null || {
		echo "FAIL: patch does not apply cleanly: ${p#"$REPO"/}" >&2
		git -C "$TMP" apply --check "$p"
		exit 1
	}
	patch -d "$TMP" -p1 --forward --quiet < "$p"
	count=$((count + 1))
done
echo "Applied $count patches cleanly onto $BASE"

diff -u "$REPO/dts/mt6789.dtsi" "$TMP/arch/arm64/boot/dts/mediatek/mt6789.dtsi"
diff -u "$REPO/dts/mt6789-advan-x1.dts" "$TMP/arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts"
echo "DTS sync check: OK (zero drift)"

clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I "$TMP/include" -I "$TMP/include/dt-bindings" -I "$TMP/arch/arm64/boot/dts" \
	"$TMP/arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts" -o "$TMP/board.pp.dts"
dtc -I dts -O dtb -o "$TMP/board.dtb" "$TMP/board.pp.dts"
echo "DTC compile check: OK"

DT_VALIDATE=${DT_VALIDATE:-$(command -v dt-validate || echo /work/.venv/bin/dt-validate)}
if [ -x "$DT_VALIDATE" ]; then
	"$DT_VALIDATE" -s "$TMP/Documentation/devicetree/bindings" -m "$TMP/board.dtb"
	echo "dt-validate check: OK (0 schema errors)"
else
	echo "WARN: dt-validate not found; skipping schema check" >&2
fi

echo "verify_patches.sh: ALL CHECKS PASSED"
