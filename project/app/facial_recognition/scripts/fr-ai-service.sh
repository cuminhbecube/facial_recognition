#!/bin/sh
# AI status and management helper for Facial Recognition

set -u

is_running() {
    if pgrep fr-media-service >/dev/null 2>&1; then
        return 0
    fi
    if [ -f /tmp/fr_ai_state.json ]; then
        now=$(date +%s 2>/dev/null || echo 0)
        ts=$(grep '"timestamp"' /tmp/fr_ai_state.json | awk -F: '{print $2}' | tr -d ' ,' || echo 0)
        if [ "$ts" -gt 0 ] && [ "$((now - ts))" -lt 10 ]; then
            return 0
        fi
    fi
    return 1
}

case "${1:-}" in
    start)
        /oem/usr/bin/fr-rtsp-service.sh start
        ;;
    stop)
        # Media service handles both
        ;;
    restart)
        /oem/usr/bin/fr-rtsp-service.sh restart
        ;;
    status)
        is_running
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}" >&2
        exit 2
        ;;
esac
