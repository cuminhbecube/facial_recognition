# RV06 — Camera hành trình thông minh cho xe

RV06 là thiết bị camera hành trình/đầu ghi hình hai kênh dành cho xe cá nhân,
xe dịch vụ và đội xe vận tải. Sản phẩm kết hợp ghi hình trước–sau, định vị GPS,
quản lý tài xế bằng thẻ RFID/GPLX, kết nối giám sát từ xa CMSV6 và cấu hình
ngay trên điện thoại hoặc máy tính qua Web Config nội bộ.

Được xây dựng trên nền tảng Rockchip RV1106, RV06 ưu tiên sự ổn định: ghi hình
liên tục, dữ liệu được bảo vệ khi mất nguồn, kết nối server chủ động và các
thành phần camera hoạt động độc lập để hạn chế ảnh hưởng chéo.

## Ghi hình trước–sau toàn diện

BECAM-2 sử dụng hai camera AHD qua bộ giải mã Pixelplus PR2100K:

- Camera trước CH1/CAM0 đi vào PR2100K VIN0 và MIPI virtual channel VC0.
- Camera sau CH2/CAM1 đi vào PR2100K VIN1 và MIPI virtual channel VC1.

PR2100K xuất YUV422 trên một liên kết MIPI CSI-2 hai lane; hai virtual channel
được ánh xạ thành hai pipeline VI/VENC độc lập. OV8858 và GC2053 trực tiếp chỉ
còn thuộc kiến trúc camera legacy.

Hai luồng ghi hình H.265 được mã hóa bằng phần cứng, giúp tiết kiệm dung lượng
thẻ SD mà vẫn đảm bảo chất lượng. Video được tự động chia thành các đoạn ngắn
và lưu theo từng ngày, giúp tra cứu nhanh, quản lý gọn gàng và hạn chế tình
trạng một thư mục chứa quá nhiều tệp sau thời gian dài sử dụng.

Khi một đoạn video hoàn tất, RV06 kiểm tra dữ liệu, đồng bộ xuống thẻ nhớ và
đổi tên an toàn từ file tạm sang file hoàn chỉnh. Cơ chế này giảm rủi ro xuất
hiện video lỗi khi xe mất điện hoặc hệ thống khởi động lại đột ngột.

## Giám sát trực tuyến qua CMSV6

RV06 hỗ trợ JT808 để kết nối nền tảng giám sát và JT1078 để truyền hình ảnh
trực tiếp theo yêu cầu. Thiết bị không tự động tiêu tốn băng thông để phát
video liên tục: live stream chỉ bắt đầu khi máy chủ gửi lệnh xem trực tiếp và
dừng ngay khi phiên xem kết thúc.

Thông qua CMSV6, người vận hành có thể:

- Theo dõi trạng thái thiết bị, GPS, camera và lưu trữ.
- Xem video trực tiếp từng kênh.
- Yêu cầu chụp ảnh từ xa.
- Tìm kiếm và phát lại video đã ghi trên thiết bị.
- Kiểm tra thông tin terminal, biển số và phiên bản firmware.

RV06 duy trì heartbeat và báo cáo vị trí để máy chủ luôn nắm được trạng thái
kết nối của xe.

## Chụp ảnh từ xa, kể cả khi không có thẻ SD

Khi CMS yêu cầu chụp ảnh, RV06 sử dụng encoder JPEG phần cứng riêng để lấy ảnh
từ camera mà không làm gián đoạn quá trình ghi hình chính.

Nếu thẻ SD sẵn sàng, ảnh chụp local có thể được lưu trên thiết bị. Trong trường
hợp không có thẻ SD, thẻ bị tháo, chỉ-đọc hoặc dung lượng thấp, ảnh chụp từ xa
vẫn được tạo trong bộ nhớ RAM, kiểm tra tính hợp lệ JPEG và gửi trực tiếp lên
CMS. Điều này giúp xe vẫn có thể phản hồi yêu cầu chụp ảnh trong các tình huống
lưu trữ tạm thời không khả dụng.

## GPS và thông tin hành trình

BECAM-2 dùng Quectel EC800M-CN ở biến thể phần cứng có GNSS để cung cấp kết nối
4G và dữ liệu vị trí. Dữ liệu GNSS được dùng cho báo cáo JT808, hiển thị OSD và
giám sát từ xa. EC25 chỉ còn là profile tương thích legacy; EG800AK không còn
nằm trong roadmap production.

GNSS là tùy chọn theo biến thể EC800M-CN. Firmware phải xác minh capability
trên module thật và không được giả lập vị trí khi biến thể không hỗ trợ GNSS.

Người dùng có thể tùy chọn hiển thị hoặc ẩn từng thông tin trên video:

- Thời gian.
- Tọa độ GPS.
- Tốc độ.
- Biển số xe.
- Tên tài xế.

Tắt hiển thị GPS trên video không làm tắt bộ thu GPS; thiết bị vẫn tiếp tục
định vị và gửi dữ liệu hành trình tới server.

## Quản lý tài xế bằng RFID/GPLX

RV06 kết nối với đầu đọc thẻ lái xe qua UART. Hệ thống nhận thẻ theo cơ chế
event-driven, phản hồi nhanh với thao tác quét thẻ và hạn chế nhận lặp lại một
thẻ trong thời gian ngắn.

Thiết bị tự xử lý các tình huống quen thuộc:

- Quét thẻ mới để đăng nhập tài xế.
- Quét lại thẻ đang hoạt động để đăng xuất.
- Quét thẻ khác để chuyển tài xế.

Thông tin tài xế có thể hiển thị trên OSD và đồng bộ vào dữ liệu giám sát. Hệ
thống đèn POWER, NET, GPS, DRIVER, REC cùng còi báo cho phép nhận biết nhanh
trạng thái nguồn, mạng, GPS, tài xế và ghi hình ngay tại xe.

## Xem hình ảnh nội bộ qua RTSP

Ngoài CMSV6, RV06 cung cấp RTSP nội bộ để xem luồng chính hoặc luồng phụ của
từng camera. Luồng phụ H.264 được tối ưu cho giám sát, trong khi luồng H.265
chính ưu tiên chất lượng ghi hình.

Ví dụ địa chỉ xem nội bộ:

```text
rtsp://<ip-thiet-bi>:554/live/cam0/main
rtsp://<ip-thiet-bi>:554/live/cam1/main
```

## Web Config dễ sử dụng

Web Config tích hợp sẵn trên thiết bị, truy cập từ mạng LAN hoặc Wi-Fi AP mà
không cần cài thêm phần mềm. Giao diện phù hợp cả điện thoại và máy tính, hỗ
trợ các nhóm chức năng:

- Tổng quan thiết bị và tình trạng hoạt động.
- Thông tin xe, biển số và tài xế.
- Camera, ghi hình và chụp ảnh.
- OSD/GPS, RTSP và CMSV6/JT808.
- Wi-Fi AP, thẻ SD, bảo trì, chẩn đoán và bảo mật.

Cấu hình được lưu an toàn trên phân vùng riêng, vì vậy các thiết lập quan trọng
như mạng, camera và CMS được giữ lại qua cập nhật firmware thông thường.

## Thiết kế cho vận hành ổn định

RV06 được thiết kế để các chức năng quan trọng cùng tồn tại: ghi hình, định vị,
live stream, chụp ảnh, RFID và Web Config. Các log chẩn đoán hỗ trợ kỹ thuật
viên kiểm tra nhanh camera, GPS, kết nối CMS, RTSP và trạng thái lưu trữ khi
cần bảo trì.

Phiên bản firmware hiện hành được ghi tại
[`FIRMWARE_VERSION`](FIRMWARE_VERSION).
