# Facial Recognition & Enrollment Camera

Production-oriented facial-recognition camera component for Rockchip RV1106,
SC3336 MIPI CSI-2, and RV06_03_Linux_SDK.

## Current status

- **Camera & RTSP**: SC3336 capture, RKISP, VENC H.264/H.265 streaming on `rtsp://DEVICE_IP:554/live/0`.
- **WebConfig**: Port 80 HTTP server with Basic Auth (`root`).
- **AI Face Recognition**:
  - YOLOv5n-Face NPU detection (640x640) with 5 facial landmarks.
  - Face alignment, quality filtering, and 128-D feature extraction.
  - Real-time Cosine Similarity matching against Face Database.
  - Interactive WebConfig enrollment for adding persons in front of the camera and displaying their name.

## RTSP

Manage RTSP on target:
```sh
/oem/usr/bin/fr-rtsp-service status
/oem/usr/bin/fr-rtsp-service restart
tail -f /var/log/fr-rtsp.log
```

## AI Recognition & Enrollment

Manage AI service on target:
```sh
/oem/usr/bin/fr-ai-service.sh status
/oem/usr/bin/fr-ai-service.sh restart
tail -f /var/log/fr-ai.log
```

## WebConfig & API

Open `http://DEVICE_IP/` in your browser.

- **Dashboard**: Real-time status of Camera, RTSP, Ethernet IP, and currently recognized person in front of the camera.
- **AI Recognition**:
  - Real-time face recognition monitor (Verified Name vs. Unknown Person).
  - Add/Enroll person in front of camera with custom name.
  - Manage enrolled persons list (View/Delete).
- **Camera & Streaming**: Configure encoder resolution, bitrate, and codec.

### API Summary:
- `GET /api/v1/status`
- `GET /api/v1/stream/status`
- `PUT /api/v1/stream/config`
- `GET /api/v1/ai/status`
- `GET /api/v1/ai/persons`
- `POST /api/v1/ai/enroll`
- `DELETE /api/v1/ai/persons`
