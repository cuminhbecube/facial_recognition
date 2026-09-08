#!/bin/sh
set -eu

sdk_root=${1:?SDK root is required}
source_image=${2:?Source update image is required}
version_file="$sdk_root/FIRMWARE_VERSION"
upgrade_dir="$sdk_root/Upgrade"

if [ ! -f "$source_image" ]; then
    echo "[export-upgrade:error] update.img does not exist: $source_image" >&2
    exit 1
fi

version=${RV06_FIRMWARE_VERSION:-}
if [ -z "$version" ]; then
    if [ ! -f "$version_file" ]; then
        echo "[export-upgrade:error] Missing $version_file" >&2
        exit 1
    fi
    version=$(sed -n '1{s/[[:space:]]//g;p;}' "$version_file")
fi

case "$version" in
    ''|*[!A-Za-z0-9._-]*)
        echo "[export-upgrade:error] Invalid firmware version: $version" >&2
        exit 1
        ;;
esac

mkdir -p "$upgrade_dir"
if [ "${RV06_FACTORY_LAYOUT_FLASH:-0}" = 1 ]; then
    destination="$upgrade_dir/factory-$version-layout-v2.img"
else
    destination="$upgrade_dir/upgrade-$version.img"
fi
temporary="$destination.new"
cp -f "$source_image" "$temporary"
sync "$temporary"
mv -f "$temporary" "$destination"

echo "[export-upgrade] version=$version image=$destination"
sha256sum "$destination"
