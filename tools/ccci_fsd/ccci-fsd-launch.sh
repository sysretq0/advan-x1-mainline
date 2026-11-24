#!/bin/bash
# Kernel-enforced read-only NVRAM + RAM tmpfs overlayfs launcher for ccci_fsd.
#
# Safety contract (fail-closed):
#   1. Resolves nvram, nvdata, nvcfg, protect1, protect2 strictly via
#      /dev/disk/by-partlabel/<name> (no hardcoded /dev/sdc* paths).
#   2. Marks ALL 5 UFS block devices read-only via `blockdev --setro` and
#      verifies `blockdev --getro == 1`.
#   3. Mounts ext4 partitions strictly `-o ro,noload,nosuid,nodev,noexec`.
#   4. Layers RAM tmpfs overlayfs upperdirs on top for Z:/X:/Y:/V:/S:, pure
#      RAM tmpfs for U:/Q:, and empty mode-0555 dirs for W:/T:/R:.
#   5. Only if every safety gate succeeds, execs ccci_fsd which opens
#      /dev/ccci_fs and then triggers /sys/bus/platform/devices/d0cf000.modem/boot.
set -euo pipefail

RUN_ROOT=/run/ccci_fs
LOWER_ROOT="$RUN_ROOT/.lower"
WORK_ROOT="$RUN_ROOT/.overlay"
BOOT_TRIGGER=/sys/bus/platform/devices/d0cf000.modem/boot
CCCI_FSD_BIN="${CCCI_FSD_BIN:-/usr/local/sbin/ccci_fsd}"

log() {
	echo "[ccci-fsd-launch] $*"
}

die() {
	echo "[ccci-fsd-launch] FATAL: $*" >&2
	echo "[ccci-fsd-launch] Failing closed — modem boot NOT triggered." >&2
	exit 1
}

lock_ro_part() {
	local label="$1"
	local dev="/dev/disk/by-partlabel/$label"
	local real_dev ro_val

	[ -b "$dev" ] || die "missing partition $dev"
	real_dev="$(readlink -f "$dev")"
	blockdev --setro "$real_dev" || die "blockdev --setro failed on $label ($real_dev)"
	ro_val="$(blockdev --getro "$real_dev")"
	[ "$ro_val" = "1" ] || die "blockdev --getro != 1 on $label ($real_dev, got '$ro_val')"
	log "locked RO: $label -> $real_dev (getro=$ro_val)"
}

mount_ro_ext4() {
	local label="$1"
	local mnt="$LOWER_ROOT/$label"
	local dev="/dev/disk/by-partlabel/$label"

	mkdir -p "$mnt"
	if mountpoint -q "$mnt"; then
		umount "$mnt" || die "failed to unmount stale $mnt"
	fi
	mount -t ext4 -o ro,noload,nosuid,nodev,noexec "$dev" "$mnt" \
		|| die "read-only ext4 mount failed for $label ($dev)"
	log "mounted RO ext4: $label -> $mnt"
}

setup_overlay() {
	local drive="$1"
	local lower_sub="$2"
	local target="$RUN_ROOT/$drive"
	local upper="$WORK_ROOT/$drive/upper"
	local work="$WORK_ROOT/$drive/work"

	[ -d "$lower_sub" ] || die "missing lower directory $lower_sub for drive $drive:"
	mkdir -p "$target" "$upper" "$work"
	if mountpoint -q "$target"; then
		umount "$target" || die "failed to unmount stale overlay $target"
	fi
	mount -t overlay overlay \
		-o "lowerdir=$lower_sub,upperdir=$upper,workdir=$work" \
		"$target" \
		|| die "overlayfs mount failed for drive $drive: ($lower_sub)"
	log "overlay ready: $drive: -> $target (lower=$lower_sub, upper=tmpfs)"
}

[ -c /dev/ccci_fs ] || die "/dev/ccci_fs not present"
[ -x "$CCCI_FSD_BIN" ] || die "$CCCI_FSD_BIN not executable"
[ -w "$BOOT_TRIGGER" ] || die "modem boot trigger $BOOT_TRIGGER not writable"

# 1. Mark ALL 5 NVRAM-related block devices read-only at kernel block layer
for part in nvram nvdata nvcfg protect1 protect2; do
	lock_ro_part "$part"
done

# 2. Fresh RAM tmpfs workspace
mkdir -p "$RUN_ROOT"
if ! mountpoint -q "$RUN_ROOT"; then
	mount -t tmpfs -o mode=0755,size=32M tmpfs "$RUN_ROOT" \
		|| die "tmpfs mount failed at $RUN_ROOT"
fi
mkdir -p "$LOWER_ROOT" "$WORK_ROOT"

# 3. Mount the 4 ext4 NVRAM partitions strictly read-only (raw nvram stays unmounted & RO)
for part in nvdata nvcfg protect1 protect2; do
	mount_ro_ext4 "$part"
done

# 4. Wire all 10 Advan X1 drives (Z/X/Y/V/S overlays, U/Q tmpfs, W/T/R empty RO)
setup_overlay Z "$LOWER_ROOT/nvdata/md"
setup_overlay X "$LOWER_ROOT/protect1/md"
setup_overlay Y "$LOWER_ROOT/protect2/md"
setup_overlay V "$LOWER_ROOT/nvdata/md_cmn"
setup_overlay S "$LOWER_ROOT/nvcfg"

mkdir -p "$RUN_ROOT/U" "$RUN_ROOT/Q"
mkdir -p "$RUN_ROOT/W" "$RUN_ROOT/T" "$RUN_ROOT/R"
chmod 0555 "$RUN_ROOT/W" "$RUN_ROOT/T" "$RUN_ROOT/R"

log "all 10 drives ready (5 UFS partitions locked RO); starting ccci_fsd"
exec "$CCCI_FSD_BIN" \
	--dev /dev/ccci_fs \
	--root "$RUN_ROOT" \
	--boot-trigger "$BOOT_TRIGGER"
