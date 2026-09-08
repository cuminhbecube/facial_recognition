# BECAM-2 Production Test Plan

## Target baseline

```text
BECAM-2 = RV1106 + PR2100K dual AHD + EC800M-CN with GNSS
```

Active source: firmware `0.6.1`. Validated on target `192.168.9.139`.

## Stage 1 — PR2100K, Four-VENC, RTSP, and dual recording — PASS

1. PR2100K on I2C4 `0x5f`, chip-ID `0x2100`, VIN0/VIN1 lock: **PASS**
2. Dual AHD 1080p25 capture on `/dev/video0` and `/dev/video1`: **PASS**
3. Simultaneous Four-VENC hardware encoding (VENC0-3): **PASS**
4. Dual H.265 SD recording to `/mnt/sdcard/DCIM/front` and `/mnt/sdcard/DCIM/rear`: **PASS**
5. All 4 RTSP Streams over TCP interleaved: **PASS**
6. JT1078 dual live video on logical channels 1 & 2: **PASS**
7. Configurable Main resolutions (1080p/720p/D1/360p/CIF) and Sub resolutions (D1/360p/CIF): **PASS**
8. Configurable Substream bitrates (200, 350, 500, 800, 1000 Kbps): **PASS**
9. D1 constraint guard: **PASS**
10. 600-second four-VENC stress test with 0 frame drops, 0 leaks, 0 crashes: **PASS**

Expected mapping:

| Item | CAM0 | CAM1 |
| --- | --- | --- |
| AHD input | VIN0/front | VIN1/rear |
| MIPI VC | VC0 | VC1 |
| VI dev/pipe | `0/0` | `1/1` |
| Main VENC | `0` H.265 | `1` H.265 |
| Sub VENC | `2` H.264 | `3` H.264 |

## Stage 2 — EC800M-CN modem

Do not identify the modem from a fixed tty number. Save USB enumeration and
the common USB parent for every discovered AT, GNSS/NMEA and network interface.

Run and retain the responses to:

```text
AT
ATI
AT+CGMM
AT+CGMR
AT+CPIN?
AT+CCID
AT+CGSN
AT+CSQ
AT+COPS?
AT+CGATT?
```

Verify SIM ready, IMEI, ICCID, operator, registration, packet attach, configured
APN, PDP activation/IP, detected Linux data interface, host route and Internet
access. Do not assume `usb0` or reuse an EC25-specific reset command.

## Stage 3 — EC800M-CN GNSS capability

GNSS is optional on EC800M-CN. Probe the physical module with supported-query
commands such as `AT+QGPS=?`, `AT+QGPS?` and `AT+QGPSLOC=?` before enabling the
production profile.

Verify cold start outdoors, `NO_FIX -> FIX`, latitude, longitude, UTC, speed
and satellite count when supported. LTE registration is not evidence of a
GNSS fix. If capability is absent, report `GNSS_SUPPORTED=NO`; never fabricate
GPS data.

## Stage 4 — integrated system

With both recorders active, run LTE Internet + GNSS FIX + JT808 + snapshot +
JT1078 CAM0/CAM1 concurrently. Verify:

- JT808 registration/authentication, heartbeat ACK, `0x0200` from the single
  `GnssManager` snapshot and driver `0x0702`;
- CMS snapshot, live stream, stop command, playback search/session and RTSP;
- OSD/Web GPS visibility without turning off the GNSS engine;
- bounded queues, no recorder stalls, no AT deadlock and no cross-feature
  failure propagation;
- LTE recovery rediscovers AT/data interfaces and restores GNSS afterward.

Run an 8–24 hour endurance test and track reconnects, fixes, satellites,
JT808 ACKs, RAM, FDs, CPU and USB/MIPI errors.

## Identity and persistence checks

When replacing EC25 with EC800M-CN:

- derive Device ID from the last 12 valid IMEI digits and canonical Wi-Fi
  suffix from the last 6;
- clear registration/authentication state tied to the previous terminal ID;
- keep ICCID tied to the SIM;
- preserve plate, company, camera/recording/snapshot/OSD/audio/ACC settings,
  CMS endpoint and user Wi-Fi policy;
- confirm normal firmware does not overwrite `dashcfg` or `gshtdata`.

EC25 remains a legacy compatibility profile until every integrated EC800M-CN
stage passes. EG800AK is not part of the production test matrix.
