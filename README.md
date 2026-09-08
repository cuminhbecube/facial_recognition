# RV06 Dashcam SDK

Firmware/source tree for the LubanCat-RV06 (RV1106, SPI-NAND) dual-camera
recorder. The production product is **BECAM-2 = RV1106 + PR2100K dual AHD +
EC800M-CN with GNSS**. The project keeps the hardware camera pipeline, recorder,
JT808/JT1078 media transport, cellular/GNSS management, RFID/GPLX reader, OSD and web
configuration in one reproducible SDK build.

## Hardware and runtime layout

| Logical channel | AHD input | Decoder path | VI/pipe | Recording directory |
|---|---|---|---|---|
| CAM0 / CH1 | Front / VIN0 | PR2100K VC0 | 0/0 | `/mnt/sdcard/DCIM/front` |
| CAM1 / CH2 | Rear / VIN1 | PR2100K VC1 | 1/1 | `/mnt/sdcard/DCIM/rear` |

PR2100K is controlled on I2C4 at `0x5f` and emits both channels as YUV422 over
one two-lane MIPI CSI-2 link. Its reference crystal is 27 MHz. The direct
OV8858/GC2053 topology is legacy.

Recording uses hardware `VI -> VENC`; the H.265 files are written through a
temporary file, flushed and atomically renamed only after a valid non-empty
packet is received. The H.264 substreams are used for JT1078/CMSV6 live video
and RTSP. The main H.265 recordings are used for local playback/search.

The current firmware version is recorded in [`FIRMWARE_VERSION`](FIRMWARE_VERSION).
Mutable settings are stored under `/run/rv06-config` on the dedicated `dashcfg`
partition. They are deliberately not replaced by normal firmware/FOTA updates.

## Services and protocols

### JT808 / CMSV6

The control connection authenticates to the configured CMS endpoint and keeps
heartbeats/location reports active. Supported paths include device status,
remote snapshot (`0x8801 -> 0x0805`), playback search (`0x9205 -> 0x1205`) and
playback session setup (`0x9201`). Endpoint, channel, transport and stream
parameters come from the CMS command; media endpoints are never hard-coded.

### Four-VENC Architecture, JT1078 and RTSP

All four hardware encoder channels run simultaneously on the RV1106 within the 64 MiB CMA budget:
- **VENC0**: CAM0 Main (H.265, up to 1080p@20 / 720p@15) $\to$ SD Front Recording + RTSP CAM0 Main
- **VENC1**: CAM1 Main (H.265, up to 1080p@20 / 720p@15) $\to$ SD Rear Recording + RTSP CAM1 Main
- **VENC2**: CAM0 Sub (H.264, D1 / 360p / CIF, Bitrate 200–1000 Kbps) $\to$ JT1078 Ch1 + RTSP CAM0 Sub
- **VENC3**: CAM1 Sub (H.264, D1 / 360p / CIF, Bitrate 200–1000 Kbps) $\to$ JT1078 Ch2 + RTSP CAM1 Sub

Live video is sent to CMSV6 when requested with `0x9101` and stopped by `0x9102`.
RTSP and JT1078 share VENC2/VENC3 fanout without allocating redundant encoders.
All 4 RTSP endpoints support TCP interleaved streaming:

```text
rtsp://<device-ip>:554/live/cam0/main (H.265 Front Main)
rtsp://<device-ip>:554/live/cam0/sub  (H.264 Front Sub)
rtsp://<device-ip>:554/live/cam1/main (H.265 Rear Main)
rtsp://<device-ip>:554/live/cam1/sub  (H.264 Rear Sub)
```

### EC800M-CN cellular and GNSS

The production modem target is an EC800M-CN hardware variant with GNSS. The
generic `CellularModemManager` and `GnssManager` discover the module, AT port,
optional NMEA port and Linux data interface instead of relying on fixed
`ttyUSB` numbers or `usb0`. The GNSS engine remains independent of OSD
visibility; turning off GPS OSD does not stop JT808 positioning or the RFID GPS
LED state.

EC800M-CN GNSS is optional by hardware variant. Exact SKU/revision, USB
VID/PID, port layout and GNSS method are `CHUA XAC MINH` until `ATI`,
`AT+CGMM`, `AT+CGMR` and GNSS capability probes pass on the physical module.
EC25 remains a legacy compatibility profile; EG800AK is not a production
target.

The discovery, migration and validation contract is documented in
[`project/app/dashcam/docs/CELLULAR_GNSS.md`](project/app/dashcam/docs/CELLULAR_GNSS.md).

### RFID/GPLX reader

The reader is `/dev/ttyS3`, `115200 8N1`. It is event-driven: the production
manager does not poll `READ_CARD` every 250 ms. A persistent byte-stream parser
accepts partial frames, multiple frames and unsolicited card events while the
RX path remains active during ACC OFF. Duplicate cards are debounced for 500 ms.

Card actions are:

```text
new card       -> LOGIN
same active    -> LOGOUT
different card -> SWITCH
```

The six physical indicators are POWER, NET, GPS, DRIVER, REC and BUZZER. The
reader protocol value `0x13` is hardware FLASH; firmware does not implement a
software blink loop. The 16-byte IO_CONTROL packet is:

```text
7E 03 00 0B [BUZZER] [POWER] [NET] [GPS] [DRIVER] [REC]
   [0x00] [0x00] [0x00] [0x00] [0x00] [CHECKSUM]
```

The unused Alarm/CAM0..CAM3 fields are always zero. LED priority is:

| LED | OFF | FLASH (`0x13`) | ON |
|---|---|---|---|
| POWER | ACC/system stopped | ACC startup or shutdown transition | ACC on and initialized |
| NET | unavailable/no IP | connecting or reconnecting | CMS/JT808 authenticated |
| GPS | GNSS unavailable | engine alive, searching/stale fix | current valid fix |
| DRIVER | no driver | short invalid/rejected-card indication | driver logged in |
| REC | recorder stopped | startup, flush, degraded/error/storage fault | healthy recording |
| BUZZER | normal | not used | 100–150 ms pulse for accepted card/event |

LED state is sent only when the complete desired packet changes. A state
transition never flushes the RFID RX buffer, so push card frames are not lost.
Detailed UART and DRV1 notes are in
[`project/app/dashcam/docs/DRIVER_UART.md`](project/app/dashcam/docs/DRIVER_UART.md)
and [`project/app/dashcam/docs/rfid_protocol.md`](project/app/dashcam/docs/rfid_protocol.md).

### OSD and Web Config

OSD is rendered into the existing hardware VENC regions. It can show time,
GPS, speed, vehicle plate and driver identity independently. CAM0 uses `CH1`
and CAM1 uses `CH2`; driver updates refresh existing regions without restarting
the recorder. Web Config serves port 80 on LAN/Wi-Fi AP and stores validated
configuration atomically.

## Build

Install the SDK dependencies (Ubuntu/Debian), source the Rockchip toolchain,
then build from the SDK root:

```sh
source tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/env_install_toolchain.sh
./build.sh lunch BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
make -C project/app/dashcam clean all
./build.sh app
./build.sh firmware
```

The application binary is `project/app/dashcam/rv06_dashcam`. The complete
Rockchip package is `output/image/update.img`; the versioned copy is
`Upgrade/upgrade-<version>.img`. Verify a package before flashing:

```sh
sha256sum output/image/update.img Upgrade/upgrade-*.img
tools/linux/Linux_Pack_Firmware/mk-update_unpack.sh \
  -i output/image/update.img -o /tmp/rv06-update-check
```

Do not flash a package that has not passed this unpack and checksum check.

## Target validation

Runtime claims require target evidence, not only a host build. Check:

```sh
grep -E '\[DASHCAM|\[ISP|\[PIPE|\[VENC|\[REC' /tmp/dashcam/recorder.log
grep -E 'RFID_LED|RFID_RX|RFID_CARD|RFID_DUP|RFID_STAT' /tmp/dashcam/recorder.log
mount | grep /mnt/sdcard
df -h /mnt/sdcard
ls -lht /mnt/sdcard/DCIM/front /mnt/sdcard/DCIM/rear
```

Confirm JT808 `state=ONLINE`, growing front/rear recordings, and independent
CAM0/CAM1 frame counters. Flashing requires Rockchip Loader/Maskrom mode:

```sh
tools/linux/Linux_Upgrade_Tool/rkdownload.sh -d output/image
```

The project does not include device credentials. Use the approved deployment
procedure for the target and never commit passwords, tokens or private keys.

## Repository hygiene

Tracked source, documentation, scripts and configuration are the project of
record. Build output, SDK media build directories, generated images, temporary
logs, editor swap files and local deployment helpers are ignored or kept out of
commits. Do not delete `dashcfg` data, target backups or firmware images that
are needed for rollback without an explicit release/retention decision.
