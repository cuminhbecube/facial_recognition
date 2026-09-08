# Facial Recognition Firmware — Current Status

Cập nhật: 2026-09-08. Firmware **0.6.2** — nền Rockchip RV1106, bố trí Luckfox
Pico Pro Max (SPI-NAND), camera đơn SC3336 MIPI CSI-2, Ethernet có dây.

Repository này là cây SDK hợp nhất (sysdrv / media / tools / app) dùng để dựng
firmware nhận diện khuôn mặt chuyên dụng. Xem [`README.md`](README.md) để biết
cách build và cấu trúc thư mục.

## Baseline đã xác minh trên thiết bị

Thiết bị tham chiếu: `192.168.1.230`, services`/oem/usr/bin` (partition `oem`,
UBIFS), log tại `/var/log/fr-*.log`.

- `fr-media-service` (media + RTSP+ lưu trữ SD) và `fr-webconfig` (HTTP :80,
  Basic Auth `root`) xác minh chạy ổn định; mọi cấu hình lưu qua web đều viết
  `rtsp.conf` và khởi động lại dịch vụ media an toàn (web sống sót qua restart).
- **Tự động dọn lưu trữ SD**: đoạn video chia theo ngày; xóa theo *giữ tối đa
  N ngày* và/hoặc *ngưỡng dung lượng trống* (MB). Áp dụng qua `-D <ngày>` /
  `-F <MB>`; mặc định `RETENTION_DAYS=0`, `FREE_SPACE_MB=500`.
  - Sửa segfault khi truyền `-F` (thiếu `:` trong getopt optstring → `atoi(NULL)`).
  - Tránh rò rỉ socket `:80` sang tiến trình con (FD_CLOEXEC) làm web chết.
- **Web UI tích hợp** (tiếng Việt): dashboard, cấu hình camera/stream, quản lý
  AI và mục *Lưu trữ quay phim (tự động dọn)*.

Hình ảnh upgrade: `Upgrade/upgrade-0.6.2.img`
(`sha256 61b0cb22e5a6b535712a925856e6d8edadf47752b1018ac848938fcad34f83e7`);
binaries đã đồng bộ vào `output/out/app_out` và `output/out/oem`.

## Nhận diện khuôn mặt

- Detector **YOLOv5n-Face** trên NPU (640x640, kèm landmark) đã được kiểm tra
  trên target (`docs/detector-verification.md` trong component).
- Recognizer hiện dùng **descriptor 128-D thủ công** (landmark + texture), chưa
  phải embedding từ model học sâu.
- WebConfig có **enrollment**: thêm / xem / xóa người, so khớp Cosine tức thời
  với database cục bộ.
- Lộ trình cải tiến (embedding học sâu, alignment chuẩn, calibration
  FAR/FRR, phiên đăng nhập 1:1, liveness) theo
  `project/app/facial_recognition/docs/implementation-plan.md`.

## Trạng thái & công việc kế tiếp

Đã xong: P1.1 cách ly component + P1.2 tuyến camera. Chờ triển khai:

| ID | Công việc |
| --- | --- |
| P2 | Tránh trùng ID trước khi enroll (dedup) |
| P3.1 | Snapshot chụp qua web khi phát hiện mặt |
| P3.2 | Webhook thông báo khi có người mới / người quen |
| P4 | Vệ sinh repo và tài liệu cuối |

Chi tiết hàng bước nhỏ và tiêu chí hoàn thành nằm trong `implementation-plan.md`
của component; status theo dõi ở `project/app/facial_recognition/docs/`.

## Kết nối tài liệu

- Build / flash / runtime / API: [`README.md`](README.md)
- Component: `project/app/facial_recognition/README.md` và thư mục `docs/`
  của component (architecture, camera, rtsp, ai, api, testing, debugging).