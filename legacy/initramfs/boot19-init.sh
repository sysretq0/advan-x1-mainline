#!/busybox sh
# Advan X1 boot19 — initramfs: USB ACM gadget + root handoff to systemd
# Same as boot18 (63247f00) except the fstab rewrite: boot18's sed used
# spaces and fstab separates fields with TABs, so both PARTUUID= sources
# survived and systemd dropped to emergency on unmatchable device paths.
# awk on fields, tab output, fixes it regardless of separator.
B=/busybox
$B mkdir -p /proc /sys /dev /mnt /run /tmp /configfs
$B mount -t proc none /proc
$B mount -t sysfs none /sys
$B mount -t devtmpfs none /dev
echo "=== INITRAMFS v5: ACM gadget + systemd handoff ==="

# --- configfs gadget (ACM serial) ---
if $B mount -t configfs none /configfs 2>/dev/null; then
    cd /configfs/usb_gadget
    $B mkdir g1 2>/dev/null
    cd /configfs/usb_gadget/g1
    echo 0x1d6b > idVendor    # Linux Foundation
    echo 0x0104 > idProduct   # multicomposite
    echo 0x0100 > bcdDevice
    $B mkdir -p strings/0x409
    echo "Advan" > strings/0x409/manufacturer
    echo "Advan X1 serial console" > strings/0x409/product
    $B mkdir -p configs/c.1/strings/0x409
    echo "ACM" > configs/c.1/strings/0x409/configuration
    echo 120 > configs/c.1/MaxPower
    $B mkdir -p functions/acm.usb0
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

# --- fstab: replace unmatchable PARTUUID sources with device nodes ---
# (GPT unique-GUID field is identical on every partition; PARTUUID= can
# never resolve on mainline. awk on fields: $1 device, $2 mountpoint.)
$B awk -v R="$ROOT" '{
    if ($2 == "/") $1 = R;
    else if ($2 == "/boot/efi") $1 = "#"$1;
    print
}' OFS="\t" /mnt/etc/fstab > /mnt/etc/fstab.new \
    && $B mv /mnt/etc/fstab.new /mnt/etc/fstab
echo "--- fstab ---"
$B cat /mnt/etc/fstab

# --- serial console: root shell without login (root pw is locked '*') ---
# console=ttyGS0 makes getty-generator spawn serial-getty@ttyGS0; this
# override swaps the login prompt for a raw shell.
$B mkdir -p /mnt/etc/systemd/system/serial-getty@ttyGS0.service.d
$B cat > /mnt/etc/systemd/system/serial-getty@ttyGS0.service.d/override.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/bin/sh
EOF
echo "--- getty override installed ---"

echo "--- handing off to systemd ---"
exec $B switch_root /mnt /sbin/init
