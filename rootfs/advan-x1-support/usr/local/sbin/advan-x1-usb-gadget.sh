#!/bin/sh
# Configure composite USB ConfigFS gadget (NCM usb0 + ACM ttyGS0) on Advan X1.
set -eu

mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
G=/sys/kernel/config/usb_gadget/g1

if [ -f "$G/UDC" ] && [ -n "$(cat "$G/UDC" 2>/dev/null || true)" ]; then
	exit 0
fi

mkdir -p "$G/strings/0x409" "$G/configs/c.1/strings/0x409"
echo 0x1d6b > "$G/idVendor"
echo 0x0104 > "$G/idProduct"
echo 0x0100 > "$G/bcdDevice"
echo 0x0200 > "$G/bcdUSB"
echo 0xEF   > "$G/bDeviceClass"
echo 0x02   > "$G/bDeviceSubClass"
echo 0x01   > "$G/bDeviceProtocol"
echo "advan-x1-001" > "$G/strings/0x409/serialnumber"
echo "Advan"        > "$G/strings/0x409/manufacturer"
echo "Advan X1"     > "$G/strings/0x409/product"
echo "NCM+ACM"      > "$G/configs/c.1/strings/0x409/configuration"
echo 250            > "$G/configs/c.1/MaxPower"

mkdir -p "$G/functions/ncm.usb0" "$G/functions/acm.usb0"
[ -L "$G/configs/c.1/ncm.usb0" ] || ln -s "$G/functions/ncm.usb0" "$G/configs/c.1/ncm.usb0"
[ -L "$G/configs/c.1/acm.usb0" ] || ln -s "$G/functions/acm.usb0" "$G/configs/c.1/acm.usb0"

UDC="$(ls /sys/class/udc 2>/dev/null | head -n 1 || true)"
if [ -n "$UDC" ]; then
	echo "$UDC" > "$G/UDC"
fi
