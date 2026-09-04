#!/bin/sh
# Ethernet-only Media & RTSP & AI service for the SC3336 Facial Recognition product.

set -u

PID_FILE=/var/run/fr-rtsp.pid
LOG_FILE=/var/log/fr-rtsp.log
KO_DIR=/oem/usr/ko
MEDIA_BIN=/oem/usr/bin/fr-media-service
IQ_DIR=/etc/iqfiles
CONFIG_FILE=/oem/usr/etc/facial-recognition/rtsp.conf
MODEL_PATH=/oem/usr/share/facial-recognition/model/yolov5n-face-rv1106.rknn
DB_PATH=/userdata/facial-recognition/database.json
if [ ! -f "$DB_PATH" ] && [ -f /oem/usr/etc/facial-recognition/database.json ]; then
    mkdir -p /userdata/facial-recognition
    cp -f /oem/usr/etc/facial-recognition/database.json "$DB_PATH" 2>/dev/null || true
fi
if [ ! -f "$DB_PATH" ] && [ ! -d /userdata ]; then
    DB_PATH=/oem/usr/etc/facial-recognition/database.json
fi

WIDTH=1920
HEIGHT=1080
BITRATE_KBPS=1024
CODEC=h265
SEGMENT_SECONDS=180

if [ -r "$CONFIG_FILE" ]; then
    # shellcheck disable=SC1090
    . "$CONFIG_FILE"
fi

is_running() {
    [ -r "$PID_FILE" ] || return 1
    pid=$(cat "$PID_FILE" 2>/dev/null || true)
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null
}

start() {
    if is_running; then
        return 0
    fi
    rm -f "$PID_FILE"

    if [ ! -c /dev/vcodec ] || [ ! -d /dev/mpi ]; then
        "$KO_DIR/insmod_ko.sh" >>"$LOG_FILE" 2>&1 || return 1
    fi
    [ -x "$MEDIA_BIN" ] || return 1
    [ -d "$IQ_DIR" ] || return 1

    export LD_LIBRARY_PATH=/oem/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    "$MEDIA_BIN" -w "$WIDTH" -h "$HEIGHT" -a "$IQ_DIR" -e "$CODEC" -b "$BITRATE_KBPS" -T "$SEGMENT_SECONDS" -m "$MODEL_PATH" -d "$DB_PATH" >>"$LOG_FILE" 2>&1 &
    echo $! >"$PID_FILE"
}

stop() {
    if is_running; then
        kill "$(cat "$PID_FILE")" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
}

case "${1:-}" in
    start) start ;;
    stop) stop ;;
    restart) stop; sleep 1; start ;;
    status) is_running ;;
    *) echo "Usage: $0 {start|stop|restart|status}" >&2; exit 2 ;;
esac
