#!/busybox sh
# Advan X1 boot23 — initramfs: USB ACM gadget + infra gate poke + reg dump
# Boot 22 verdict: gadget fully bound (configfs-gadget probe returned 0,
# /dev/ttyGS0 existed) but no session on the wire — PC saw nothing.
# Suspect: infra_ao clock gates closed (u-boot fork proved ACM only with
# its own clock driver) and/or tphy port config. v7 force-sets the
# IFRAO_SSUSB gate and dumps musb + u2port0 registers.
# busybox-static has no devmem applet: dd on /dev/mem instead.
B=/busybox
$B mkdir -p /proc /sys /dev /mnt /run /tmp /configfs
$B mount -t proc none /proc
$B mount -t sysfs none /sys
$B mount -t devtmpfs none /dev

# MMIO via busybox devmem (kernel: CONFIG_STRICT_DEVMEM=n)
rdreg() { devmem "$1"; }
wrreg() { devmem "$1" 32 "$2"; }

echo "=== INITRAMFS v11: observation only — NO clock pokes ==="
# Boot-24 loader probe verdict: musb MAC registers are LIVE at loader entry
# (0x11200000=0x2000, +4=0x01ff0000, +8=0x060001fe, +0x60=0xf80) with the
# ifrao CG2 SSUSB gate bit1 CLOSED (sta=0x100) and USBPLL EN=0. Mainline
# saw the MAC all-zero with gate bit1 OPEN. So the all-zero state is
# created between loader exit and initramfs entry. v11 changes NOTHING:
# every register is read BEFORE any write, to catch the state the kernel
# handed to init and separate "kernel opened gates + killed MAC" from
# "MAC died with gates untouched".

echo "--- ifrao CG2 sta 0x100010AC (NO poke) : $(rdreg 0x100010AC) ---"
echo "--- USBPLL CON0/1/2 (NO poke)          : $(rdreg 0x1000C3C4) $(rdreg 0x1000C3C8) $(rdreg 0x1000C3CC) ---"
echo "--- USB_TOP_SEL mux 0x100000A0 (NO poke): $(rdreg 0x100000A0) ---"
echo "--- topck UPDATE1 0x10000008           : $(rdreg 0x10000008) ---"
echo "--- SPM PWR_STATUS/2ND 0x1000616C/0x170: $(rdreg 0x1000616C) $(rdreg 0x10006170) ---"
echo "--- SPM PERI_PWR 0x100063D0            : $(rdreg 0x100063D0) ---"
echo "--- musb word @0x0c DEVCTL (NO poke)   : $(rdreg 0x1120000c) ---"
echo "--- musb word @0x00 (NO poke)          : $(rdreg 0x11200000) ---"
echo "--- tphy DTM1 0x11f4036c (entry)       : $(rdreg 0x11f4036c) ---"

# --- v13 FIX: force B-session in the tphy (uboot-g99 fork phy-mtk-tphy.c,
# u2_phy_instance_power_on, full end-state) ---
# Boot-26 verdict: DTM1=0x370B (missing RG_VBUSVALID/RG_AVALID) left DEVCTL
# VBUS=00 -> musb_start set SESSION -> HW FSM auto-host again (0x07).
# Fork's power_on ends at DTM1 bits 0,1,2,3,5,8,9,10,12,13 = 0x372F:
# RG_IDPULLUP(0)|RG_IDDIG(1)|RG_AVALID(2)|RG_BVALID(3)|RG_VBUSVALID(5)|
# FORCE_IDPULLUP(8)|FORCE_IDDIG(9)|FORCE_AVALID(10)|FORCE_SESSEND(12)|
# FORCE_VBUSVALID(13), RG_SESSEND(4)=0. With BVALID+VBUSVALID forced,
# DEVCTL.VBUS reads 11 and musb_start (mainline musb_core.c) takes the
# is_active branch and NEVER sets SESSION - the exact uboot chain flow
# (USB_ISSUES.md: "musb_start() <== devctl 98", gadget enumerates by D+
# pullup alone).
# 1) drop any session claim so the FSM re-samples:
wrreg 0x1120000c 0x00000000
# 2) apply the fork force end-state:
wrreg 0x11f4036c 0x372F
$B sleep 1
echo "--- after B-session force: DTM1=$(rdreg 0x11f4036c) DEVCTLword=$(rdreg 0x1120000c) ---"

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
{
echo "=== initramfs v13 evidence (B-session force 0x372F) $(date) ==="
echo "ROOT=$ROOT"
echo "--- after B-session force: DTM1 0x11f4036c ---"
rdreg 0x11f4036c
echo "--- musb DEVCTL word @0x0c after force ---"
rdreg 0x1120000c
echo "--- ifrao CG2 sta 0x100010AC (entry state, no poke) ---"
rdreg 0x100010AC
echo "--- USBPLL CON0 0x1000C3C4 / CON1 0x3C8 / CON2 0x3CC (entry) ---"
rdreg 0x1000C3C4
rdreg 0x1000C3C8
rdreg 0x1000C3CC
echo "--- USB_TOP_SEL mux 0x100000A0 + UPDATE1 0x10000008 (entry) ---"
rdreg 0x100000A0
rdreg 0x10000008
echo "--- SPM PWR_STATUS 0x1000616C / 2ND 0x10006170 / PERI_PWR 0x100063D0 (entry) ---"
rdreg 0x1000616C
rdreg 0x10006170
rdreg 0x100063D0
echo "--- musb regs 0x11200000 (first 0x60, entry state) ---"
for o in 0 4 8 12 16 20 24 28 32 36 40 44 48 52 56 60 64 68 72 76 80 84 88 92; do
    echo "0x11200000+$(printf 0x%02x $o): $(rdreg $(( 0x11200000 + o )))"
done
echo "--- musb wide scan 0x11200000..0x2000 stride 0x20 (nonzero only) ---"
for o in $(seq 0 32 8192); do
    v=$(rdreg $(( 0x11200000 + o )))
    [ "$v" != "0x00000000" ] && echo "0x11200000+$(printf 0x%04x $o): $v"
done
echo "--- musb scan done ---"
echo "--- tphy u2port0 regs 0x11f40000 (first 0xb0) ---"
for o in 0 4 8 12 16 20 24 28 32 36 40 44 48 52 56 60 64 68 72 76 80 84 88 92 96 100 104 108 112 116 120 124 128 132 136 140 144 148 152 156 160 164 168 172; do
    echo "0x11f40000+$(printf 0x%02x $o): $(rdreg $(( 0x11f40000 + o )))"
done
echo "--- tphy u2phy com bank 0x11f40300 (PHY control regs; u-boot-g99 USB_ISSUES.md: control at 0x300+, 0x318/0x31c/0x320 ctrl, 0x368 analog, 0x36c calibration) ---"
for o in 0 4 8 12 16 20 24 28 32 36 40 44 48 52 56 60 64 68 72 76 80 84 88 92 96 100 104 108 112 116 120 124; do
    echo "0x11f40300+$(printf 0x%02x $o): $(rdreg $(( 0x11f40300 + o )))"
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
echo "--- UDC rebind after B-session force ---"
G1=/configfs/usb_gadget/g1
echo "" > $G1/UDC 2>/dev/null
$B sleep 1
UDC=$($B ls /sys/class/udc | $B head -n 1)
echo "$UDC" > $G1/UDC 2>&1
$B sleep 2
echo "after rebind: g1/UDC=$($B cat $G1/UDC 2>&1)"
echo "word @0x0c (DEVCTL bits23:16): $(rdreg 0x1120000c)"
echo "tphy DTM1 0x11f4036c: $(rdreg 0x11f4036c)"
echo "ifrao CG2 sta after bind: $(rdreg 0x100010AC)"
echo "udc state: $($B cat /sys/class/udc/*/state 2>&1)"
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
