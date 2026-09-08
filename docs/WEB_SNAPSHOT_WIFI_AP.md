# RV06 Web, Snapshot and Wi-Fi AP

## Runtime mapping

| Function | CAM0 | CAM1 |
| --- | --- | --- |
| Main H.265 | VENC 0 | VENC 1 |
| Sub H.264 | VPSS/RGA 0 -> VENC 2 | VPSS/RGA 1 -> VENC 3 |
| JPEG one-shot | VENC 4 on demand | VENC 5 on demand |

JPEG channels are created only for an active request and destroyed after the
encoded buffer is copied. This avoids reserving CMA while the preview is idle.
Main packets are delivered to the recorder before any RTSP consumer.

## Web

The C++ server listens on HTTP port 80 only when
`/oem/usr/etc/dashcam/admin.password.hash` contains a provisioned `crypt(3)`
hash. The production image does not contain a shared default password.

Implemented endpoints:

```text
POST /api/v1/auth/login
GET  /api/v1/status
GET  /api/v1/config
PUT  /api/v1/config
GET  /api/v1/cameras/cam0/snapshot?quality=1..100
GET  /api/v1/cameras/cam1/snapshot?quality=1..100
```

Sessions use an HttpOnly, SameSite=Strict cookie. Login returns a CSRF token;
all future state-changing endpoints must require it. Requests and sockets have
fixed size/time limits. Audit records are appended to
`/mnt/sdcard/logs/web-audit.log` without credentials.

Configuration is parsed with the SDK cJSON implementation and range-checked
before it is persisted. Updates use `config.json.new`, `fsync`, rename, and a
`config.json.backup`; they never reboot or restart a camera automatically. The
mobile page exposes CAM0 sub-stream FPS/bitrate and JPEG quality. Passwords are
kept in separate mode-0600 provisioning files, not in `config.json`.

Validated camera, main/sub encoder, segment, storage, Web port, and snapshot
settings are loaded into typed runtime configuration on the next dashcam service
start. Hardware probe status still overrides configuration: loss of PR2100K
VIN1/VC1 keeps CAM1 unavailable without stopping a proven CAM0 path. The 4 GB
card profile reserves 512 MB instead of 2 GB.

Snapshot requests are limited to one per second per camera and time out after
three seconds. The current LubanCat RGN implementation rejects attachment to an
idle JPEG VENC channel, so JPEG OSD is currently reported as unsupported rather
than simulated.

## Wi-Fi AP

The detected adapter is AIC8800 (`aic8800_fdrv`) and advertises AP mode through
`iw list`. `S70rv06-wifi-ap` verifies this again at every boot and fails without
affecting the dashcam when the capability or provisioning is missing.
It waits up to 30 seconds for the asynchronously loaded AIC8800 driver, while
`dhcpcd` is explicitly prevented from managing the AP-owned `wlan0` interface.
Hostapd and dnsmasq failures are mirrored to the UART boot log.

The AP uses:

```text
SSID: DASHCAM-<last 6 Wi-Fi MAC characters>
IP: 192.168.50.1/24
DHCP: 192.168.50.10-192.168.50.100
WPA2-PSK/CCMP, client isolation, maximum 4 clients
```

`wifi-ap.psk` contains the unique 64-hex WPA PSK, not the clear passphrase.
Internet forwarding from the Wi-Fi AP to the cellular interface is disabled.

The confirmed deployment passphrase is `88888888`. The packaged file contains
only its SSID-specific derived PSK and is installed with mode 0600.

The confirmed debug Web administrator password is `86868686`. Only its
`crypt(3)` hash is packaged, with mode 0600.
