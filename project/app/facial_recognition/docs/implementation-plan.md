# Kế hoạch triển khai Facial Authentication

Cập nhật: 2026-09-05. Phạm vi: Luckfox Pico Pro Max / RV1106 / SC3336,
Ethernet, RTSP và WebConfig. Mục tiêu là đăng ký khuôn mặt và xác minh 1:1
với tài khoản được chọn trên thiết bị.

## Cách thực hiện

- Mỗi lượt hoàn thành một bước nhỏ, ghi bằng chứng và bước tiếp theo ở cuối file.
- Phân biệt kiểm tra source, build, test host và test thiết bị. Không suy ra
  runtime thành công từ việc build thành công.
- Deploy thay đổi có backup và hướng rollback; giữ phương thức đăng nhập hiện tại.
- Chỉ bật UI khi backend tương ứng chạy thật. Không cấp quyền đăng nhập từ
  descriptor thủ công hay threshold chưa hiệu chỉnh.
- Không dùng RTSP decode làm nguồn AI. Queue giới hạn, bỏ frame cũ khi quá tải.
- Model cần nguồn, checksum, license và đặc tả tiền/hậu xử lý; chọn theo RAM
  đỉnh và chất lượng đo trên RV1106, không chỉ theo kích thước file.

## Baseline từ kiểm tra source

- Đã có `fr_media_service.cpp`, detector RKNN, WebConfig enrollment và database JSON.
- `docs/detector-verification.md` ghi nhận kiểm tra detector trên target ngày
  2026-08-28; đây là bằng chứng lịch sử, chưa kiểm tra lại ở lượt này.
- `FaceRecognizer::ExtractFeature()` hiện tính 128 giá trị từ landmarks và
  texture/intensity. Đây chưa phải embedding từ model recognition học sâu.
- `AlignAndCropFace()` hiện crop bbox có margin và resize, chưa affine alignment
  theo 5 landmarks.
- `FaceDatabase::FindMatch()` có threshold mặc định 0.70; chưa có bằng chứng
  calibration production trong bước kiểm tra này.
- Environment `/tmp/fr-rknn-p6WNqC/env` không còn. Host có Python 3.14.4.
- YuNet và SFace là ứng viên trước đây, chưa phải lựa chọn đã benchmark RV1106.

## Các bước nhỏ

| ID | Công việc | Tiêu chí hoàn thành | Trạng thái |
| --- | --- | --- | --- |
| 00 | Kiểm kê source và môi trường hiện tại | Ghi nhận implementation thật, sai khác với kế hoạch cũ | Xong (source) |
| 01 | Kiểm tra baseline thiết bị | IP, hash binary/model, API, RAM, camera/RTSP; lưu log có thời gian | Chưa làm |
| 02 | Tái tạo converter ở thư mục bền vững | Python tương thích, Toolkit import được; lưu phiên bản dependency và lệnh tái tạo | Chưa làm |
| 03 | Đánh giá detector hiện có | Đối chiếu log lịch sử, nguồn/license, bbox/landmark trên ảnh thật; quyết định giữ hay thử YuNet | Chưa làm |
| 04 | Benchmark detector cùng RTSP | Đo latency, RAM/RSS, frame RTSP; test 10 phút, 30 phút, 2 giờ | Chưa làm |
| 05 | Chọn recognizer nhẹ | So sánh SFace/MobileFaceNet với weight có nguồn rõ; convert, query metadata và đo RAM | Chưa làm |
| 06 | Kiểm tra embedding | So ONNX/RKNN trên cùng ảnh aligned; 1.000 inference; kiểm tra finite, dimension, ổn định RAM | Chưa làm |
| 07 | Thay descriptor bằng model | Model load một lần; reusable buffers; lỗi inference không tạo danh tính | Chưa làm |
| 08 | Alignment và quality | Affine đúng template model; test blur, tối, mặt nhỏ, nhiều mặt, landmark lỗi | Chưa làm |
| 09 | Version hóa database | Model ID/hash/dimension gắn mỗi embedding; dữ liệu descriptor cũ không dùng với model mới; backup/migration rõ | Chưa làm |
| 10 | Enrollment thật | Tạo/xóa người, capture mẫu đạt quality, lưu sau reboot, audit; không lưu ảnh gốc mặc định | Chưa làm |
| 11 | Calibration | Dataset được đồng ý sử dụng; genuine/impostor scores; báo cáo FAR/FRR và threshold | Chưa làm |
| 12 | Phiên đăng nhập | Xác minh 1:1 gắn request với session/tài khoản, timeout, chống replay kết quả, rate limit, recovery login | Chưa làm |
| 13 | Liveness và quyền truy cập | Kiểm thử ảnh in/màn hình/video; ghi giới hạn RGB; chưa đạt thì không bật face-only login đặc quyền | Chưa làm |
| 14 | Kiểm thử tích hợp | Camera + RTSP + AI + DB + Web, quá tải/restart/mất LAN/reboot, chạy 24 giờ | Chưa làm |

## Báo cáo mỗi bước

Ghi: file đọc/tạo/sửa; API đã xác minh; lệnh thực hiện; kết quả build;
kết quả runtime; số đo và điều kiện đo; lỗi còn lại; deploy/rollback;
bước tiếp theo. Không gắn nhãn hoàn thành cả dự án khi mới chạy detector.

## Nhật ký

### 2026-09-05 — Bước 00

- Đã đọc `AGENTS.md`, README, recognizer implementation, database header,
  Makefile và tài liệu detector verification.
- Đã kiểm kê source/scripts/docs, kiểm tra Python host và environment cũ.
- Phát hiện descriptor thủ công và bbox crop cần được thay thế trong các bước
  07–08; bảo toàn code hiện tại để không ghi đè công việc đang có.
- Tạo kế hoạch này và liên kết từ README. Không thay runtime, không deploy.
- Build và performance: không chạy vì bước này chỉ kiểm kê và cập nhật docs.
- Bước tiếp theo: 01, lấy baseline thiết bị trước khi sửa pipeline.
