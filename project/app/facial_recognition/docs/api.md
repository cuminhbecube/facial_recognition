# REST API Reference

WebConfig serves a versioned `/api/v1` REST API on port 80 with HTTP Basic Authentication (`root`).

## Endpoints

### 1. System & Stream
- `GET /api/v1/status`: Comprehensive status of hardware, Ethernet, camera, RTSP, and AI service.
- `GET /api/v1/system/status`: Lightweight system health status.
- `GET /api/v1/stream/status`: Current RTSP streaming status and endpoint URL.
- `GET /api/v1/stream/config`: Current video encoder configuration.
- `PUT /api/v1/stream/config`: Update resolution (`width`, `height`), `bitrate_kbps`, and `codec` (`h264`/`h265`).
- `POST /api/v1/stream/{start,stop,restart}`: Control RTSP streaming service.

### 2. AI Face Recognition & Enrollment
- `GET /api/v1/ai/status`: Current recognition state (detected person name, ID, similarity score, bounding box, FPS, faces count).
- `GET /api/v1/ai/persons`: List all enrolled persons (`id`, `name`, `created_at`).
- `POST /api/v1/ai/enroll`: Enroll person currently standing in front of camera (`name=Nguyễn+Văn+A` or JSON `{"name":"..."}`).
- `DELETE /api/v1/ai/persons?id={id}`: Delete enrolled person by ID.
