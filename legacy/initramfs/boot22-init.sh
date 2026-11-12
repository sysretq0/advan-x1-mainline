#!/busybox sh
# Advan X1 boot22 — initramfs: USB ACM gadget + root handoff to systemd
# Same as boot19 (3bd6c15c) plus: writes its own evidence log onto the
# rootfs (/mnt/var/log/initramfs.log) BEFORE switch_root, so musb/UDC
# bind results survive the power-cut that eats journald's tail and the
# ramoops overwrite from the stock boot.
B=/busybox
$B mkdir -p /proc /sys /dev /mnt /run /tmp /configfs
$B mount -t proc none /proc
$B mount -t sysfs none /sys
$B mount -t devtmpfs none /dev
echo "=== INITRAMFS v7: ACM gadget + gate poke + reg dump ==="

# --- USB clock gate force-on (boot 22: gadget bound but no session on
# the wire; u-boot fork proved ACM only after enabling infra gates.
# IFRAO_SSUSB = infra_ao2 gate bit 1: set 0x100010A4, sta 0x100010AC.) ---
echo "--- infra gate sta before: $(devmem 0x100010AC) ---"
devmem 0x100010A4 32 0x2          # set register: bit 1 SSUSB
echo "--- infra gate sta after:  $(devmem 0x100010AC) ---"

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
    for d in /dev/sdc72 /dev/sd*[0-9] /dev/mmcblk*p[0-9]; do
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
{
echo "=== initramfs v7 evidence $(date) ==="
echo "ROOT=$ROOT"
echo "--- infra_ao2 gate sta 0x100010AC ---"
devmem 0x100010AC
echo "--- musb regs 0x11200000 (first 0x60) ---"
for o in 0x0 0x4 0x8 0xc 0x10 0x14 0x18 0x1c 0x20 0x24 0x28 0x2c 0x30 0x34 0x38 0x3c 0x40 0x44 0x48 0x4c 0x50 0x54 0x58 0x5c; do
    echo "0x11200000+$o: $(devmem $((0x11200000 + o)) 2>/dev/null)"
done
echo "--- tphy u2port0 regs 0x11f40000 (first 0x100) ---"
for o in 0x0 0x4 0x8 0xc 0x10 0x14 0x18 0x1c 0x20 0x24 0x28 0x2c 0x30 0x34 0x38 0x3c 0x40 0x44 0x48 0x4c 0x50 0x54 0x58 0x5c 0x60 0x64 0x68 0x6c 0x70 0x74 0x78 0x7c 0x80 0x84 0x88 0x8c 0x90 0x94 0x98 0x9c 0xa0 0xa4 0xa8 0xac; do
    echo "0x11f40000+$o: $(devmem $((0x11f40000 + o)) 2>/dev/null)"
done
echo "--- /sys/class/udc ---"
$B ls -l /sys/class/udc 2>&1
echo "--- g1/UDC content ---"
$B cat /configfs/usb_gadget/g1/UDC 2>&1
echo "--- g1/configs ---"
$B ls -l /configfs/usb_gadget/g1/configs/c.1 2>&1
echo "--- ttyGS0 node ---"
$B ls -l /dev/ttyGS0 /sys/class/tty/ttyGS0 2>&1
echo "--- dmesg musb/udc/gadget/phy ---"
$B dmesg | $B grep -iaE "musb|udc|gadget|tphy|u2port|11f40000|usb" | $B tail -n 40
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
