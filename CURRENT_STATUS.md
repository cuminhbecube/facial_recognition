# RV06 Dashcam Current Status

## Active Baseline — 2026-08-20

The product target is now:

```text
BECAM-2 = RV1106 + PR2100K dual AHD + EC800M-CN with GNSS
```

- Active source version: `0.6.2` (branch from `0.6.1`).
- Active task: **P0 — Migrate BECAM-2 Cellular Modem to EC800M-CN + GNSS & Production DHCP Networking**.
- Production camera input: PR2100K on I2C4 `0x5f`, two-lane MIPI CSI-2,
  VIN0/VC0 for CAM0 front and VIN1/VC1 for CAM1 rear.
- Main recorders remain hardware H.265 VENC0/VENC1; hardware H.264
  VENC2/VENC3 feed JT1078/RTSP.
- Production cellular target: EC800M-CN hardware variant with GNSS, managed by
  generic `CellularModemManager` with `ModemProfile::EC800M` and `GnssManager`.
- EC25 is legacy-supported via `ModemProfile::EC25` for rollback.
- **Production DHCP Networking COMPLETE**: `CellularModemManager` owns the full
  `udhcpc` process lifecycle (`StartDhcpClient`, `StopDhcpClient`, `ReapDhcpClient`,
  `HandleDhcpLease`, `ClearDhcpState`) using `/oem/usr/bin/becam-udhcpc-script` and
  atomic status file `/tmp/dashcam/cellular-dhcp.status`. Single owner, no duplicate
  process, no zombie processes, automatic cleanup of stale EC25 static routes/IPs,
  dynamic PDP vs host IP separation, and full WebConfig integration.
- BusyBox: `CONFIG_UDHCPC=y` verified and built into rootfs.
- Build: **PASS** (`-Werror` clean). Rootfs/Firmware: **PASS**. Regression guards: **PASS**.

CAM0 & CAM1 dual AHD persistent bring-up and Four-VENC encoder pipeline are CLOSED on target `192.168.9.139`.
All four hardware VENC channels operate simultaneously:
- VENC0: CAM0 MAIN H.265 1280x720@15 (PASS)
- VENC1: CAM1 MAIN H.265 1280x720@15 (PASS)
- VENC2: CAM0 SUB H.264 640x360@12 (PASS)
- VENC3: CAM1 SUB H.264 640x360@12 (PASS - FIXED RK_ERR_VENC_NOMEM)

Production Status Matrix:
- PR2100K dual: PASS (I2C4 0x5f, VIN0/VC0 + VIN1/VC1, 2-lane MIPI CSI-2 1188 Mbps)
- CAM0 capture: PASS (/dev/video0 UYVY 1080p25)
- CAM1 capture: PASS (/dev/video1 UYVY 1080p25)
- Dual VI / CIF: PASS
- Dual recording SD: PASS (H.265 720p15 recording to /mnt/sdcard/DCIM/front and rear)
- Dual RTSP MAIN: PASS (/live/cam0/main, /live/cam1/main)
- Dual RTSP SUB: PASS (/live/cam0/sub, /live/cam1/sub)
- Four VENC: PASS (simultaneous 4 hardware encoders)
- CMA optimization: PASS (raw MB pool count=1, VENC u32BufSize=width*height/4, ref buffer sharing, H264 main profile)
- VENC3 NOMEM: FIXED (0xa004800c resolved; CmaAllocated ~38.8 MB / 64 MB; 26.7 MB headroom)
- CMA resize: NOT REQUIRED (fits comfortably within 64 MiB CMA)
- Stress 600s: PASS (16,226 frames/cam, 0 crash, 0 NOMEM, full CMA release on shutdown to 1.7 MB)
- AGENTS.md exception: ENDED

- JT1078 Dual Live Streaming (CAM0 & CAM1): PASS (simultaneous 12 fps, ~500 kbps, SPS/PPS/IDR gate, 0 swap, 0 drops)
- 10-Cycle Connect/Disconnect Stress: PASS (10/10 cycles, 0 FD leaks, 0 CMA leaks)
- Shared Encoded Fanout (RTSP + JT1078): PASS (shared VENC2/VENC3, max 4 active VENCs)
- Dual SD Recording during JT1078: PASS (continuous 15 fps H.265 uninterrupted)
- Configurable Video Resolutions & Bitrates on WebConfig: PASS
  * VENC0 & VENC1 (Main): 1080p (1920x1080), 720p (1280x720), D1 (720x576), 360p (640x360), CIF (352x288)
  * VENC2 & VENC3 (Sub): D1 (720x576), 360p (640x360 - Default), CIF (352x288)
  * Substream Bitrate: 200, 350, 500 (Default), 800, 1000 Kbps (Independently configurable per camera)
  * D1 Constraint Guard: PASS (When Sub is D1, Main is restricted below 1080p across Web UI, API, launcher, and daemon)
  * Dual 1080p@20 Main + Dual 360p@12 Sub verified on target with ~21.2 MiB safe CMA headroom.

- **Active Release Version**: `0.6.2`
- **Branch**: `0.6.2`
- **Upgrade Artifact**: `Upgrade/upgrade-0.6.2.img` (76 MB)
- **SHA256**: `61b0cb22e5a6b535712a925856e6d8edadf47752b1018ac848938fcad34f83e7`
- **Live Target State**: `ONLINE` (`192.168.43.100/24` via `usb0`, GPS FIX 11 sats on `/dev/ttyUSB3`, JT808 heartbeats active to `cam.tracking.vn:6608`)

## Historical Handoff Update — 2026-08-11

### Firmware 0.5.25

| Artifact | SHA256 | State |
|---|---|---|
| `Upgrade/upgrade-0.5.25.img` | `ff7092b8755430744733b0d2367f36e2e99c9e801f1d792d041ecab50145f266` | Full firmware build PASS with product privacy and RAM/Internal Flash status |
| `project/app/dashcam/rv06_webconfig` | `115dcb23d6f3e513133bd0b6e13444e2990b34cbd35f810ddf1ee238e4cd073b` | Build PASS |
| `project/app/dashcam/rv06_dashcam` | `6e0eb0be25467fe93b14bec72de4945bd9d7ad36c0c868adb945ab3273b66166` | Build PASS |

### Web Config system-page cleanup

- The `Hệ thống` navigation entry has been removed.
- Maintenance now contains firmware/service status, Self Test, Diagnostics
  Export, configuration Export/Import, Restart Camera, Reboot Device and the
  final `Vùng nguy hiểm` Factory Reset section.
- `/system` is compatibility-only and returns `302 Location: /maintenance`.
- `/api/v1/config/system` and `/api/v1/system/action` remain available to avoid
  backend regressions.
- Unauthenticated `/maintenance` returns `302 Location: /login`, as expected.
- Target evidence after deployment: `rv06_dashcam` PID 565,
  `rv06_webconfig` PID 3495, CAM0/CAM1 `RECORDING`, EC25 `ONLINE`.

The final Maintenance JavaScript audit is closed in source: `Build` reads the
real `status.device.build` value, action listeners are registered before any
status fetch, and independent `Promise.allSettled()` fallbacks keep the page
operational when Snapshot, JT808 or another status endpoint is unavailable.
Regression guards cover the listener ordering, fallback fields, CSRF endpoint
wiring and exact confirmation literals. Reboot Device and Factory Reset remain
source/wiring-verified but were not invoked during non-destructive validation.

### Product model privacy

- Product-facing model is fixed to `BECAM-2` in Web status, Device Info,
  system configuration API, JT808 registration and terminal attributes.
- Web APIs no longer return SoC or hardware-version fields; Dashboard and
  Device Info no longer create those DOM rows.
- `0x0100`, `0x0107` and `0x0104` use `BECAM-2` instead of an SoC/SDK model.
- Internal RV1106 camera, SDK, build and driver paths remain unchanged.
- Build and host regression PASS. Deployment retry to `192.168.1.232` was
  pending when the target became unreachable (`No route to host`).

### Dashboard RAM and Internal Flash

- Cached status now exposes nested `memory` and `flash` byte counters.
- RAM used is `MemTotal - MemAvailable`; `MemFree` is not used.
- Internal Flash is measured with `statvfs("/")`; it is not the SD card and is
  not combined with `/mnt/sdcard` or the persistent-config partition.
- Dashboard renders compact RAM/Flash meters with 80/90% and 85/95%
  warning/critical thresholds respectively.
- Diagnostics export includes `rv06_dashcam` RSS in bytes.
- No shell command or filesystem scan is used. Build and regression PASS;
  runtime deployment remains pending while `192.168.1.232` is unreachable.

## Latest Handoff Update — 2026-08-10

This section is the current source/build handoff; older historical sections
below remain for traceability.

### Firmware and build checkpoints

| Version | Artifact | SHA256 | State |
|---|---|---|---|
| 0.5.13 | `Upgrade/upgrade-0.5.13.img` | `5d942baefecead429007210aefe8873438fc978845db061ecc3e89b2e8181b8b` | historical target build |
| 0.5.18 | `Upgrade/upgrade-0.5.18.img` | `a33a49406b77534a07fc367c1dbc1c3142fe3af638a6ffab105127fcd890d598` | historical package |
| 0.5.19 | `Upgrade/upgrade-0.5.19.img` | `6c13c300ed32af5f67dab63845adc6a632cc62c219627450c8393f6b305e9422` | historical package |
| 0.5.20 | `Upgrade/upgrade-0.5.20.img` | `72141d048416ede25892b3c83ffcd67db093f26d2f314cef55ce33987ac586d3` | historical package |
| 0.5.21 | `Upgrade/upgrade-0.5.21.img` | `9db214d9c006580eed008fb1e04e0ca3da5c343823a81c04deaa63e6663097ec` | historical package |
| 0.5.22 | `Upgrade/upgrade-0.5.22.img` | `1cd89278e2b9c11de3d5c72b3ab1dca600b96e1f70cafead31378e4faeb99449` | Active release with EC25 4G Cellular, Wi-Fi AP & full audit fixes |

The current unstripped application binary builds successfully with SHA256:

```text
project/app/dashcam/rv06_dashcam
aeb0b928b044f3f2c4e12f302726107c6add05d74c2346f739954c5cbe0269a2
```

This newer source binary fixes the snapshot empty/invalid-packet VENC release
path and logs `[MEM]` RAM/CMA/RSS/FD telemetry every 60 seconds. It has not yet
been packaged or deployed. The stripped `0.5.21` binary embedded in the last
rootfs and observed on target has SHA256
`e9e0eef48bc939f500bc3dddde87fc1a43c2bbe05c865cebc85ec6e82d0e7171`.

### Reliability fixes and regression coverage

- CAM1 ISP/VI/VENC/bind failures degrade to CAM0-only instead of tearing down
  the mandatory CAM0 pipeline. ISP group mode is explicit opt-in.
- SD boot handling never formats media automatically. Invalid/unpartitioned
  cards remain unavailable until an authenticated manual maintenance action.
- Playback file overlap uses the configured segment duration and playback speed
  changes wall-clock pacing without changing media timestamp cadence.
- Firmware packaging rebuilds/verifies the staged app SHA and rejects stale
  rootfs inputs.
- Web Config uses four bounded workers with a maximum pending queue of 16.
- Launcher/log monitor PID files prevent duplicate processes; Wi-Fi recovery is
  retry-bounded and propagates real startup failures.
- `make host-regression` runs source-level regression guards for these critical
  invariants without requiring the RV1106 target or full SDK emulation.

Target `192.168.1.253` evidence on 2026-08-09:

```text
CAM0/CAM1 ISP, VI, VENC: READY
CAM0/CAM1 main/sub stream: READY/STREAMING
JT808: ONLINE, authentication and heartbeat ACK successful
Web Config: HTTP 302 to /login (expected unauthenticated behavior)
Wi-Fi AP: wlan0 192.168.50.1
Launcher instances: 1
Recording: WAITING_FOR_SD (media fault, not camera fault)
```

### RFID/GPLX current implementation

- Production `DriverUartManager` no longer sends periodic `READ_CARD` polling.
- `/dev/ttyS3` remains continuously drained with `poll()` and a persistent
  parser; partial, combined and unsolicited card frames are supported.
- RX remains active while ACC is OFF; card actions are ignored in that state.
- Duplicate debounce is 500 ms; the former 5-second cooldown/card-removal
  gate is removed.
- Accepted cards produce LOGIN/LOGOUT/SWITCH and JT808 `0x0702`; the last
  reachable target showed CMS ACK `result=0`.
- Comma-terminated driver-name parsing was fixed so valid cards are not
  incorrectly reported as `INVALID_IDENTITY`.
- IO_CONTROL drives POWER, NET, GPS, DRIVER, REC and BUZZER. Alarm and CAM0..3
  packet fields remain zero.
- LED FLASH uses reader value `0x13`, not a software blink loop. The latest
  source adds short DRIVER FLASH for rejected cards and emits `[RFID_LED]`
  only when the complete LED state changes.

Last target evidence:

```text
[RFID_CARD] license=123456789012 action=LOGIN rev=5
[JT808][DRIVER] report=0x0702 event=LOGIN revision=5 sequence=16
[JT808][DRIVER] ack sequence=16 result=0 state=ACKED
[REC][CAM0 OV8858] state=RECORDING
[REC][CAM1 GC2053] state=RECORDING
[JT808] state=ONLINE
```

### Repository cleanup

Removed obsolete target-specific scripts containing hard-coded deployment
commands/credentials: `deploy_232_final.exp` and `test_logs_232.exp`.
Generated SDK build directories and old firmware images remain retained for
reproducible builds and rollback; persistent configuration and backups are not
routine cleanup targets.

## Current Goal

Keep dual-camera H.265 recording stable while enabling EC25-E GNSS, RTSP main
streams, hardware substreams, CMSV6/JT808/JT1078 and lightweight Web Config
without browser video. Local/Web snapshot and CMS `0x8801 -> JPEG -> 0x0805`
with multimedia upload are implemented for both cameras, including the RAM
fallback when SD is unavailable. Audio remains disabled.

Playback search `0x9205 -> 0x1205` and playback session control `0x9201` are
implemented. Completed H.265 recordings are selected by real configured
segment duration, parsed as Annex-B access units and sent through the shared
JT1078 packetizer with requested playback-speed pacing. CMS end-to-end display
must still be regression-tested whenever packetizer or recording layout changes.

## Hardware Mapping

| Role | Sensor | Hardware path | Raw node | ISP node | VI dev/pipe | VENC | Output |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Front | OV8858 | I2C4 `0x36` -> DPHY1/MIPI0 -> CIF0 -> ISP0 | `/dev/video0`, BGGR10, 1632x1224@30 | `/dev/video22`, NV12 | `0/0` | `0` | `/mnt/sdcard/DCIM/front` |
| Rear | GC2053 | I2C3 `0x37` -> DPHY2/MIPI1 -> CIF1 -> ISP1 | `/dev/video11`, GRBG10, 1920x1080@30 | `/dev/video32`, NV12 | `1/1` | `1` | `/mnt/sdcard/DCIM/rear` |

Do not confuse CAM1 raw node `/dev/video11` with CAM0 auxiliary node
`/dev/video1`.

IQ files used on target:

- `ov8858_HS5885-BNSM1018-V01_default.json`
- `gc2053_CMK-OT2274-V10_28IRC-F20.bin`

## Active Pipeline

```text
OV8858 -> CIF0 -> rkisp-vir0 -> VI 0:0 -> H.265 VENC 0 -> SD
GC2053 -> CIF1 -> rkisp-vir1 -> VI 1:0 -> H.265 VENC 1 -> SD
```

The production path uses hardware `RK_MPI_SYS_Bind(VI, VENC)`. Each camera has
an independent AIQ context. Both AIQ contexts are initialized before
`RK_MPI_SYS_Init`, and `rk_aiq_uapi2_sysctl_setMulCamConc(ctx, true)` is enabled
for dual operation.

Required launcher settings:

```sh
RV06_CAMERA_MODE=dual
RV06_DIAGNOSTIC=off
RV06_ISP_GROUP=off
```

`RV06_ISP_GROUP=off` is mandatory. Do not enable camgroup for this heterogeneous
OV8858 + GC2053 pair. Camgroup produced `camgroup result map overflow` and
`no free aiq params buffer`, causing CAM0 black frames and CAM1 purple frames.

## Recording Settings

| Setting | CAM0 | CAM1 |
| --- | --- | --- |
| VI output | 1280x720 NV12 | 1280x720 NV12 |
| Sensor/VI maximum | 1632x1224 | 1920x1080 |
| FPS | 15 | 15 |
| Codec | H.265 hardware VENC | H.265 hardware VENC |
| Rate control | VBR | VBR |
| Target bitrate | 1024 Kbps | 800 Kbps |
| Maximum bitrate | 1324 Kbps | 1100 Kbps |
| GOP | 30 | 30 |
| VI buffers | 2 | 2 |
| VENC stream buffers | 2 | 2 |
| Segment duration | 180 seconds | 180 seconds |

The recorder requests IDR and waits for VPS/SPS/PPS plus a random-access NAL
before opening a segment. It writes to a hidden `.h265.tmp`, calls `fsync`, then
renames the non-empty file. Timestamps use UTC+7 through the shared
`VietnamTimestamp` fallback.

## Verified Runtime Result

Target after flashing `0.3.4-debug`:

- Current observed DHCP address: `192.168.1.247` (may change).
- Fixed Ethernet MAC: `02:11:06:00:00:01`.
- SD: `/dev/mmcblk1p1` mounted read/write at `/mnt/sdcard`, 119.1 GiB visible.
- CAM0 and CAM1 both exceeded 2,700 encoded frames without restart.
- First complete 3-minute segment after boot:
  - Front: 23,051,931 bytes.
  - Rear: 4,744,697 bytes.
- Both streams were decoded with VLC and produced visible frames.
- No `CAMGROUP`, `CIF_ISP_PIC_SIZE_ERROR`, `no free aiq params buffer`, recorder
  crash, or zero-byte output was observed after the fix.
- MIPI reported 62 CRC and 21 data errors only during startup, ending at uptime
  27.77 seconds. No further MIPI error was observed through segment close.

Previous flashed firmware image:

```text
Upgrade/upgrade-0.3.4-debug.img
SHA256 54d74ba3e3f8dfe1c38c9f27cd775803f978789251974c08022966604729cb24
```

## Storage And OSD Added For 0.3.5

- The recorder accepts only `/dev/mmcblk1p1` mounted read/write at
  `/mnt/sdcard`; it never falls back to rootfs.
- Free-space reserve is 5% of the card, clamped to 256 MiB through 2 GiB. When
  needed, only the oldest regular `.h265`, `.265`, or `.mp4` recording under
  `DCIM` is deleted.
- Hidden interrupted `.h265.tmp` files older than 10 minutes are removed.
- The launcher first mounts FAT, then attempts `fsck.vfat -a`, and formats FAT32
  only after the exact SD block device and partition have been verified.
- VENC0 and VENC1 use RK MPI `OVERLAY_RGN` handles 16 and 17 in
  `RK_FMT_ARGB8888`, 352x64, aligned to 16 pixels, at bottom center.
- OSD foreground is opaque white and the background is fully transparent. It
  shows Vietnam time and the static camera label; it contains no `REC` or fake
  GPS data.
- Static text is rasterized once. Time is updated once per second without
  recreating the Region, and an OSD error never stops recording.

Pre-package target test recorded both cameras for 50 seconds and decoded both
outputs with VLC. CAM0 OSD static rasterization took 211 us and a time update
took 797 us; CAM1 took 95 us and 586 us respectively. Physical auto-format and
24-hour endurance tests have not yet been run.

The same binary was then direct-deployed through the production init path and
completed a full 180-second segment while starting the next segment:

- Front: `front_20260801_220607.h265`, 23,285,404 bytes.
- Rear: `rear_20260801_220607.h265`, 8,821,392 bytes.
- Process RSS: 17,276 KiB.
- RK DMA heap: 38,492 / 49,152 KiB used, 10,660 KiB available.
- No storage, OSD, CIF/ISP, camgroup, write, rename, or empty-file error.

Packaged and flash-validated firmware:

```text
Upgrade/upgrade-0.3.5-debug.img
SHA256 5369a7479347847ab2b417426cc7053ccb388d8301dc8aa451ce218b584af871
Size 78,924,362 bytes
```

Post-flash verification at `192.168.1.230` confirmed that the target binary and
launcher checksums match the files staged in the image. Both cameras exceeded
7,200 encoded packets, completed multiple 180-second segments, and continued
recording. The first post-boot pair was:

- Front: `front_20260801_222108.h265`, 22,464,185 bytes.
- Rear: `rear_20260801_222108.h265`, 1,628,966 bytes.

Both post-flash files decoded successfully with VLC and showed the white,
transparent, bottom-centered OSD. The process used about 18 MiB RSS and 15% CPU
while the system reported about 70% CPU idle. Startup MIPI errors ended at
uptime 27.89 seconds and did not recur during the checked recording interval.

## GPS, RTSP And Substream Added For 0.3.6

EC25-E GNSS is identified from USB VID/PID `2c7c:0125` and a common USB parent.
Interface `01` is used for NMEA and interface `02` for AT; `/dev/ttyUSB1` and
`/dev/ttyUSB2` are observed runtime names, not hard-coded identities. The AT
port must answer `OK`, is protected with an exclusive `flock`, and is never
used as the NMEA reader. The manager starts GNSS when required and prefers raw
NMEA GGA/RMC/VTG; `AT+QGPSLOC` fallback is deliberately disabled while the USB
NMEA interface works.

On the target, EC25-E reported a real fix around `20.9902, 105.8571`, zero
vehicle speed, 3-4 satellites, and HDOP 0.9-1.4. Valid GGA satellite and HDOP
values survive subsequent RMC/VTG updates. Raw GGA/RMC debug is rate limited to
one line per 10 seconds. Fix state is logged on a transition or every 30
seconds. A five-second stale timeout resets OSD coordinates and speed to zero.

Unbinding physical USB parent `1-1.3` produced
`state=UNAVAILABLE reason=USB_DISCONNECTED` and zero coordinates while the same
camera process and both recorders kept running. Rebinding rediscovered both
interfaces and restored the real fix without rebooting or restarting cameras.

RTSP uses the LubanCat `librtsp.a` implementation on TCP port 554:

| Stream | Pipeline | Endpoint |
| --- | --- | --- |
| CAM0 main | existing VENC0 H.265 1280x720@15 | `/live/cam0/main` |
| CAM1 main | existing VENC1 H.265 1280x720@15 | `/live/cam1/main` |
| CAM0 sub | VI0:1 -> VENC2 H.264 640x360@12 | `/live/cam0/sub` |
| CAM1 sub | VI1:1 -> VENC3 H.264 640x360@12 | `/live/cam1/sub` |

Main RTSP copies the existing VENC packet only after recorder write succeeds.
It does not decode or re-encode main video. Each substream branches from the
same VI/ISP pipeline with one VI buffer and two VENC buffers; it is created only
after every active main recorder has accepted at least 300 packets. All four
RTSP queues are bounded at 60 packets. Overflow drops that stream's queue and
waits for the next codec configuration plus IDR; it cannot block SD recording.

Direct-deployed candidate checksum:

```text
/oem/usr/bin/rv06_dashcam
SHA256 5997ba3d56b70faf9f46f13ed937fccb446e2a73eb36273eb247aadcf3dcc96f
```

Runtime validation after the final deploy confirmed all four sessions reached
`READY`. CAM0/CAM1 substreams started after 315/313 successful main packets and
continued beyond 360 packets. VLC over RTSP interleaved TCP decoded CAM0 sub as
H.264 and generated visible 640x360 frames with transparent OSD, real GNSS
coordinates, and speed. The same implementation before the final logging and
failure-handling refinements decoded all four URLs.

The final binary completed two consecutive three-minute front/rear segment
pairs. The first pair, `front_20260801_225809.h265` and
`rear_20260801_225809.h265`, was 22,462,386 and 1,586,254 bytes. VLC decoded
both files and generated visible frames with the expected OSD. The next pair
closed at 22,620,356 and 1,411,461 bytes. No queue overflow, CIF/ISP size error,
write error, or recorder restart occurred.

With two main encoders, two sub encoders and four OSD regions active, RK DMA
heap usage was 42,892 / 49,152 KiB, leaving 6,260 KiB. Process RSS was about 24
MiB and application CPU about 26%. The final direct-deployed run has not yet
completed a 24-hour endurance test.

Packaged and flash-validated firmware:

```text
Upgrade/upgrade-0.3.6-debug.img
SHA256 ebdcd1d6b50bedd9cdd59d02a35be5071ea97b9ec836208a5552ab88dc5eddba
Size 79,055,434 bytes
```

The stripped binary staged in rootfs has SHA256
`4012c96d920f41dec30241f0f6dccdfb6195bc9f25956a60f2888a45da4767a8`
and contains all four RTSP paths plus EC25 USB/NMEA discovery strings.
Post-flash validation at `192.168.1.231` confirmed that exact checksum, fixed
MAC `02:11:06:00:00:01`, EC25-E NMEA fix, four RTSP sessions, and both hardware
substreams. The first post-flash segment pair was
`front_20260801_231254.h265` at 22,735,329 bytes and
`rear_20260801_231254.h265` at 1,482,930 bytes; both recorders immediately
opened their next segment. VLC decoded CAM1 sub over RTSP-TCP and produced
visible H.264 frames with OSD. No camera, encoder, queue, or recorder error was
observed during this check.

## Web Config Added For 0.3.7

`rv06_webconfig` is a separate native C++ process listening on port 80. It does
not link Rockit, RK MPI, RTSP, OpenCV, an image codec, or a Web framework. It
uses one bounded request loop, two-second socket timeouts, a two-second status
cache, eight fixed session slots, and 16 fixed login-rate buckets. The target
binary is about 200 KiB unstripped and used 1.2-1.5 MiB RSS during validation.

There is deliberately no Web stream or preview. `/live` and the snapshot API
return 404; the frontend contains no `video`, `canvas`, `iframe`, or camera
image element. RTSP remains the only live-video interface.

Configuration and status are separated into real page loads:

```text
/
/cameras
/recording
/osd-gps
/stream
/network
/storage
/system
/maintenance
/diagnostics
/security
```

The pages are static HTML/CSS/JavaScript with no CDN or Internet dependency.
Dashboard status polls at five-second intervals. Validated configuration is
shown read-only: no camera, encoder, network, recorder, format, factory-reset,
or reboot control is presented as active until its backend apply and rollback
path is implemented and target-validated.

Authentication uses the provisioned debug password hash for password
`86868686`; the clear password is not installed or logged. Sessions use random
HttpOnly, SameSite=Strict cookies. CSRF is required for logout and reserved for
all future state changes. Login is rate limited, every login/logout is audited,
request/body sizes are bounded, static paths are allowlisted, and there is no
generic command endpoint.

Direct deployment at `192.168.1.231` verified LAN and AP access
(`http://192.168.50.1/`). Playwright exercised all 11 routes at 390x844 and
1440x900 with no console error, horizontal overflow, or media element. One
hundred authenticated status requests completed in 2.18 seconds. During that
load, both recorders closed growing segments, both substreams passed 20,000
packets, GNSS retained a real fix, and no CIF/ISP, encoder, queue, write, or
rename error appeared.

Firmware packaging completed as
`Upgrade/upgrade-0.3.7-debug.img` (79,055,434 bytes, SHA-256
`db099c0ef9f441f1cb13e8da6c58516b765d6339eed12736bc6bf2e7d325a6ba`).
The packaged rootfs contains stripped `rv06_webconfig` SHA-256
`a6c6e7687160231b4047e12fee0529ae1c3b83030740a9b2f75dc5e59ace9e2b`
and unchanged stripped `rv06_dashcam` SHA-256
`4012c96d920f41dec30241f0f6dccdfb6195bc9f25956a60f2888a45da4767a8`.
The password hash is installed mode `0600`; all 13 Web assets and launcher
stop/start integration are present. This packaged image is `CHUA XAC MINH`
after flashing; direct-deployment validation does not replace a post-flash
test.

## Writable Runtime Config Added For 0.3.8

The Web UI now edits only the target-validated runtime subset: recording
segment duration (`60`, `120`, `180`, `300`, or `600` seconds), OSD, EC25 GPS,
RTSP, and hardware substream enable flags. Camera identity, VI/VENC mapping,
resolution, FPS, bitrate, storage device, network topology, and Web video stay
locked. Web video remains absent.

`/oem/usr/etc/dashcam/runtime.conf` is a fixed key/value schema. The launcher
does not source it; every key and value is parsed through an allowlist and an
invalid value falls back to the proven default. PUT APIs require authentication
and CSRF, reject unknown fields and invalid cross-field combinations, then
write `.new`, call `fsync`, rename the previous file to `.backup`, atomically
rename the new file, and sync the directory. Saving never reboots or restarts
the device automatically.

Application restart is a separate fixed command. It requires CSRF, the current
admin password, the exact confirmation `RESTART APP`, a browser confirmation,
and an audit entry. There is still no generic shell-command endpoint. Format
SD, factory reset, Wi-Fi mutation, camera geometry, snapshot, and Web streaming
remain unavailable.

Direct deployment at `192.168.1.232` validated rejection of missing CSRF
(`403`), invalid segment duration (`400`), and substream enabled while RTSP was
disabled (`400`). A valid change to 60-second segments was saved, reported as
pending, applied through the Web restart command, and caused both recorders to
close and reopen a segment: CAM0 7,540,925 bytes and CAM1 5,041,514 bytes. The
setting was restored to 180 seconds and applied through the same command.
CAM0/CAM1 recording, both hardware substreams, EC25 fix, RTSP and Web all
returned after each restart without a CIF/ISP or recorder error. Final measured
RSS was about 24.1 MiB for `rv06_dashcam` and 1.3 MiB for Web Config. After the
final restore, the normal 180-second segments closed at 22,660,464 bytes for
CAM0 and 15,470,072 bytes for CAM1, and both next segments continued growing.

Playwright exercised all 11 routes at 390x844 and 1440x900 with the new select,
toggles, and restart form. There was no console error, horizontal overflow, or
video/media element.

Firmware packaging completed as
`Upgrade/upgrade-0.3.8-debug.img` (79,055,434 bytes, SHA-256
`66cc1389f7b68a6a3e80f07472aee60074569f60c6cfd08eca1190f70eb1515a`).
The packaged rootfs contains stripped `rv06_dashcam` SHA-256
`c56879c2560c8b8f35692b79d45c8c43cd1d81c17c6e963eaf1f356c6d86d763`
and stripped `rv06_webconfig` SHA-256
`2baf3954de7b90a7877e482102919c3f047fc042cffaec5f77841a92acf7b71f`.
Post-flash validation on `192.168.1.233` subsequently confirmed the exact
0.3.8 stripped hashes above. Both main recorders completed a 180-second
segment, both substreams advanced, RTSP/Web listened on ports 554/80, and no
CIF/ISP error appeared. The persisted runtime config had GPS deliberately set
to `off`, and the running state matched it.

## Automatic Apply And Maintenance Added For 0.3.9

The explicit `RESTART APP` form and endpoint have been removed. A successful
configuration PUT now writes atomically and automatically asks the launcher to
restart only `rv06_dashcam`. The native Web process, authentication session,
Wi-Fi AP, and LAN remain active. The launcher reloads the fixed allowlist before
starting the camera again. A scheduling failure rolls the config back instead
of leaving a saved but unapplied value.

Target validation changed segment duration from 180 to 60 seconds and back to
180 using only the Save API. Web PID and CSRF session remained unchanged while
the camera PID changed and both main/sub pipelines returned. A terminated apply
child initially exposed a zombie process; Web now ignores `SIGCHLD`, and the
second apply completed with only the live Web and camera processes remaining.

The Security page can change the admin password after current-password and CSRF
validation. The new salted crypt hash is written atomically mode `0600`, other
sessions are revoked, and no password enters JSON or audit logs. Target testing
temporarily changed the password, proved the old password was rejected and the
new password accepted, then restored `86868686` successfully.

Maintenance now provides a non-disruptive 15-item self-test and a bounded text
diagnostics export. The target result was 12 `PASS`, GPS `DISABLED` according to
config, and snapshot/hardware watchdog `NOT_SUPPORTED`. The exported report was
38,376 bytes and contained only fixed runtime-config, meminfo, mounts, recorder
log tail, and Web audit sections. Playwright passed all 11 routes at mobile and
desktop viewports with no console error, overflow, or media element.

Firmware packaging completed as `Upgrade/upgrade-0.3.9-debug.img` (79,186,506
bytes, SHA-256
`cbbe343952c09010d6da25f5b9aa0de87c6741299b5a672bfb442e62c8446ec9`).
The packaged rootfs contains stripped `rv06_dashcam` SHA-256
`c56879c2560c8b8f35692b79d45c8c43cd1d81c17c6e963eaf1f356c6d86d763`
and stripped `rv06_webconfig` SHA-256
`d048fbce6f55d673f9e4fa49249b6f215f76fbfaa16be764db912dce39fc0feb`.
The packaged admin password hash is mode `0600`, runtime configuration is mode
`0644`, and the rootfs contains the automatic apply marker, password-change,
self-test, and diagnostics-export paths.

Post-flash validation on `192.168.1.234` confirmed fixed Ethernet MAC
`02:11:06:00:00:01` and exact packaged hashes for `rv06_dashcam`,
`rv06_webconfig`, and `dashcam.sh`. Both main recorders received frames and
closed a 180-second segment, both substreams emitted packets, GPS had a real fix,
RTSP/Web/Wi-Fi AP were ready, and `/dev/mmcblk1p1` was mounted read/write with
about 119 GiB total capacity. No `CIF_ISP_PIC_SIZE_ERROR`, `ERROR_NO_FRAME`,
assertion, or crash appeared.

Automatic apply was tested after the flash by changing segment duration
`180 -> 60 -> 180`. Both PUT requests returned HTTP 202, the existing Web PID
and CSRF session survived, only the camera process restarted, and
`runtime.applied.conf` returned to 180 seconds. After the pipeline warm-up, all
supported self-test items passed; snapshot and hardware watchdog remained
`NOT_SUPPORTED`. Diagnostics export produced a bounded 40,373-byte report, and
the served JavaScript contained no restart command or Web video element.

## Configurable Encoder Controls Added For 0.4.0

The Camera page now edits CAM0 and CAM1 independently. The runtime schema v2
contains main resolution, main FPS, main bitrate, sub resolution, and sub FPS
for each camera. Sensor identity, VI dev/pipe/channel, VENC channel, codec,
buffer count, and SD ownership remain fixed. The allowlists are:

```text
main resolution: 1280x720, 1024x576, 640x360
main FPS:        10, 15, 20
main bitrate:    600, 800, 1024, 1500, 2000 Kbps
sub resolution:  640x360, 640x480
sub FPS:         5, 10, 12, 15
```

Both frontend and backend reject a sub resolution larger than main or a sub FPS
higher than main FPS. Runtime files from 0.3.9 safely receive the proven defaults
for missing schema-v2 fields. Saving writes atomically and uses the existing
automatic camera-only restart path.

Direct target validation on `192.168.1.234` rejected an invalid
`640x360 main + 640x480 sub` request with HTTP 400. A valid mixed test applied
CAM0 `1024x576@10`, 800 Kbps with `640x480@10` sub, and CAM1 `640x360@15`,
600 Kbps with `640x360@12` sub. VI and VENC dimensions matched in the target
log; both main recorders and both substreams emitted packets without CIF/ISP,
size, or no-frame errors. The resulting front/rear H.265 segments decoded in
VLC at the requested main resolutions with visible transparent OSD. Settings
were restored to the original 1280x720 main and 640x360 sub defaults, after
which all supported self-test items passed.

Playwright exercised all 11 routes at 390x844 and 1440x900. The Camera page had
two separate camera groups and ten selects, with no overflow, console error, or
Web media element.

Firmware packaging completed as `Upgrade/upgrade-0.4.0-debug.img` (79,186,506
bytes, SHA-256
`7d8de20f20ce8d0c07ff547ff6ed3ceea48db9ade90a855ee3920ed5d2e71505`).
The packaged rootfs contains stripped `rv06_dashcam` SHA-256
`0b67634050e32e1fd47bf4529cd08b51e942d5fba04ee824d3d8a8d257559119`,
stripped `rv06_webconfig` SHA-256
`cee28a72cc2663519cecb7fba3affd50434f9bde43d68fa460f814dc1297ac7f`,
and launcher SHA-256
`dcb0d1b30a0b222f0aff04ccd120cc0927109b5e7fb3a2d3984c463e1831facf`.
The packaged runtime file is schema v2 mode `0644`, the password hash remains
mode `0600`, and the served assets contain neither Web video nor an explicit
restart command.

Post-flash validation on `192.168.1.236` confirmed fixed MAC
`02:11:06:00:00:01` and exact packaged hashes for the camera binary, Web binary,
and launcher. Runtime and applied files both used schema v2 with the expected
defaults. A Camera API test changed only CAM1 main bitrate `800 -> 600 -> 800`
Kbps. Both changes returned HTTP 202, the Web PID and CSRF session survived,
only the camera PID changed, and target VENC logs showed the requested bitrate.
After restore, all supported self-test items passed, GPS had a real fix, both
substreams emitted packets, and no CIF/ISP, size, no-frame, write, rename,
assertion, or crash error appeared.

The restored pipeline then completed a natural 180-second segment pair:
`front_20260802_013913.h265` at 22,997,128 bytes and
`rear_20260802_013913.h265` at 3,732,898 bytes. Both recorders immediately
continued into the next segment. The SD remained `/dev/mmcblk1p1` mounted
read/write with about 119 GiB visible.

## Driver UART And CAM0 Identity OSD Added For 0.4.1

UART3 is enabled by the existing LubanCat RV06 Device Tree and appears as
`/dev/ttyS3` under platform device `ff4d0000.serial`. The debug console remains
on `/dev/ttyFIQ0`. The new manager configures UART3 as 115200 8N1, parses a
strict UTF-8 DRV1 JSON Lines schema with JsonCpp, validates identity fields,
keeps a bounded 64-ID replay window, and stores the active driver only in RAM.
The target rootfs already provides `libjsoncpp.so.26`.

CAM0 identity text is now `CH1 | <vehicle plate> | <driver>`. Login/logout
events update only the bitmap of the existing VENC Region from the OSD worker;
they do not recreate the Region or touch VI, ISP, VENC, RTSP, recorder, or the
active segment. Time, EC25 coordinates, and speed retain their existing rows
and one-second update interval. CAM1 keeps its current static label.

The Web OSD/GPS page accepts an empty plate or 3 through 16 ASCII letters,
digits, dots, and hyphens. It canonicalizes letters to uppercase and writes
`vehicle_plate` through the existing atomic runtime-config mechanism. Runtime
schema is now version 3.

The ARM parser test passed on the RV1106 target for login, duplicate ID,
numeric-license rejection, invalid event/JSON, and logout state clearing.
Direct deployment on `192.168.1.236` showed UART `READY`, both main recording
files growing, both hardware substreams streaming, four OSD Regions ready, and
EC25-E retaining a real fix. An explicit UART3 driver unbind/rebind kept the
same `rv06_dashcam` PID while the front file grew from 14,065,284 to 14,812,642
bytes; the UART manager logged `UNAVAILABLE` and recovered to `READY`.

Web validation rejected an invalid plate with HTTP 400 and accepted a valid
plate with HTTP 202. Playwright passed all 11 routes at 390x844 and 1440x900
with the plate field present, no console error, no horizontal overflow, and no
Web media element. VLC decoded target segment `front_20260802_021557.h265`
(23,330,906 bytes); a captured frame showed transparent white OSD text
`CH1 | 30A-12345`, unchanged time/GPS/speed rows, and no `CAM0 FRONT` text.

Firmware packaging completed as `Upgrade/upgrade-0.4.1-debug.img` (79,186,506
bytes, SHA-256
`4b3ef7a257795ebae59992d2b873bd4817c0253e2c8bd5889c349464d8d487bc`).
The packaged rootfs contains stripped `rv06_dashcam` SHA-256
`e90fe5f3600fac292295d00171cf6ac303f738830574d10a4f552ca739b7b6ff`,
stripped `rv06_webconfig` SHA-256
`ba48897d67620e409932e76b5866894e9ea90c5b64dd5f5bf9ab172bdc39a0d4`,
runtime schema v3, and `libjsoncpp.so.26`. Physical external-UART login/ACK and
a captured frame containing a live driver name are still `CHUA XAC MINH`
because no UART sender or TX/RX loopback was attached during this run. The
packaged image is not yet flash-validated.

## Temporary Debug UART For 0.4.2

DRV1 temporarily moved from `/dev/ttyS3` to the existing debug UART
`/dev/ttyFIQ0`. The launcher publishes the selected device through the fixed
`RV06_DRIVER_UART_DEVICE` environment variable; production code accepts only
`/dev/ttyFIQ0` or `/dev/ttyS3`. Input-only flush replaces the previous full
TTY flush so opening the protocol reader does not discard queued console TX.

The Buildroot console getty is disabled because it otherwise competes with the
DRV1 manager for RX bytes. Kernel console output remains enabled, so a temporary
external controller must ignore non-DRV1 lines on TX. Direct deployment at
`192.168.1.236` confirmed no getty process and only `rv06_dashcam` holding an
open descriptor to `/dev/ttyFIQ0`. The manager reported `READY` at 115200 8N1.

During a five-second check the same camera PID remained active while the front
pending file grew from 12,023,850 to 12,785,225 bytes and rear grew from
1,740,257 to 1,846,861 bytes. Both substreams were ready and no CIF/ISP,
no-frame, write, rename, assertion, or crash error appeared. A CH340 debug
adapter was visible as Windows COM12, but another terminal process held it;
physical login/ACK remains `CHUA XAC MINH` rather than forcibly closing the
user's terminal.

The final package is `Upgrade/upgrade-0.4.2-debug.img` (79,186,506 bytes,
SHA-256
`436f76ff41f5c209d87f6fff79549746156ba5d123110c8e6fa1deaa86a2422b`).
The packaged rootfs has no active serial getty entry. Its stripped
`rv06_dashcam` SHA-256 is
`057c619f978dd3f2331abca886b08445cd7f9fb772ed3b672e55a6735d9a7b50` and
its stripped `rv06_webconfig` SHA-256 is
`8d154f64c239ff0f77022b15b10ce393a5d2e849daf23653145f7b348f6d631c`.
Both binaries and the launcher report version `0.4.2-debug`. This image is
packaged but not yet flash-validated.

## Firmware 0.4.3 Candidate Validation

The directly deployed candidate on `192.168.1.237` adds CR, LF, CRLF, and
250-ms complete-object framing plus bounded RX transport logs. Three physical
COM12 transmissions (`id=43001` through `43003`) using CR, LF, and CRLF
produced no `state=RX` line and no ACK. The manager remained `READY` on
`/dev/ttyFIQ0`; both cameras, four RTSP sessions, OSD, substreams, and recording
remained active. This proves the attempted bytes did not reach the Linux TTY;
the external USB-UART TX-to-board-RX path is still `CHUA XAC MINH`.

The same candidate adds allowlisted Wi-Fi AP settings and verified SD repair
and format actions to Web Config. Invalid Wi-Fi updates and missing storage
confirmations returned HTTP 400. Applying the existing Wi-Fi settings restarted
only hostapd/dnsmasq and retained the camera PID. A real non-destructive repair
completed, remounted `/dev/mmcblk1p1`, restarted the camera, and resumed both
recorders. A real format was deliberately not run because the card contains
video. All eleven authenticated HTML routes returned HTTP 200, and Network and
Storage API responses were verified on target. Browser rendering was not
repeated because no local Playwright/Chromium runtime is currently installed.

The final package is `Upgrade/upgrade-0.4.3-debug.img` (79,186,506 bytes,
SHA-256
`2bf0a90616ef4e9cc558e729a083a965f9c8cd5e4334f18b175465716919c244`).
The packaged rootfs reports `0.4.3-debug`, contains no `ttyFIQ0` getty, and
includes the fixed Wi-Fi service and mode-0755 storage helper. Its stripped
`rv06_dashcam` SHA-256 is
`138bf9ee6bd82384245f501ef25dc991862d1ec7d4c1f452dab1248b440ebfb3` and
its stripped `rv06_webconfig` SHA-256 is
`bb8b8405a7c5f5d76cf80aca09a297003c2b1b4333d36d537419ee5ca15c00b1`.
The image is packaged but not yet flash-validated.

## Known Remaining Observations

1. CAM0 is no longer a black encoded frame, but its current scene is very dark.
   OV8858 exposure is 1120/1240 and analogue gain is 2047/2047. AIQ is already
   driving near/max sensitivity, so an obstructed lens, dark placement, glass,
   or illumination must be checked before changing ISP code. Exact optical cause
   is `CHUA XAC MINH`.
2. CAM1 no longer has the heavy purple cast or horizontal corruption. Its image
   is still soft under strong glare and appears to be viewed through a marked or
   reflective surface. Clean/reseat/inspect the module and flex before changing
   Bayer order, IQ, lane count, or Device Tree. Exact remaining focus cause is
   `CHUA XAC MINH`.
3. Boot logs contain `failed to set hflip (val: 1)` for OV8858 and two
   `failed to set hdr mode 0` messages. They did not stop AIQ, VI, VENC, or file
   growth. Do not suppress or reinterpret them without SDK and target evidence.
4. IP address is assigned by DHCP and can change even though the MAC is fixed.

## Important Implementation History

- Firmware before `0.3.3` could start a segment on a P-frame. The current code
  gates file creation on VPS/SPS/PPS/IDR, so raw H.265 segments are independently
  decodable.
- Do not add `u32Offset` to the pointer returned by
  `RK_MPI_MB_Handle2VirAddr()` in the current recorder. That caused target
  `EFAULT` and corrupt output. The current base pointer plus `u32Len` follows the
  LubanCat VENC sample and has been validated on target.
- Firmware `0.3.3` used AIQ camgroup to make both sensors emit frames, but the
  asynchronous heterogeneous sensors overflowed the group result map. Firmware
  `0.3.4` fixed this using two SDK-style multi-camera AIQ contexts and normal VI
  channel enable semantics.
- `stIspOpt.stMaxSize` must remain camera-specific: 1632x1224 for OV8858 and
  1920x1080 for GC2053.

## Build And Package

From SDK root:

```sh
make -C project/app/dashcam clean all
./build.sh app
./build.sh firmware
```

Set `FIRMWARE_VERSION` before packaging. `project/scripts/export_upgrade.sh`
publishes the final image as `Upgrade/upgrade-<version>.img` only after Rockchip
packaging succeeds.

## Target Validation Checklist

```sh
grep -E '\[DASHCAM|\[ISP|\[PIPE|\[VENC|\[REC' /tmp/dashcam/recorder.log
grep -Ei 'CAMGROUP|no free aiq|CIF_ISP|PIC_SIZE|ERROR' /tmp/dashcam/recorder.log
mount | grep /mnt/sdcard
df -h /mnt/sdcard
ls -lht /mnt/sdcard/DCIM/front /mnt/sdcard/DCIM/rear
dmesg | grep 'mipi-csi2-hw ERR'
```

Runtime claims require growing front and rear files, successful segment close,
and decoded visible frames. Logs alone are insufficient.

## Firmware 0.4.4 Candidate

The candidate removes the fixed `CAM1 REAR` text. CAM0 and CAM1 now render the
same vehicle/driver identity snapshot as `CH1 | <plate> | <driver>` and
`CH2 | <plate> | <driver>`, respectively. A driver revision updates both
existing main/sub OSD Region bitmaps without restarting the pipeline or opening
a new segment.

For debug validation without a working external UART RX path, a root-owned,
mode-0600 `/tmp/dashcam/driver-uart-test.json` file is consumed by the same DRV1
parser and produces `/tmp/dashcam/driver-uart-test.ack`. The path is gated by
`RV06_DRIVER_UART_TEST_ENABLED=on` in the debug launcher and must be removed for
production. The LubanCat ARM build passes with `-Werror`.

Packaging completed as `Upgrade/upgrade-0.4.4-debug.img` (79,186,506 bytes,
SHA-256
`8fafea8270d214aa4d0ae5d3104e382c91adc7ec83c089203cc26d6def127fef`).
The packaged stripped `rv06_dashcam` SHA-256 is
`e17b553c9f5035f0cb59901bf9d98e74266e292cb35097cd5e66f1458b63df92`.
Rootfs inspection confirmed the `CH1`/`CH2` labels, debug injection and ACK
paths, debug launcher gate, Web version `0.4.4-debug`, and no `ttyFIQ0` getty.
The packaged image was flashed and verified at `192.168.1.238`. Target hashes
matched the package. Both main encoders, both hardware substreams, four RTSP
sessions, and all four OSD Regions reported `READY`; current front/rear pending
files grew during the test and no CIF/ISP size, no-frame, assertion, or crash
error appeared.

A root-only DRV1 injection with `id=44001` returned `status=ok`. The camera PID
remained `478`, the same current front/rear segment paths continued growing,
and the main/sub Regions for both cameras logged `driver_bitmap_update=OK` at
revision 1. VLC-decoded target frames are saved as
`LOGS/osd_cam0_0.4.4.png` and `LOGS/osd_cam1_0.4.4.png`. They visibly confirm
transparent white `CH1 | NGUYEN VAN AN` and `CH2 | NGUYEN VAN AN`, unchanged
time/GPS/speed rows, and no `CAM1 REAR`. The vehicle plate was absent because
the active plate configuration was empty.

## JT808 And JT1078 Candidate For 0.4.5

The candidate adds an independent JT808-2013 control client and JT/T 1078-2016
TCP video sender without changing VENC0/VENC1 or `SegmentWriter`. The existing
recorder remains the target-validated raw H.265 `.h265` pipeline; it was not
converted to a container during this change.

JT808 implements `0x0100`, `0x8100`, `0x0102`, `0x8001`, `0x0001`, `0x0002`,
`0x0200`, `0x9101`, and `0x9102`, including `0x7e` framing, escaping, XOR,
partial/multiple TCP frames, inbound fragmentation, bounded duplicate command
tracking, timeouts, exponential reconnect, and active route/IP detection.
`AUTH_MODE=REGISTER_RESPONSE` persists the returned authentication code
atomically in `/oem/usr/etc/dashcam/jt808.conf` mode `0600`; `STATIC` is also
supported. Logs and Web Config expose only authentication length/set state.

The media endpoint is never fixed in firmware. Each `0x9101` supplies the host,
TCP/UDP ports, logical channel, data type, stream type and transport. Initial
scope accepts video/video-plus-audio requests over TCP but sends video only;
UDP and non-video modes are rejected. `0x9102` stops only the requested logical
channel. CAM0 is channel 1 and CAM1 is channel 2.

When JT808 is configured, both hardware sub encoders are forced to H.264
Annex-B 640x360 at 12 FPS, CBR 500 Kbps, GOP 12 and Normal-P without B-frames.
An open request triggers a VENC IDR request. JT1078 waits for SPS/PPS/IDR,
preserves each VENC access unit, and fragments it into payloads of at most 950
bytes with the `0x30316364` header. Each channel has an independent TCP worker
and a queue bounded to 24 frames, 1 MiB and 2 seconds. Overflow drops queued
video, retains codec configuration and requests a new IDR.

RTSP remains enabled and substream creation is no longer coupled to the RTSP
enable flag. A separate mobile Web Config route `/jt808` edits the CMSV6
identity and endpoint. `AUTH_CODE` is password-style input and is never returned
in clear by the API.

Target validation at `192.168.1.238` used an ARM mock CMSV6 process installed
only under `/tmp`. It completed registration, returned and persisted a 15-byte
authentication code, authenticated, opened/stopped both channels, repeated
duplicate `0x9101` commands, and repeated the full test after a control socket
disconnect. Both reconnect runs passed after duplicate tracking was correctly
scoped to one JT808 socket.

The final mock result was:

```text
CAM0: 21 JT1078 packets, SPS=1, PPS=1, IDR=1
CAM1: 20 JT1078 packets, SPS=1, PPS=1, IDR=1
MOCK PASS JT808_JT1078_CAM0_CAM1
```

During the test both H.265 recorders continued writing and RTSP remained on
port 554. After restoring an unconfigured default `jt808.conf`, the final
candidate PID recorded both cameras, started VENC2/VENC3 at the required CBR
settings, and reported no CIF/ISP size, no-frame, assertion, write or rename
error. Candidate target checksums are:

```text
rv06_dashcam:   f569c03921c764cf6af59cffdb87446928a18c25e560e5d47e62ccbd5cca0270
rv06_webconfig: f63f4c56aef3c26d7136dd1c63adcd90640eae49f7968dfd0df784efcb0f2386
```

Connection to the real CMSV6 instance, real server registration policy,
production terminal identity, Internet route switching between eth0/usb0,
long slow-network queue pressure and 24-hour endurance are `CHUA XAC MINH`.

Firmware packaging completed successfully:

```text
Upgrade/upgrade-0.4.5-debug.img
Size: 76 MiB
SHA256: fa684acdd1285b98c1cb2988852b2ca8e7d8f902dfe24031a229089adcbfc613
Packaged rv06_dashcam:   9159a84246dc1fa9242ca79fb9cb4a2d8d9ac09fec73d44ea7197794a5018c21
Packaged rv06_webconfig: 06df79555a171e2a58325823de737610098aed697562c2921feea44d1220a435
```

Rootfs inspection confirmed `jt808.conf` mode `0600`, the separate `/jt808`
page, version `0.4.5-debug`, and the final JT808/JT1078 symbols. This packaged
image is `CHUA XAC MINH` after flashing; the target results above are from the
same unstripped candidate deployed directly before firmware packaging.

## Real CMSV6 Validation And 0.4.6

The direct-deployed `0.4.6-debug` candidate was validated on the device at
`192.168.1.239` against `cam.tracking.vn:6608`. TCP reachability and DNS were
confirmed over Ethernet. The board registered with terminal phone
`874903786373`, persisted the 14-byte binary registration authentication value
as `HEX:<hex>`, authenticated successfully, acknowledged heartbeats and reached
`state=ONLINE`. The authentication value was not printed in logs or returned by
Web Config.

CMSV6 sent the standard JT/T 1078-2016 `0x9101` body without a separate
transport byte. The parser now accepts that standard layout, selects TCP for the
video-only implementation, and tolerates at most two trailing CMS extension
bytes. CMS supplied media endpoint `cam.tracking.vn:6631`. Runtime results:

```text
CAM0 / VENC2 H.264 -> JT1078 channel 1: connected, SPS/PPS/IDR, dropped=0
CAM1 / VENC3 H.264 -> JT1078 channel 2: connected, SPS/PPS/IDR, dropped=0
Recorder CAM0/CAM1: RECORDING while both JT1078 channels were active
JT808 status: ONLINE, heartbeat response result=0
```

Repeated `0x9101` commands from CMS replace only the requested media session;
they do not restart the camera pipeline or recorder. The current implementation
sends video only even when CMS requests data type `0` (audio and video), as
audio is outside the approved initial scope.

Web Config `/jt808` was tested through a real authenticated browser API session.
It shows per-field Vietnamese provisioning guidance, channel/codec settings and
live `ONLINE` state while keeping `AUTH_CODE` secret. The debug identity in the
default configuration belongs only to this board. Every production device must
be provisioned in CMSV6 with a unique `TERMINAL_PHONE` and `TERMINAL_ID`.

Firmware packaging completed successfully:

```text
Upgrade/upgrade-0.4.6-debug.img
Size: 76 MiB
SHA256: 09ee6536fd9bb79ef53bc806568a2457933fbb060f142ad3ea5a1f3fc052036e
Packaged rv06_dashcam:   dc524a7978c6fb73bba820780b2a14839c3a6efd8cc16596f35231cd131e7701
Packaged rv06_webconfig: e07bf01d6f03ab1675508a520aed211afe11d10a59975a67c6af5d6f49676dc2
```

The exact image was flashed and reboot-tested at `192.168.1.240`. Target hashes
for both ARM binaries match the packaged hashes above. Web status reported
firmware `0.4.6-debug`, CAM0/CAM1 online, both main and sub encoders advancing,
both recorders in `RECORDING`, GPS fix, RTSP ready and JT808 `ONLINE`. The 128 GB
SD card was mounted read/write from `/dev/mmcblk1p1`; both current H.265 files
grew during a four-second measurement. No CIF/ISP size, no-frame, write, rename,
allocation, OOM or crash error was found.

After startup, two JT1078 TCP sockets were established to the CMS-provided port
6631. Both channels received SPS/PPS/IDR, queues stayed at zero and reported
`dropped_frames=0`; JT808 heartbeat acknowledgements returned result zero. At
the sampled load, CPU was 46% idle and Linux reported approximately 16 MB
available RAM. `CmaFree` was zero with all 64 MB reserved CMA in use, so DMA/CMA
headroom remains a long-run risk to monitor even though no allocation failure
or pipeline error was present. Remaining validation is CMS display/playback
confirmation, route failover and the 24-hour endurance run.

## JT1078 Header Compatibility Fix For 0.4.7

Real packet capture showed that `0.4.6-debug` sent the JT1078 fixed header as
`80 62`. This was invalid for JT/T 1078-2016 because the four-bit RTP `CC`
field is fixed to one, and the marker bit must identify an atomic packet or the
last fragment. CMS acknowledged TCP bytes but repeatedly issued `0x9101` while
the Web player showed no video.

`0.4.7-debug` sends byte 4 as `0x81` (`V=2`, `P=0`, `X=0`, `CC=1`) and sets
`M=1` only for an atomic packet or final fragment. Payload type remains H.264
dynamic type 98. The target at `192.168.1.240` was direct-deployed through the
normal launcher. Packet capture confirmed live headers beginning
`30 31 63 64 81 e2`, correct BCD SIM `874903786373`, and logical channels 1/2.
Both media sockets stayed connected, both streams sent SPS/PPS/IDR and both
H.265 recorders continued writing after the controlled restart.

Firmware packaging completed successfully but this exact image has not yet
been flashed:

```text
Upgrade/upgrade-0.4.7-debug.img
SHA256: e3a020e78c1973b0084f3d5c78c20ef203514c24f3a338ffdfe0be8f4f0631d2
```

CMSV6 playback must be checked with protocol type JT808/JT808-2013 (video
terminal with JT1078 support), not JT808-2019. Direct deployment verifies the
wire format but does not replace visual confirmation in the CMS player.

## Stream OSD Scale Fix For 0.4.8

The main record streams remain unchanged at 1280x720 with a 608x96 OSD bitmap.
The 640x360 H.264 sub/JT1078 streams now use a 304x48 bitmap generated from the
same design canvas, preserving the same relative OSD size instead of covering
almost the full stream width. Main and sub Region handles remain persistent;
the change does not restart VI, ISP, VENC, recorder or JT1078 when text updates.

Direct deployment at `192.168.1.240` confirmed:

```text
VENC0 CAM0 main: handle=16 size=608x96 scale_ratio=1:1
VENC1 CAM1 main: handle=17 size=608x96 scale_ratio=1:1
VENC2 CAM0 sub:  handle=18 size=304x48  scale_ratio=1:2
VENC3 CAM1 sub:  handle=19 size=304x48  scale_ratio=1:2
```

Both sub streams were captured through RTSP at their real 640x360 resolution.
CAM0 and CAM1 showed centered, transparent OSD with the intended proportional
text size. Both H.265 recorders remained in `RECORDING`; both JT1078 channels
continued sending H.264 frames with zero dropped frames and JT808 stayed
`ONLINE` during validation.

Firmware packaging completed successfully:

```text
Upgrade/upgrade-0.4.8-debug.img
SHA256: a1e28e05322c113b8cb411b127461eb8363ac9668b74542c2324c3c45a2e2ce4
```

## JT808 Driver Status Synchronization For 0.4.9

Accepted DRV1 login/logout revisions now generate the standard JT808-2013
driver identity report `0x0702`; CMS `0x8702` requests also return the current
state. The authenticated `/jt808` Web Config status shows login state, driver
name, license number and synchronization result. Logout clears identity fields.

The candidate was direct-deployed at `192.168.1.241`. A login for
`NGUYEN VAN AN` revision 1 was sent as `0x0702` sequence 7 and CMSV6 returned
result 0. Logout revision 2 was sent as sequence 12 and also received result 0.
Web status changed from `LOGGED_IN` with the name/license to `LOGGED_OUT` with
both fields empty. The camera PID stayed at 1615 and the same front/rear pending
segment files remained open throughout both events. Both recorders and both
JT1078 channels continued advancing with zero reported packet drops.

Firmware packaging completed successfully:

```text
Upgrade/upgrade-0.4.9-debug.img
SHA256: 6cde53241bd9188e22cdec509c7ff2db1590d3e787289ddf46c498278dd71267
```

## Persistent Configuration For 0.5.0

The 256 MiB SPI NAND layout now uses the previously unallocated final 768 KiB:

```text
256K(env),1M@256K(idblock),1M(uboot),5M(boot),248M(rootfs),768K(dashcfg)
```

`dashcfg` is mounted as JFFS2 at `/persist`, while applications use the stable
symlink `/run/rv06-config -> /persist/dashcam`. Mutable files include
`runtime.conf`, `jt808.conf`, Web password hash, Wi-Fi AP configuration and
their atomic backups. `/oem/usr/etc/dashcam` contains firmware defaults only.

`mk-update_pack.sh` explicitly skips `dashcfg`. The generated package payload
contains only `env`, `idblock`, `uboot`, `boot`, and `rootfs`, so normal FW and
FOTA updates cannot overwrite the persistent configuration partition. Startup
formats `dashcfg` only when a 2 KiB NAND read succeeds and every byte is
`0xff`; a non-blank mount failure falls back to firmware defaults without
erasing data.

Transitioning a device from 0.4.x requires running
`/oem/usr/bin/rv06_config_migrate_prepare.sh` before flashing. It copies the
allowlisted current configuration to a verified read/write SD card and writes
a SHA256 manifest. The first 0.5.0 boot imports only a valid manifest, commits
the persistent store, then removes the migration bundle. On the current target
the SD kernel node `/dev/mmcblk1p1` disappeared while a stale mount entry
remained, so migration correctly returned `SD_NOT_VERIFIED`; do not flash this
first transition until the SD is detected and migration reports `state=READY`.
As a recovery fallback, the current device configuration was copied over SSH
to the owner-only local directory
`Upgrade/config-backup-0.4.9-192.168.1.241` with a SHA256 manifest. This backup
contains credentials and must not be committed or distributed.

The rootfs, app, and firmware builds completed successfully. Image inspection
confirmed the new MTD layout, executable startup/migration scripts, matching
source/staging checksums, and no `dashcfg` entry in the update package file.
Runtime preservation across an actual update remains `CHUA XAC MINH` until the
migration, flash, config-change, and second-update test are completed.

```text
Upgrade/upgrade-0.5.0-debug.img
SHA256: 1c97bb97d16ab5a8aecd3b7f8346de4da8f913bb49d146b645d17d0c01e60694
```

## JT1078 No-SD Recovery For 0.5.1

Post-flash validation of 0.5.0 at `192.168.1.242` found JT808 online and CMSV6
issuing valid TCP `0x9101` commands for channels 1 and 2. Both sockets connected
to port 6631, but every session remained at zero frames because no SD kernel
node existed. The substream startup gate counted only packets successfully
written by the recorder, so `WAITING_FOR_SD` permanently caused
`DISABLED_MAIN_NOT_STABLE`.

The 0.5.1 candidate keeps the recorder-first gate when storage is writable. If
the verified SD is unavailable, it instead waits for 300 valid packets from
each H.265 main encoder before allocating the hardware H.264 substreams. The
recorder counters remain separate and continue to report zero while no SD is
present.

Direct deployment with exactly one camera process confirmed VENC2/VENC3 at
640x360, 12 FPS, 500 Kbps CBR and successful SPS/PPS/IDR delivery. During the
measured interval channel 1 reached 606 frames and 854,926 bytes; channel 2
reached 603 frames and 833,206 bytes. Both queues remained empty,
`dropped_frames=0`, both media sockets stayed established, and JT808 heartbeat
ACK returned result zero. CMS did not repeat `0x9101` after video started.

The 0.5.0 `dashcfg` partition also proved too small on this physical NAND:
four of its six 128 KiB erase blocks are bad, leaving JFFS2 at 100% usage and
preventing initialization. The 0.5.1 layout reduces the largely empty rootfs
partition from 248 MiB to 240 MiB and expands `dashcfg` to 8960 KiB:

```text
256K(env),1M@256K(idblock),1M(uboot),5M(boot),240M(rootfs),8960K(dashcfg)
```

The running rootfs uses 63.2 MiB, leaving substantial room within 240 MiB.
Persistent-store migration on the resized partition remains `CHUA XAC MINH`
until 0.5.1 is flashed and the protected configuration backup is restored.

Firmware packaging completed successfully. The package still omits `dashcfg`:

```text
Upgrade/upgrade-0.5.1-debug.img
SHA256: c2d03c3da0ae101782bb90f683804cdc677e2cefa9e8f2d81ec827ff7fb08d5a
```
