# Test plan

Phase 1 verifies sensor probe, stable native capture, format/FPS, and repeated
camera restart. Phase 2/3 verifies VENC/RTSP startup, client disconnect and
reconnect, and network interruption. Later AI tests measure latency, AI FPS,
RAM, CPU, NPU, temperature, known/unknown outcomes, and false accept/reject.

Long-running acceptance includes 1/6/12/24-hour memory snapshots, RTSP
reconnects, AI restart, camera restart, Web reload, database access, and SD
insert/removal. No performance result is claimed before it is measured on the
SC3336 target.
