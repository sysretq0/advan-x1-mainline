#!/bin/sh
echo "=== Input Devices ==="
cat /proc/bus/input/devices | grep -E "^(N:|H:|P:)"
echo "=== Touch IRQ ==="
grep -iE "spi0\.0|icnl|eint.*9\b" /proc/interrupts || true
echo "=== libinput list-devices (Touchscreen) ==="
libinput list-devices 2>/dev/null | grep -A 18 -i "Chipone" || true
echo "=== DRM & Panfrost ==="
ls -la /dev/dri/
echo "=== GDM / GNOME Shell status ==="
systemctl status gdm --no-pager -n 10 || true
