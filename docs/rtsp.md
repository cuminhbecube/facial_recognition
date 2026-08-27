# RTSP

The desired endpoints are `/live/main` and optional `/live/sub` on port 554.
Defaults remain targets, not established capabilities: main 1920x1080, 25 fps,
H.265, 3072 Kbps, GOP 50 CBR; sub 640x360, 15 fps, H.264, 512 Kbps.

Use the locally shipped RV1106 `simple_vi_bind_venc_rtsp.c` and RK MPI VENC
headers to establish the real API/lifecycle. This component will check every
MPI return value and publish stream health (clients, uptime, encoded/dropped
frames) through `/api/v1`; no RTSP implementation has been added yet.
