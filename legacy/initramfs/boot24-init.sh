#!/busybox sh
# Advan X1 boot24 — initramfs: no-devmem experiment.
# Boot 22 verdict: gadget fully bound (configfs probe returned 0,
# /dev/ttyGS0 existed) but no session on the wire — PC saw nothing.
# Boot 24 needs NO kernel rebuild: reuse kernel 860cc405 + DTB 04259939.
# Experiments (all sysfs, no /dev/mem):
#  A) log /sys/class/udc/*/state + current_speed — UDC state machine
#     truth: "not attached" = session never started.
#  B) UDC unbind/rebind cycle — re-trigger session/VBUS edge that may
#     have been missed because the cable was already in at boot.
B=/busybox
$B mkdir -p /proc /sys /dev /mnt /run /tmp /configfs
$B mount -t proc none /proc
$B mount -t sysfs none /sys
$B mount -t devtmpfs none /dev
echo "=== INITRAMFS v8: sysfs UDC diagnostics (no devmem) ==="

# --- configfs gadget (ACM serial) ---
if $B mount -t configfs none /configfs 2>/dev/null; then
    cd /configfs/usb_gadget
    $B mkdir g1 2>/dev/null
    cd /configfs/usb_gadget/g1
    echo 0x1d6b > idVendor
    echo 0x0104 > idProduct
    echo 0x0100 > bcdDevice
    $B mkdir -p strings/0x409
    echo "Advan" > strings/0x409/manufacturer
    echo "Advan X1 serial console" > strings/0x409/product
    $B mkdir -p configs/c.1/strings/0x409
    echo "ACM" > configs/c.1/strings/0x409/configuration
    echo 120 > configs/c.1/MaxPower
    $B mkdir -p functions/acm.usb0
    $B rm -f configs/c.1/acm.usb0 2>/dev/null
    $B ln -sf functions/acm.usb0 configs/c.1/
    UDC=$($B ls /sys/class/udc | $B head -n 1)
    echo "--- UDC: $UDC ---"
    if [ -n "$UDC" ]; then
        echo "$UDC" > UDC 2>/dev/null && echo "gadget enabled on $UDC" \
            || echo "gadget UDC bind FAILED"
    else
        echo "no /sys/class/udc entry - musb not probed"
    fi
else
    echo "configfs mount FAILED"
fi

# --- probe-mount root ---
ROOT=""
for i in 1 2 3 4 5 6 7 8 9 10; do
    for d in /dev/sd*[0-9] /dev/mmcblk*p[0-9]; do
        [ -b "$d" ] || continue
        if $B mount -t ext4 "$d" /mnt 2>/dev/null; then
            echo "=== ROOT MOUNTED: $d ==="
            ROOT=$d
            break 2
        fi
    done
    $B sleep 2
done
if [ -z "$ROOT" ]; then
    echo "=== NO ROOT FOUND ==="
    $B cat /proc/partitions
    $B dmesg | $B tail -n 100
    exec $B sh
fi

# --- evidence log onto the rootfs (survives power cut) ---
LOG=/mnt/var/log/initramfs.log
G1=/configfs/usb_gadget/g1
udc_state() { $B cat /sys/class/udc/*/state 2>&1; }
{
echo "=== initramfs v8 evidence $(date) ==="
echo "ROOT=$ROOT"
echo "--- UDC state AFTER first bind (A: session truth) ---"
echo "state: $(udc_state)"
echo "current_speed: $($B cat /sys/class/udc/*/current_speed 2>&1)"
echo "g1/UDC: $($B cat $G1/UDC 2>&1)"
echo "--- UDC unbind/rebind cycle (B: re-trigger VBUS edge) ---"
echo "" > $G1/UDC 2>/dev/null
echo "after unbind: state=$(udc_state)"
$B sleep 1
UDC=$($B ls /sys/class/udc | $B head -n 1)
echo "$UDC" > $G1/UDC 2>&1
$B sleep 2
echo "after rebind: state=$(udc_state)"
echo "current_speed: $($B cat /sys/class/udc/*/current_speed 2>&1)"
echo "--- sysfs udc dir ---"
$B ls -l /sys/class/udc 2>&1
echo "--- ttyGS0 node ---"
$B ls -l /dev/ttyGS0 2>&1
echo "--- dmesg musb/udc/gadget/phy ---"
$B dmesg | $B grep -iaE "musb|udc|gadget|tphy|u2port|vbus|usb" | $B tail -n 50
} > "$LOG" 2>&1
echo "--- evidence log: $LOG ---"

# --- fstab: replace unmatchable PARTUUID sources with device nodes ---
$B awk -v R="$ROOT" '{
    if ($2 == "/") $1 = R;
    else if ($2 == "/boot/efi") $1 = "#"$1;
    print
}' OFS="\t" /mnt/etc/fstab > /mnt/etc/fstab.new \
    && $B mv /mnt/etc/fstab.new /mnt/etc/fstab
echo "--- fstab ---"
$B cat /mnt/etc/fstab

# --- serial console: root shell without login (root pw is locked '*') ---
$B mkdir -p /mnt/etc/systemd/system/serial-getty@ttyGS0.service.d
$B cat > /mnt/etc/systemd/system/serial-getty@ttyGS0.service.d/override.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/bin/sh
EOF
echo "--- getty override installed ---"

echo "--- handing off to systemd ---"
exec $B switch_root /mnt /sbin/init
