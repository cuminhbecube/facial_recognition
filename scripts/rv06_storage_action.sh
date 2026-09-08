#!/bin/sh
set -eu

DEVICE=/dev/mmcblk1p1
PARENT=/dev/mmcblk1
MOUNT_PATH=/mnt/sdcard
MOUNT_OPTIONS=rw,noatime,uid=1000,gid=1000,fmask=0133,dmask=0022
REQUEST=/tmp/dashcam/storage-maintenance.request
LOCK_DIR=/tmp/dashcam/storage-maintenance.lock
LOG=/tmp/dashcam/storage-maintenance.log

log()
{
	echo "[STORAGE_WEB] $*" >>"$LOG"
}

cleanup()
{
	if verify_device && ! awk -v path="$MOUNT_PATH" '$2 == path { found=1 } END { exit !found }' /proc/mounts; then
		mkdir -p "$MOUNT_PATH"
		mount -t vfat -o "$MOUNT_OPTIONS" "$DEVICE" "$MOUNT_PATH" 2>/dev/null || true
	fi
	rm -f "$REQUEST"
	rmdir "$LOCK_DIR" 2>/dev/null || true
}

verify_device()
{
	[ -b "$PARENT" ] && [ -b "$DEVICE" ] || return 1
	[ "$(cat /sys/class/block/mmcblk1/device/type 2>/dev/null)" = SD ] || return 1
	case "$(readlink -f /sys/class/block/mmcblk1p1 2>/dev/null)" in
		*/mmcblk1/mmcblk1p1) ;;
		*) return 1 ;;
	esac
}

action=${1:-}
case "$action" in repair|format) ;; *) exit 2 ;; esac
mkdir -p /tmp/dashcam
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
	log "state=REFUSED reason=BUSY action=$action"
	exit 3
fi
trap cleanup EXIT INT TERM

if ! verify_device; then
	log "state=REFUSED reason=DEVICE_NOT_VERIFIED action=$action"
	exit 4
fi

: >"$REQUEST"
log "state=START action=$action device=$DEVICE"
camera_pid=$(pidof rv06_dashcam 2>/dev/null || true)
if [ -n "$camera_pid" ]; then
	kill -TERM $camera_pid 2>/dev/null || true
	waited=0
	while pidof rv06_dashcam >/dev/null 2>&1 && [ "$waited" -lt 15 ]; do
		sleep 1
		waited=$((waited + 1))
	done
	if pidof rv06_dashcam >/dev/null 2>&1; then
		log "state=FAILED stage=STOP_RECORDER action=$action"
		exit 5
	fi
fi

sync
mounted=$(awk -v path="$MOUNT_PATH" '$2 == path { print $1; exit }' /proc/mounts)
if [ -n "$mounted" ]; then
	if [ "$mounted" != "$DEVICE" ]; then
		log "state=REFUSED reason=WRONG_MOUNT source=$mounted action=$action"
		exit 6
	fi
	if ! umount "$MOUNT_PATH"; then
		log "state=FAILED stage=UNMOUNT action=$action"
		exit 7
	fi
fi

if [ "$action" = repair ]; then
	log "state=RUNNING stage=FSCK device=$DEVICE"
	fsck.vfat -a "$DEVICE" >>"$LOG" 2>&1 || true
else
	log "state=RUNNING stage=FORMAT device=$DEVICE fs=fat32"
	if ! mkfs.vfat -F 32 -n RV06_DASHCAM "$DEVICE" >>"$LOG" 2>&1; then
		log "state=FAILED stage=FORMAT action=$action"
		exit 8
	fi
fi

mkdir -p "$MOUNT_PATH"
if ! mount -t vfat -o "$MOUNT_OPTIONS" "$DEVICE" "$MOUNT_PATH"; then
	log "state=FAILED stage=MOUNT action=$action"
	exit 9
fi
mkdir -p "$MOUNT_PATH/DCIM/front" "$MOUNT_PATH/DCIM/rear" \
	"$MOUNT_PATH/logs/system" "$MOUNT_PATH/logs/gps"
sync
log "state=DONE action=$action device=$DEVICE mount=$MOUNT_PATH"
