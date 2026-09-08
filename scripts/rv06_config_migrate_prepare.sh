#!/bin/sh

SOURCE=/run/rv06-config
[ -d "$SOURCE" ] || SOURCE=/oem/usr/etc/dashcam
MOUNT=/mnt/sdcard
TARGET=$MOUNT/.rv06-config-migration
STAGE=$MOUNT/.rv06-config-migration.new

log() { echo "[CONFIG_MIGRATION] $*"; }

[ -d "$SOURCE" ] || {
	log "state=ERROR reason=SOURCE_NOT_FOUND"
	exit 1
}
[ "$(cat /sys/class/block/mmcblk1/device/type 2>/dev/null)" = SD ] || {
	log "state=ERROR reason=SD_NOT_VERIFIED"
	exit 1
}
set -- $(awk '$2 == "/mnt/sdcard" { print $1, $4; exit }' /proc/mounts)
[ "$1" = /dev/mmcblk1p1 ] || {
	log "state=ERROR reason=SD_NOT_MOUNTED_FROM_EXPECTED_DEVICE"
	exit 1
}
case ",$2," in *,rw,*) ;; *) log "state=ERROR reason=SD_READ_ONLY"; exit 1 ;; esac

rm -rf "$STAGE" || exit 1
mkdir -p "$STAGE" || exit 1
for name in runtime.conf runtime.conf.backup jt808.conf jt808.conf.backup \
	admin.password.hash admin.password.hash.backup wifi-ap.conf wifi-ap.conf.backup \
	wifi-ap.psk wifi-ap.psk.backup; do
	if [ -f "$SOURCE/$name" ]; then
		cp -f "$SOURCE/$name" "$STAGE/$name" || { rm -rf "$STAGE"; exit 1; }
	fi
done
( [ -f "$STAGE/runtime.conf" ] && [ -f "$STAGE/jt808.conf" ] &&
  [ -f "$STAGE/admin.password.hash" ] ) || {
	rm -rf "$STAGE"
	log "state=ERROR reason=REQUIRED_CONFIG_MISSING"
	exit 1
}
(cd "$STAGE" && sha256sum ./* > manifest.sha256) || exit 1
printf 'schema_version=1\n' > "$STAGE/migration.complete"
sync
rm -rf "$TARGET"
mv "$STAGE" "$TARGET"
sync
log "state=READY source=$SOURCE target=$TARGET files=$(find "$TARGET" -type f | wc -l)"
