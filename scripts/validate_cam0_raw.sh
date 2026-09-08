#!/bin/sh
# Isolate the sensor-to-CIF path before bringing up AIQ, VI, or VENC.

set -u

NODE=/dev/video0
TMP=/tmp/dashcam/v4l2-command.out

run_probe()
{
	name=$1
	shift
	echo "[CAM0_RAW] step=$name command=$*"
	rm -f "$TMP"
	"$@" >"$TMP" 2>&1
	rc=$?
	cat "$TMP"
	echo "[CAM0_RAW] step=$name rc=$rc"
}

echo "[CAM0_RAW] state=START node=$NODE expected_sensor=OV8858 expected_mode=1632x1224@30"
if [ ! -c "$NODE" ]; then
	echo "[CAM0_RAW] state=ERROR_NODE_MISSING node=$NODE"
	exit 1
fi

if ! command -v v4l2-ctl >/dev/null 2>&1; then
	echo "[CAM0_RAW] state=ERROR_V4L2_CTL_MISSING"
	exit 1
fi

run_probe querycap v4l2-ctl -d "$NODE" --all
run_probe formats v4l2-ctl -d "$NODE" --list-formats-ext
run_probe stream timeout 15 v4l2-ctl -d "$NODE" --stream-mmap=2 --stream-count=30 --stream-to=/dev/null

echo "[CAM0_RAW] kernel_tail=BEGIN"
dmesg | grep -Ei 'ov8858|rkcif|mipi0|csi2|rkisp' | tail -n 120 || true
echo "[CAM0_RAW] kernel_tail=END"
echo "[CAM0_RAW] state=FINISHED"
