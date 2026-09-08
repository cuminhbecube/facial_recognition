# Comprehensive Project Status - RV06 Dashcam & RFID System

## Latest Update — 2026-08-20

Production product definition:

```text
BECAM-2 = RV1106 + PR2100K dual AHD + EC800M-CN with GNSS
```

EC800M-CN replaces EC25 as the production modem target. EC25 remains only as a
legacy compatibility profile, and EG800AK is removed from the production
roadmap. Since GNSS is optional across EC800M-CN hardware variants, the exact
production SKU/revision and its runtime USB/AT/GNSS/data-interface mapping are
`CHUA XAC MINH` until verified on the physical module.

The active source is firmware `0.6.1` with **Dual AHD Camera via Pixelplus PR2100K Video Decoder**, **Simultaneous Four-VENC Hardware Encoding**, **Dual JT1078 Streaming**, **Four RTSP Streams**, and **Dynamic WebConfig Resolutions & Substream Bitrates**:
- **Dual AHD via PR2100K Video Decoder**:
  - Decoded video input: Pixelplus PR2100K on **I2C4** (`0x5F`), chip ID `0x2100`.
  - Translates two analog HD camera streams (VIN0 $\to$ CAM0 Front, VIN1 $\to$ CAM1 Rear) into a 2-lane MIPI CSI-2 stream using Virtual Channels (**VC0** for CAM0, **VC1** for CAM1).
  - YUV422 input ingested directly by Rockchip CIF/VI without RAW Bayer demosaicing.
  - Independent CAM0 (`/dev/video0`) and CAM1 (`/dev/video1`) at 1080p25.
- **Simultaneous Four-VENC Encoder Architecture**:
  - VENC0: CAM0 Main (H.265, 1080p@20 / 720p@15 / D1 / 360p / CIF) $\to$ `/mnt/sdcard/DCIM/front` + RTSP Main.
  - VENC1: CAM1 Main (H.265, 1080p@20 / 720p@15 / D1 / 360p / CIF) $\to$ `/mnt/sdcard/DCIM/rear` + RTSP Main.
  - VENC2: CAM0 Sub (H.264, D1 / 360p / CIF @ 5-15 FPS, Bitrate 200-1000 Kbps) $\to$ JT1078 Ch1 + RTSP Sub.
  - VENC3: CAM1 Sub (H.264, D1 / 360p / CIF @ 5-15 FPS, Bitrate 200-1000 Kbps) $\to$ JT1078 Ch2 + RTSP Sub.
  - Shared encoded fanout: RTSP and JT1078 share VENC2/VENC3 channels (VENC count = 4, NOT 6).
  - VENC3 NOMEM eliminated via optimized single MB buffer counts and dedicated RGA NV12 subpools.
  - Total CMA allocation in 4-VENC mode: ~38.8 MiB out of 64 MiB total (25.2 MiB safe headroom).
- **RTSP Streaming (All 4 Streams Verified)**:
  - `rtsp://<ip>:554/live/cam0/main` (H.265 Main Front)
  - `rtsp://<ip>:554/live/cam1/main` (H.265 Main Rear)
  - `rtsp://<ip>:554/live/cam0/sub` (H.264 Sub Front)
  - `rtsp://<ip>:554/live/cam1/sub` (H.264 Sub Rear)
- **WebConfig Dynamic Resolutions & Substream Bitrates**:
  - Configurable Main Resolutions: `1920x1080`, `1280x720`, `720x576`, `640x360`, `352x288`.
  - Configurable Sub Resolutions: `720x576`, `640x360` (default), `352x288`.
  - Configurable Sub Bitrates: `200`, `350`, `500` (default), `800`, `1000` Kbps per camera.
  - D1 Constraint Guard: When Sub is D1 (720x576), Main is restricted below 1080p.
  - Dual 1080p@20 Main + Dual 360p@12 Sub operates with ~21.2 MiB safe CMA headroom.

The built package is `Upgrade/upgrade-0.6.1.img`.

## 1. System Overview & Current Release
- **SDK Repository Root**: `/home/adimi/RV06_03_Linux_SDK`
- **Application Source Root**: `/home/adimi/RV06_03_Linux_SDK/project/app/dashcam`
- **Current Active Version**: v0.6.1
- **Last Built**: 2026-08-20
- **Firmware Checksum (SHA256)**: `742424031b875e9d9cc15596decd8a4abcfce65999c6a1d75af5996f4a193aba`

---

## 2. Target Device & Hardware Specifications
- **Master Processor**: Rockchip RK1106 (ARM Cortex-A7, UClibc Toolchain)
- **Active IP Address**: `192.168.9.139` (SSH / Telnet `root` / `root`)
- **AHD Video Decoder**: Pixelplus PR2100K (I2C Bus: **I2C4**, Address: `0x5F`, 2-lane MIPI CSI-2, VC0/VC1)
- **ACC Ignition Hardware Pin**: `GPIO1_B1` (sysfs GPIO 41)
  - Active Level: **Active LOW** (`0` = ACC ON / BẬT KHÓA, `1` = ACC OFF / TẮT KHÓA)
  - Environment Variable: `RV06_ACC_ACTIVE_LEVEL=0`
  - Debounce Engine: `300 ms` ON debounce / `2000 ms` OFF debounce
  - Grace Period: 10-second shutdown delay after file flush before system power off
- **RFID GPLX Panel UART**: `/dev/ttyS3` (Platform `ff4d0000.serial`, Baudrate 115200 8N1)
- **CMSV6 Telematics Server**: `cam.tracking.vn:6608` (JT808 control / JT1078 video streams on port 6631)
- **GNSS / 4G Module**: Quectel EC800M-CN hardware variant with GNSS. Exact
  VID:PID, AT/NMEA port mapping and Linux data interface: `CHUA XAC MINH`; do
  not hard-code the former EC25/EG800AK mapping.
- **Storage Media**: SD Card (`/dev/mmcblk1p1` VFAT mounted read/write at `/mnt/sdcard`, max 128 GB)
- **NAND Flash Partition Layout (256 MB SPI NAND)**:
  ```text
  256K(env), 1M@256K(idblock), 1M(uboot), 5M(boot), 208M(rootfs), 32M(gshtdata), 8960K(dashcfg)
  ```
  - `/dev/mtdblock5` formatted as JFFS2 and mounted at `/persist` (`/run/rv06-config -> /persist/dashcam`).

---

## 3. Dual Camera Hardware Mapping & Encoder Settings

### AHD Video Decoder Pipeline (PR2100K)
| Role | Input Source | Decoder & Interface | Virtual Channel | VI Dev/Pipe | VENC | Storage Output |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Front (CAM0)** | AHD 1080p25/30 | PR2100K VIN0 $\to$ I2C4 `0x5F` $\to$ MIPI DPHY1 (2 lanes) | **VC0** | `0/0` | VENC 0 (Main), VENC 2 (Sub) | `/mnt/sdcard/DCIM/front` |
| **Rear (CAM1)** | AHD 1080p25/30 | PR2100K VIN1 $\to$ I2C4 `0x5F` $\to$ MIPI DPHY1 (2 lanes) | **VC1** | `1/1` | VENC 1 (Main), VENC 3 (Sub) | `/mnt/sdcard/DCIM/rear` |

*Note: Legacy sensor drivers (OV8858/GC2053) remain accessible via `RV06_CAMERA_INPUT=mipi_sensor_legacy` for hardware fallback.*

### Active Camera Modes & Launcher Configuration
- Mandatory Launcher Environment:
  ```sh
  RV06_CAMERA_MODE=dual
  RV06_CAMERA_INPUT=ahd_pr2100k
  RV06_PR2100K_I2C_BUS=4
  RV06_PR2100K_I2C_ADDR=0x5f
  RV06_DIAGNOSTIC=off
  RV06_ISP_GROUP=off
  RV06_ACC_ACTIVE_LEVEL=0
  ```

### Encoding Parameters
| Parameter | CAM0 Main | CAM1 Main | CAM0 Sub | CAM1 Sub |
| :--- | :--- | :--- | :--- | :--- |
| **Resolution** | 1080p / 720p (default) / D1 / 360p / CIF | 1080p / 720p (default) / D1 / 360p / CIF | D1 / 360p (default) / CIF | D1 / 360p (default) / CIF |
| **FPS** | 10, 12, 15 (default), 20, 25 | 10, 12, 15 (default), 20, 25 | 5, 10, 12 (default), 15 | 5, 10, 12 (default), 15 |
| **Codec** | H.265 Hardware VENC (VENC0) | H.265 Hardware VENC (VENC1) | H.264 Hardware VENC (VENC2) | H.264 Hardware VENC (VENC3) |
| **Rate Control** | CBR (600, 800, 1024, 1500, 2000 Kbps) | CBR (600, 800, 1024, 1500, 2000 Kbps) | CBR (200, 350, 500, 800, 1000 Kbps) | CBR (200, 350, 500, 800, 1000 Kbps) |
| **GOP** | FPS * 2 (e.g. 30) | FPS * 2 (e.g. 30) | FPS * 2 (e.g. 24) | FPS * 2 (e.g. 24) |
| **Output Target** | SD Card Rec + RTSP Main | SD Card Rec + RTSP Main | JT1078 Ch1 + RTSP Sub | JT1078 Ch2 + RTSP Sub |
| **Segment Length**| 60, 120, 180 (default), 300, 600 s | 60, 120, 180 (default), 300, 600 s | Real-time streaming | Real-time streaming |

---

## 4. Modular Software Architecture & Subsystems

```text
+-----------------------------------------------------------------------------------+
|                                 rv06_dashcam                                      |
|                                                                                   |
|  +--------------------+   +-------------------+   +----------------------------+  |
|  |    acc_driver      |   |    rfid_driver    |   |   driver_uart_manager      |  |
|  |  (GPIO41 Hardware) |   |  (UART3 Protocol) |   |   (Card Swipe & Events)    |  |
|  +---------+----------+   +---------+---------+   +-------------+--------------+  |
|            |                        |                           |                 |
|            +------------------------+---------------------------+                 |
|                                     |                                             |
|                        +------------v------------+                                |
|                        |      jt808_client       |                                |
|                        |  (CMSV6 Server Sync)    |                                |
|                        +-------------------------+                                |
+-----------------------------------------------------------------------------------+
```

### Module Breakdown
1. **`acc_driver.h / acc_driver.c` (Standalone ACC Hardware Driver)**:
   - Manages raw sysfs GPIO 41 reading (`acc_read_raw_gpio()`), instantaneous status (`acc_read_is_on()`), environment active level lookup (`acc_get_active_level()`), and 300ms ON / 2000ms OFF debounce engine (`acc_debounce()`).
   - Completely independent of RFID UART frames or serial protocols.

2. **`rfid_driver.h / rfid_driver.c` (Standalone RFID Protocol Driver)**:
   - Handles UART3 serial connection (`/dev/ttyS3`, 115200 8N1), 11 IO channels, card read (`0x01`), write (`0x02`), and IO control (`0x03`).
   - Contains Vietnamese accent removal (`rfid_remove_vietnamese_accents()`) to convert UTF-8 names to uppercase ASCII.
   - Maps system status to 11 LED/Buzzer channels (`rfid_map_dashcam_to_output()`) and adapts status to CMSV6 (`map_dashcam_to_cmsv6()`).
   - Automatically enables panel LED power rail (`led_pwr = 0x01`) when ACC is ON.

3. **`driver_uart_manager.cpp / driver_uart_manager.hpp`**:
   - C++ thread manager handling driver card swiping, persistent driver state (`/run/rv06-config/driver.state`), and ACC transition events.

4. **`jt808_client.cpp / jt808_client.hpp`**:
   - Manages JT808 TCP control connection to `cam.tracking.vn:6608` and JT1078 real-time video streaming channels 1 and 2 to port 6631.
   - Encodes location report `0x0200` with multi-layer extended telemetry for CMSV6.
   - Handles driver identity reports (`0x0702`) and server requests (`0x8702`).

5. **`rv06_webconfig` (Embedded Web Server)**:
   - Native C++ process listening on port 80 (RSS ~1.5 MB).
   - Manages read-only/writable runtime settings (`runtime.conf`), Wi-Fi AP parameters, SD maintenance, password updates, and system self-tests.
   - Zero video streaming elements or heavy web frameworks.

6. **`rfid-tool` (CLI Diagnostic Utility)**:
   - Standalone executable supporting commands: `acc`, `read`, `write`, `gps`, `net`, `login`, `rec`, `buzzer`, `all-off`, `sync`, and `pulse-buzzer`.

---

## 5. CMSV6 Location Protocol Specification (`0x0200` Extensions)

To ensure accurate telematics display on CMSV6 / 808gps servers, location report `0x0200` is constructed with the following TLV extension matrix:

| CMSV6 UI Element | Extension IDs & Encoding Format | Transmitted Value / Bitmask | Expected CMSV6 Display |
| :--- | :--- | :--- | :--- |
| **Ignition State** | `status_dw` Bit 0 | `1` when ACC ON, `0` when ACC OFF | `Online, 4G, ACC On, Park Acc On` |
| **Signal Quality** | Extension `0x30` (1 byte) | CSQ = `31` (Maximum Signal) | `Network Signal Excellent` |
| **GNSS Satellites** | Extension `0x31` (1 byte) | Satellites Count = `15` | `Number of satellites: 15` |
| **Storage Presence** | Multi-Layer Matrix: `0x77` (JT1078 Standard 2B), `0xE5` (CMSV6 Vendor 2B), `0x05` (Legacy 2B) | `0x0001` (Primary Storage Present & Normal) | `Hard Disk(Exist)` |
| **Video Recording** | Multi-Layer Matrix: `0x76` (JT1078 Standard 2B), `0xE1` (CMSV6 Vendor 4B), `0x91` (Legacy 4B) | `0x0003` / `0x00000003` (Channel 1 & 2 Active) | `Recording: Kênh1,Kênh2` |
| **False Alarm Fix** | Completely Removed `0x14` | Omitted from TLV payload | Eliminates false `Video Loss Alarm` |

---

## 6. Full SDK Build & Deployment Sequence

When modifying application source code or drivers, the following sequence MUST be followed to ensure binaries inside `rootfs` and `upgrade-0.5.5-debug.img` are updated:

```bash
# 1. Rebuild application binaries and install to out/
cd /home/vunl/RV06_03_Linux_SDK/project/app/dashcam
make clean && make all

# 2. Stage binaries to SDK output staging (/output/out/app_out)
cd /home/vunl/RV06_03_Linux_SDK
./build.sh app

# 3. Package rootfs and export upgrade image
./build.sh firmware
```

Output firmware image location:
- Path: `Upgrade/upgrade-0.5.5-debug.img`
- SHA256: `100f639cdce335f26e031e9bac53c069f8882ffaa97811db7a2678a1fd51333d`

---

## 7. Version & Feature Evolution History

- **0.3.3 - 0.3.4**: Initial dual camera setup (OV8858 + GC2053). Resolved `camgroup` overflow by separating independent AIQ contexts (`RV06_ISP_GROUP=off`).
- **0.3.5**: Added SD storage auto-repair/formatting logic, non-blocking FAT32 mount, and VENC OSD regions (Vietnam timestamp + channel label).
- **0.3.6**: Added Quectel EC25-E NMEA GPS parsing, RTSP server on port 554 (`/live/cam0/main`, `/live/cam1/main`, `/live/cam0/sub`, `/live/cam1/sub`).
- **0.3.7 - 0.3.9**: Built native lightweight `rv06_webconfig` C++ server on port 80. Added persistent runtime config (`runtime.conf`), atomic file operations, and self-tests.
- **0.4.0 - 0.4.4**: Added dual camera resolution/bitrate controls, driver identity OSD overlay (`CH1 | <plate> | <driver>`), and DRV1 JSON parser.
- **0.4.5 - 0.4.9**: Integrated JT808 control client (`cam.tracking.vn:6608`) and JT1078 TCP video streaming (port 6631). Fixed JT1078 RTP header format (`0x81`). Added driver report `0x0702` sync.
- **0.5.0 - 0.5.2**: Created 8960 KB JFFS2 persistent partition (`dashcfg` at `/persist`). Added driver state persistence (`driver.state`) across reboots.
- **0.5.3 - 0.5.4**: Optimized startup auth token reuse. Developed initial RFID UART C driver (`rfid_driver.c/h`) and `rfid-tool` CLI.
- **0.5.5**: **Modular Architecture Refactoring & CMSV6 Protocol Fixes**:
  - Decoupled ACC hardware driver (`acc_driver.c/h`) from RFID UART protocol code.
  - Implemented 300ms/2000ms ACC debounce engine on `GPIO1_B1` (sysfs GPIO 41).
  - Built multi-layer CMSV6 extension matrix (`0x77`, `0x76`, `0xE5`, `0xE1`, `0x05`, `0x91`, `0x30`, `0x31`) for `Hard Disk(Exist)`, `Recording: Kênh1,Kênh2`, and `Network Signal Excellent`.
  - Removed `0x14` extension to resolve false `Video Loss Alarm`.
- **0.5.8**: Tối ưu hóa chu trình đọc thẻ RFID (giảm độ trễ khóa 5 giây, xử lý trạng thái thẻ chưa đăng nhập).
- **0.5.9**: Tái cấu trúc pipeline OSD với `OSD_BindManager` và refactor Snapshot manager.
- **0.5.10**: Fix lỗi `VI_BIND_FAILED` bằng manual frame passing cho Snapshot; loại bỏ render ảnh CAM0/CAM1 trong Web Config.
- **0.5.11**: Cập nhật Web Config (ẩn view ảnh Snapshot) và thêm ID thiết bị vào trang Tổng quan.
- **0.5.12**: Cập nhật backend `rv06_webconfig` lấy mã ID thiết bị đầy đủ từ `TERMINAL_PHONE` (ví dụ `874903786373`).
- **0.5.13**: Triển khai kiến trúc Unified StorageManager: Tách biệt ưu tiên Recording > Streaming > Snapshot, tự động dừng Snapshot khi dung lượng còn dưới reserve, tự phục hồi khi gặp ENOSPC. Triển khai `DeviceInfoManager` tập trung dữ liệu phần cứng/phần mềm/EC25 modem (IMEI/ICCID), phản hồi non-blocking cho truy vấn JT808 `0x8107` -> `0x0107`, `0x8104`/`0x8106` -> `0x0104`, phản hồi thành công `0x0001 (Result 0)` cho `0x8103`, và chuẩn hóa gói tin nhị phân CMSV6 `0x3040` (hỗ trợ Machine Type: SD Card, Channel Recording Bitmask: CH1+CH2, Storage Disk SD1 capacity/free MB/serial, Network 4G LTE, IMEI, Version NO. 0.5.13, 4G module active) khắc phục hoàn toàn lỗi hiển thị thông tin thiết bị/thẻ nhớ/kênh trên CMSV6. Bổ sung API `/api/v1/device-info` và thẻ Thông tin thiết bị trên Web Config.
- **0.5.26**: **P0 Playback Runtime Speed Control & Timeline Seek Fix**:
  - Triển khai `[PLAYBACK_RX]` ghi log chi tiết telemetry cho inbound CMS messages `0x9201` và `0x9202`.
  - Cập nhật dynamic speed pacing (`PlaybackManager::SetSpeed`), thay đổi nhịp phát `wallFrameUs` ngay lập tức mà không cần reconnect socket hay reset decoder.
  - Cập nhật timeline seek (`PlaybackManager::Seek`), ngắt file reader ngay lập tức (<1ms), định vị đúng segment video trên thẻ nhớ, bỏ qua các frame trước mốc seek, đồng bộ VPS/SPS/PPS và IDR keyframe trước khi phát lại, rebase timestamp mốc thời gian chuẩn xác.
  - Hỗ trợ đầy đủ chu trình Pause -> Seek -> Resume và Speed -> Seek.
- **0.5.27**: **P0 Vehicle Plate & Driver State Persistence Fixes**:
  - **Khắc phục lỗi mất biển số xe sau upgrade firmware**: Đồng bộ canonical `vehicle.conf` (phân vùng JFFS2 `/persist/dashcam/vehicle.conf`) sang `dashcam.sh`, `main.cpp`, `web_config_server.cpp`, và `jt808_client.cpp`. Sửa lỗi `PublishRuntimeConfig()` ghi đè rỗng vào `runtime.conf`.
  - **Khắc phục lỗi không phục hồi tên tài xế lên OSD sau reboot**: Nạp `LoadPersistentState()` tường minh trong `DriverUartManager::Start()` ngay sau khi mount flash; loại bỏ lệnh xoá RAM `loggedIn = false` khi ACC-OFF. OSD (CAM0 & CAM1) và JT808 đồng bộ hiển thị tên tài xế ngay từ frame khởi động đầu tiên.
- **0.5.28**: **P0 ACC OFF Power / Recording / RTSP / JT808 / Wi-Fi Adaptive Policy & RAM Leak Fix**:
  - **Quản lý Wi-Fi AP theo ACC**: Tự động đếm 5 phút (300s) sau khi ACC OFF để tắt Wi-Fi AP (`/etc/init.d/S70rv06-wifi-ap stop`) tiết kiệm nguồn; hủy timer nếu ACC ON trở lại trong 5 phút; bật lại Wi-Fi AP ngay khi ACC ON.
  - **Delay dừng ghi video**: Cấu hình `acc_record_stop_delay_sec=10` (0-3600s), đếm ngược sau debounce ACC OFF 2000ms, hủy bộ đếm nếu ACC ON lại, fsync sạch segment `.tmp` -> `.h265` khi hết giờ.
  - **Giải phóng RTSP RAM**: Ngắt client RTSP và giải phóng queue ngay khi ACC OFF, lazy re-enable khi ACC ON.
  - **Chu kỳ vị trí thích ứng JT808**: ACC ON `10s` (`jt808_location_interval_acc_on_sec=10`), ACC OFF `60s` (`jt808_location_interval_acc_off_sec=60`), gửi ngay bản tin `0x0200` khi đổi trạng thái ACC.
  - **Fix rò rỉ RAM do unlinked log inode & Fix cảnh báo camera giả lập**: Chuyển cơ chế rotate log (`rotate_log`, `rv06_logmon`) từ `mv` sang trim in-place (`cat $trim > $file`), giải phóng 13 MB RAM tmpfs bị kẹt trong deleted inode của `tee`, khôi phục `MemAvailable` lên 25+ MB, và sửa `web_config_server` đọc đúng log realtime để triệt tiêu cảnh báo `RAM_LOW`, `CAM0_NO_FRAME`, `CAM1_NO_FRAME`.
  - **WebConfig UI & Persistent Settings**: Bổ sung thẻ cấu hình Chính sách Nguồn & ACC, bảo toàn qua nâng cấp firmware và reboot.
