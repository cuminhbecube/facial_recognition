# BECAM-2 Web Config

## Scope

Firmware 0.6.0 provides configuration and status Web UI without Web video. Live
view remains available only through the four RTSP URLs. Snapshot, MJPEG, HLS,
WebRTC, and browser video adapters are not present.

## Runtime

```text
/oem/usr/bin/rv06_webconfig
/oem/usr/share/dashcam/www
/run/rv06-config/admin.password.hash
/run/rv06-config/runtime.conf
/mnt/sdcard/logs/web-audit.log
```

The server binds `0.0.0.0:80`, allowing access from Ethernet and the verified
Wi-Fi AP at `192.168.50.1`. It is launched independently before the camera app;
a bind, authentication, or frontend failure cannot stop recording.

## Pages

| Route | Purpose |
| --- | --- |
| `/` | Device, camera, recorder, GPS, network, and SD status |
| `/cameras` | Validated CAM0/CAM1 and main/sub encoder configuration |
| `/recording` | Recorder and storage policy |
| `/osd-gps` | OSD visibility and generic GNSS status; GNSS acquisition remains enabled |
| `/stream` | Four RTSP paths and bounded queue configuration |
| `/network` | Ethernet and Wi-Fi AP configuration |
| `/storage` | Verified SD device and filesystem policy |
| `/system` | Firmware and Web service limits |
| `/maintenance` | Service state, self-test, and diagnostics export |
| `/diagnostics` | Bounded recorder log tail |
| `/security` | Session information and logout |

Each route loads a separate HTML document. Shared CSS and JavaScript total less
than 31 KiB and require no external asset.

## API

```text
POST /api/v1/auth/login
GET  /api/v1/auth/me
POST /api/v1/auth/logout
POST /api/v1/auth/password
GET  /api/v1/status
GET  /api/v1/config/cameras
GET  /api/v1/config/recording
GET  /api/v1/config/osd-gps
GET  /api/v1/config/stream
GET  /api/v1/config/network
GET  /api/v1/config/storage
GET  /api/v1/config/system
PUT  /api/v1/config/cameras
PUT  /api/v1/config/recording
PUT  /api/v1/config/osd-gps
PUT  /api/v1/config/stream
PUT  /api/v1/config/network
POST /api/v1/storage/action
GET  /api/v1/diagnostics
POST /api/v1/self-test
GET  /api/v1/diagnostics/export
```

Writable fields are restricted to segment duration, vehicle plate,
OSD/GPS/RTSP/substream switches, and an allowlisted encoder subset for each camera:
- Main resolution: `1920x1080`, `1280x720`, `720x576`, `640x360`, `352x288`
- Main bitrate: `600`, `800`, `1024`, `1500`, `2000` Kbps
- Main FPS: `10`, `12`, `15`, `20`, `25`
- Sub resolution: `720x576`, `640x360` (default), `352x288`
- Sub bitrate: `200`, `350`, `500` (default), `800`, `1000` Kbps
- Sub FPS: `5`, `10`, `12`, `15`

Sub geometry cannot exceed main geometry, and sub FPS cannot exceed main FPS.
Additionally, a hardware memory constraint guard prevents pairing D1 (`720x576`)
substream with 1080p (`1920x1080`) main stream; if D1 is chosen for sub, main is
automatically limited to 720p or lower.

A vehicle plate is empty or 3 through 16 uppercase ASCII letters, digits, dots,
and hyphens. PUT uses form-encoded fixed schemas and requires CSRF. Saving is atomic
and automatically restarts only the camera process while Web and its session remain active.
The Security page supports atomic password change. Generic command, snapshot, and live-page
paths return 404.

The Network page edits only the Wi-Fi AP SSID and channels 1, 6, or 11. A new
password is optional when retaining the current SSID and is required when the
SSID changes. Passwords must contain 8 through 63 characters, are converted to
a WPA PSK through the fixed `/usr/sbin/wpa_passphrase` helper, and are never
returned by the API or written to logs. Applying this setting restarts only
hostapd/dnsmasq through the fixed Wi-Fi service; camera recording is unaffected.

The Storage page exposes only repair and format for the verified SD partition
`/dev/mmcblk1p1`. Both actions require a literal backend confirmation, use a
single maintenance lock, stop the recorder cleanly before unmounting, and
remount with the same ownership/mask options used at boot. No request can
select a block device or execute a generic command. Format is destructive and
must never be used as a validation test on a card containing required footage.

## Security Limits

- Eight fixed sessions, 30-minute sliding timeout.
- Sixteen fixed source-address rate buckets, maximum five login attempts per
  minute and address.
- Random 192-bit session and CSRF tokens from `/dev/urandom`.
- HttpOnly and SameSite=Strict session cookie.
- CSRF validation on logout and every state-changing API.
- 16 KiB maximum request and 4 KiB maximum body.
- Two-second receive/send timeout.
- Exact static route allowlist; no path-derived filesystem access.
- No shell command endpoint and no password in logs or JSON configuration.
- Content Security Policy, frame denial, no-sniff, and no-referrer headers.

The packaged password is a crypt hash for the explicitly requested debug
password. Production provisioning must replace it with a unique per-device
secret.

## Persistent Configuration

All settings changed by Web Config are written atomically below
`/run/rv06-config`. On firmware 0.5.0 and later this is a symlink to
`/persist/dashcam` on the dedicated 8960 KiB JFFS2 MTD partition `dashcfg`.
Normal firmware and FOTA packages deliberately omit this partition, so camera,
recording, OSD, JT808, Wi-Fi, and Web password settings survive an update.

Files under `/oem/usr/etc/dashcam` are immutable firmware defaults used only to
initialize an empty persistent store. A full-chip erase, programmer erase, or
explicit factory erase is outside the normal update path and can erase
`dashcfg`.

## Measured Cost

Direct-deployed target measurements with two main encoders, two sub encoders,
four OSD regions, RTSP, GPS, recorder, hostapd, and dnsmasq active:

```text
Web RSS:             1.0-1.5 MiB
Web idle CPU:        below top reporting resolution
100 status requests: 2.18 seconds total
Status cache:        2 seconds
Frontend polling:    5 seconds
```

Recorder packet delivery and RTSP queues are not shared with the Web process.

## Production cellular/GNSS wording

The production target is EC800M-CN with GNSS. User-facing pages and APIs use
generic `Cellular`, `4G`, `Data session` and `GNSS` labels rather than EC25
names. Runtime status comes from the cached `CellularModemManager` and
`GnssManager` snapshots; browser polling must not issue AT commands.

Display the detected module model/revision, SIM, operator, signal, IMEI,
Device ID, ICCID, registration, packet attach, PDP state, detected interface,
host IP, Internet state, GNSS fix, satellites, coordinates, speed and last-fix
time when available. Do not expose SoC identity or authentication secrets.
Unsupported EC800M-CN GNSS capability must be reported honestly, never replaced
with fabricated coordinates. EC25 is legacy-only and EG800AK is not a
production target.
