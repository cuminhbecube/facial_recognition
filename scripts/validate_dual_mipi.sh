#!/bin/sh
set -eu

echo '=== sensor probe ==='
dmesg | grep -Ei 'ov8858|gc2053|rkcif|rkisp|mipi' || true

echo '=== media graph ==='
media-ctl -d /dev/media0 -p
media-ctl -d /dev/media1 -p

echo '=== V4L2 devices ==='
v4l2-ctl --list-devices

echo '=== SD card ==='
mount | grep ' /mnt/sdcard ' || exit 1
test -w /mnt/sdcard || exit 1

echo 'PASS: inspect both media graphs and recorder log before declaring camera success.'
