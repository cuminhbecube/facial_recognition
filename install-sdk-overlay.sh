#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sdk_root=${1:-}

if [ -z "$sdk_root" ] || [ ! -f "$sdk_root/build.sh" ] || \
   [ ! -f "$sdk_root/project/build.sh" ]; then
    echo "Usage: $0 /path/to/RV06_03_Linux_SDK" >&2
    exit 1
fi

cp -a "$repo_dir/sdk-overlay/." "$sdk_root/"
cp -f "$repo_dir/FIRMWARE_VERSION" "$sdk_root/FIRMWARE_VERSION"

if ! grep -q 'scripts/export_upgrade.sh' "$sdk_root/project/build.sh"; then
    patch -d "$sdk_root" -p1 < "$repo_dir/integration/project-build.patch"
fi

echo "Installed RV06 dashcam SDK overlay into $sdk_root"
