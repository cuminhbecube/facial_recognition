# BECAM-2 Hardware Camera Mapping

## Production topology — firmware 0.6.1

```text
AHD front -> PR2100K VIN0 -> MIPI CSI-2 VC0 -> CIF/VI 0:0 -> VENC0 (H.265 Main) / VENC2 (H.264 Sub)
AHD rear  -> PR2100K VIN1 -> MIPI CSI-2 VC1 -> CIF/VI 1:1 -> VENC1 (H.265 Main) / VENC3 (H.264 Sub)
```

| Role | Input | Decoder control | MIPI output | Virtual channel | VI dev/pipe | Main/sub VENC |
| --- | --- | --- | --- | --- | --- | --- |
| CAM0/front | AHD 1080p25/30 on VIN0 | PR2100K, I2C4 `0x5f` | YUV422, D-PHY1, 2 lanes | VC0 | `0/0` | `0 / 2` |
| CAM1/rear | AHD 1080p25/30 on VIN1 | PR2100K, I2C4 `0x5f` | YUV422, D-PHY1, 2 lanes | VC1 | `1/1` | `1 / 3` |

The two AHD channels share one physical MIPI link but remain separate logical
VI, buffer-pool and encoder paths. Main recording uses hardware `VI -> VENC`;
the application does not decode the main stream or identify a camera by a
hard-coded `/dev/videoX` number. Substreams branch to VENC2/VENC3 through dedicated RGA
NV12 scaling pools with zero CPU copy overhead.

PR2100K uses its own 27 MHz reference crystal. Device Tree describes that
external oscillator as a 27 MHz fixed clock; RV1106 must not drive
`MCLK_REF_MIPI0` into the crystal circuit.

`RV06_CAMERA_INPUT=ahd_pr2100k` is the production input. All four VENC encoders
(VENC0-3) operate simultaneously within the standard 64 MiB CMA budget (~38.8 MiB
allocated, 25.2 MiB safe headroom).

## Target Verification Matrix — PASS

- PR2100K I2C4 `0x5f`, chip ID `0x2100`: **PASS**
- PR2100K dual AHD VIN0/VC0 + VIN1/VC1 1080p25: **PASS**
- MIPI CSI-2 2-lane 1188 Mbps capture on `/dev/video0` and `/dev/video1`: **PASS**
- Simultaneous 4-channel hardware encoding (VENC0-3): **PASS**
- Dual H.265 SD recording: **PASS**
- 4x RTSP streams over TCP interleaved: **PASS**
- JT1078 dual live stream on logical channels 1 & 2: **PASS**
- 600-second continuous four-VENC stress test with 0 frame drops and 0 leaks: **PASS**

## Legacy history

OV8858 on I2C4 `0x36` and GC2053 on I2C3 `0x37` were used by firmware before
the 0.6.0 AHD migration. Their RAW Bayer modes, IQ files and independent AIQ
contexts remain relevant only to the explicit legacy fallback.
