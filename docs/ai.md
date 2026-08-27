# AI Face Recognition Pipeline

## Architecture & Implementation

The AI pipeline is implemented as an independent high-performance module designed for Rockchip RV1106:

```text
[ Camera Frame (640x640 RGB) ]
             │
             ▼
[ YOLOv5n-face Detector (NPU RKNN) ]
  • Input: [1, 640, 640, 3] NHWC UINT8
  • Outputs: 3 detection pyramids (80x80, 40x40, 20x20)
  • Decodes Bounding Boxes & 5 Facial Landmarks (2 eyes, nose, 2 mouth corners)
  • Non-Maximum Suppression (IoU >= 0.45)
             │
             ▼
[ Face Alignment & Quality Assessment ]
  • Rejects blurred, tilted (>35 deg), edge-clipped, or small (<50px) faces
  • Normalizes and crops face patch (64x64 / 112x112)
             │
             ▼
[ Feature Extraction & Cosine Similarity Matcher ]
  • 128-dimensional L2-normalized feature vector (geometry + spatial descriptors)
  • Cosine Similarity comparison against Face Database (`database.json`)
  • Similarity Threshold: 0.70 (>= 0.70 -> Matched Person, < 0.70 -> Unknown / Unregistered)
             │
             ▼
[ Face Database Management & WebConfig ]
  • JSON persistence with atomic writes at `/oem/usr/etc/facial-recognition/database.json`
  • Real-time recognition monitor on WebConfig Dashboard
  • Interactive Enrollment: capture face in front of camera with custom name
```

## Management & Commands

On target device:
```sh
# Check AI daemon status
/oem/usr/bin/fr-ai-service.sh status

# Start / Restart AI daemon
/oem/usr/bin/fr-ai-service.sh restart

# Real-time state check
cat /tmp/fr_ai_state.json
```
