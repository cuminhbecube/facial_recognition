# Firmware nhận diện khuôn mặt — Rockchip RV1106

Bộ SDK hợp nhất để xây dựng firmware **camera nhận diện khuôn mặt** trên nền
Rockchip RV1106 (bố trí Luckfox Pico Pro Max, SPI-NAND, buildroot) với một
camera SC3336 MIPI CSI-2. Toàn bộ cây SDK Rockchip `RV06_03` được giữ trong
cùng một repository: `sysdrv` (U-Boot / kernel / buildroot), `media` (thư viện
pipeline ISP) cùng chương trình nhận diện, kèm toolchain và `media_out` prebuilt
để clone về là build được ngay.

Phiên bản firmware hiện hành: **0.6.2** (xem [`FIRMWARE_VERSION`](FIRMWARE_VERSION)).

## Nhận diện khuôn mặt

- Phát hiện khuôn mặt bằng **YOLOv5n-Face** chạy trên NPU RV1106 (640x640), kèm
  5 điểm landmark.
- Căn chỉnh, lọc chất lượng và trích xuất vector đặc trưng **128-D**.
- So khớp **Cosine Similarity** theo thời gian thực với danh sách người đã đăng
  ký.
- **Đăng ký người mới trực tiếp trên web** khi đứng trước camera: thêm / xem /
  xóa danh sách người dùng.

## Camera, RTSP và WebConfig

- Luồng H.264/H.265 mã hóa phần cứng phát qua RTSP:
  `rtsp://<ip-thiet-bi>:554/live/0`.
- Web Config trên cổng 80 (đăng nhập `root`): dashboard, cấu hình camera /
  streaming, quản lý AI và lưu trữ; API REST đầy đủ.
- Thiết bị dùng **Ethernet có dây** — không kéo theo Wi-Fi userspace hay module
  để tối giản và ổn định.

## Lưu trữ quay phim tự động dọn

Video ghi theo ngày trong thư mục trên thẻ SD. Người dùng cấu hình từ web hai
chế độ dọn dữ liệu:

- **Giữ đoạn tối đa (ngày)** — tự xóa các đoạn cũ hơn giới hạn.
- **Ngưỡng dung lượng trống (MB)** — dọn khi thẻ dưới ngưỡng dung lượng trống
  để tránh đầy thẻ.

Thiết lập được lưu vào `/oem/usr/etc/facial-recognition/rtsp.conf` và áp dụng
ngay bởi dịch vụ media (`-D <ngay>` / `-F <MB>`).

## Bố cục kho lưu trữ

| Thư mục | Nội dung |
|---|---|
| `sysdrv/` | U-Boot, kernel (gồm driver SC3336 + DTS Pico Pro Max) và buildroot |
| `media/` | Thư viện media của Rockchip (ISP / IVA / rockit / rga / mpp ...) |
| `output/out/media_out/` | Headers + thư viện + IQ file prebuilt cho app |
| `tools/` | Công cụ packaging Linux và toolchain `arm-rockchip830-linux-uclibcgnueabihf` |
| `project/app/facial_recognition/` | Ứng dụng nhận diện khuôn mặt |
| `project/app/capture_ai/` | `3rdparty/rknpu2` + model `yolov5n-face-rv1106.rknn` |
| `project/cfg/` | BoardConfig, kể cả profile FACIAL_RECOGNITION |

## Build

```sh
sudo apt-get install -y git ssh make gcc gcc-multilib g++-multilib module-assistant g++ gawk texinfo libssl-dev bison flex fakeroot cmake unzip gperf autoconf device-tree-compiler libncurses5-dev pkg-config bc python-is-python3 passwd openssl vim file cpio rsync
./build.sh info        # kiểm tra board profile đang chọn
./build.sh firmware    # build toàn bộ + đóng gói firmware
```

Profile board đã được kích hoạt sẵn qua symlink `.BoardConfig.mk`. Output:
`output/image/update.img` và `Upgrade/upgrade-0.6.2.img` (file này không commit
lên git; tạo lại mỗi lần release).

Chi tiết: [`README.md`](README.md), `project/app/facial_recognition/docs/`,
[`CURRENT_STATUS.md`](CURRENT_STATUS.md).