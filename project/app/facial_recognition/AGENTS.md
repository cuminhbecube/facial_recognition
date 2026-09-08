# Facial Recognition component

This directory is a dedicated Facial Recognition application for RV1106 and a
single SC3336 MIPI CSI-2 sensor.  It is deliberately separate from the
repository's dashcam application.

Before changing camera or media code, verify the active board DTS, sensor
endpoint, I2C bus/address, GPIOs, and runtime media topology.  Do not copy
dashcam, GPS, JT808/JT1078, recording, or vehicle logic into this component.

Use APIs and build integration already present in this RV06_03_Linux_SDK only.
Document anything not verified from this SDK or the target board as **NOT
VERIFIED**.

## WebConfig rules

- Preserve working Basic `root` authentication, RTSP persistence, restart, and boot startup.
- Derive all metrics and service state from the target; never fake a status or hard-code an interface.
- Validate every API request before writing configuration. Use atomic writes and retain a rollback backup.
- Never pass unvalidated Web request data to a shell command and never store passwords in frontend assets.
- WebConfig must remain reachable when camera, RTSP, or future AI services fail.
- Do not implement firmware upgrade until the RV06 SDK upgrade mechanism is verified.
- Do not claim an RTSP URL embedded in a browser is a live-view implementation. Keep additions lightweight for RV1106.
