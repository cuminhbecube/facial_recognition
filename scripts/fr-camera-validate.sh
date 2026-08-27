#!/bin/sh
# Runtime evidence collector for the verified SC3336 Pico Pro Max firmware.
# It intentionally discovers V4L2 nodes instead of assuming /dev/video0.
set -eu

command -v media-ctl >/dev/null 2>&1 || {
    echo "ERROR: media-ctl is not installed" >&2
    exit 1
}
command -v v4l2-ctl >/dev/null 2>&1 || {
    echo "ERROR: v4l2-ctl is not installed" >&2
    exit 1
}

echo '=== media graph ==='
media-ctl -p
echo '=== V4L2 devices ==='
v4l2-ctl --list-devices
echo '=== SC3336 kernel messages ==='
dmesg | grep -i 'sc3336\|rkcif\|rkisp\|mipi' || true
