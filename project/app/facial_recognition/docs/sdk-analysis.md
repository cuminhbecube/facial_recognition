# SDK analysis (Phase 0)

Analysis date: 2026-08-26.  Source of truth: local `RV06_03_Linux_SDK`.

## Verified SDK inventory

| Area | Finding | Evidence |
| --- | --- | --- |
| SDK / target | RV06 SDK, `RK_CHIP=rv1106`, ARM/uClibc Buildroot | selected `project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk` |
| Toolchain | `arm-rockchip830-linux-uclibcgnueabihf` | selected BoardConfig |
| Kernel | `rv1106_lbc_defconfig`; `CONFIG_VIDEO_SC3336=y` | `sysdrv/source/kernel/arch/arm/configs/rv1106_lbc_defconfig` |
| SC3336 driver | `drivers/media/i2c/sc3336.c`, compatible `smartsens,sc3336` | kernel driver source |
| SC3336 native modes | 2304x1296 RAW10, 25 fps (27 MHz xvclk) and 30 fps (24 MHz xvclk) | `sc3336.c:supported_modes` |
| SC3336 IQ | `sc3336_CMK-OT2119-PC1_30IRC-F16.{json,bin}` exists | `media/isp/.../isp_iqfiles` |
| Media APIs | Rockit headers and RV1106 VI/VENC/RTSP samples exist | `media/rockit/.../rk_mpi_{vi,venc}.h`, `media/samples/simple_test` |
| RTSP sample | direct `simple_vi_bind_venc_rtsp.c` exists | `media/samples/simple_test` |
| Scaling | RV1106 RGA sample suite and VPSS APIs exist | `media/rga/release_rga_rv1106...`, `rk_mpi_vpss.h` |
| RKNN | `rknn_api.h`, `librknnmrt.so`, and a face-detection sample/model exist | `project/app/capture_ai/3rdparty/rknpu2` |
| RKNN version | Queryable at runtime through `RKNN_QUERY_SDK_VERSION`; exact deployed version is **NOT VERIFIED** | local `rknn_api.h` |
| Debug utilities | `media-ctl` and `v4l2-ctl` are in the built rootfs | `output/out/rootfs_uclibc_rv1106/usr/bin` |
| SQLite | Buildroot has a SQLite package, but target inclusion is **NOT VERIFIED** | `sysdrv/source/buildroot/.../package/sqlite` |

## Luckfox Pico Pro Max reference baseline

The user identified the target hardware as **Luckfox Pico Pro Max**. The
official Luckfox SDK (external to this checkout) supplies
`rv1106g-luckfox-pico-pro-max.dts`, which includes
`rv1106-luckfox-pico-pro-max-ipc.dtsi`. That DTSI contains a complete SC3336
camera node and is the appropriate comparison baseline:

| Item | Official Luckfox DTS value |
| --- | --- |
| Sensor | `smartsens,sc3336` at `sc3336@30` |
| Control bus | `i2c4`, 400 kHz, address `0x30` |
| Sensor clock | `MCLK_REF_MIPI0` (`xvclk`) and `mipi_refclk_out0` pinctrl |
| Sensor control | `pwdn-gpios = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>`; no reset GPIO in this node |
| CSI link | `data-lanes = <1 2>` |
| Media path | SC3336 -> CSI D-PHY -> `mipi0_csi2` -> `rkcif_mipi_lvds` -> `rkisp_vir0` |
| Module / IQ identity | `CMK-OT2119-PC1`, `30IRC-F16` |

Source: [official Luckfox Pico DTS](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-pro-max.dts)
and [its IPC DTSI](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-pro-max-ipc.dtsi).

These values are **verified as the official Pro Max reference**, not yet as the
active configuration or physical wiring of this SDK checkout. They may be used
to select and port a matching board configuration; they must not be pasted into
the current LubanCat DTS without a source-level comparison and target test.

## Current checkout mismatch

The active BoardConfig selects `rv1106g-lubancat-rv06.dts` and packages IQ files
for OV8858 and GC2053. A targeted DTS search found no SC3336 node in that DTS
or its RV1106 DTS references. Therefore the following are **NOT VERIFIED**:

* SC3336 board presence, I2C controller/address, and compatible node;
* CSI host, lane count/order, endpoint connection, clock, reset, PWDN, and
  regulator GPIOs;
* ISP path and runtime `/dev/videoX` nodes;
* whether the SC3336 IQ file is appropriate for the installed lens/module.

Do not modify the current BoardConfig, add a guessed DTS overlay, or hard-code
VI/video-node identifiers. The next safe action is to import/port the official
Luckfox Pro Max DTS and its dependencies for this SDK version, review the diff,
then verify it on the target.

## Reusable references, not reusable product code

`media/samples/simple_test/simple_vi_bind_venc_rtsp.c` and the Rockit MPI
headers are the preferred references for Phase 2/3. `project/app/capture_ai`
proves this SDK ships RKNN and has a YOLOv5 face detector, but it hard-codes a
different 1632x1224 camera setup and display pipeline; it must not be copied
into this product without revalidation.

The existing `project/app/dashcam` and root `src/` application are excluded:
they implement dashcam/vehicle functions and are not dependencies of this
component.

## Phase gates

1. Obtain/identify the SC3336-specific DTS and carrier documentation.
2. Build and flash that configuration; record `media-ctl -p` and V4L2 output.
3. Bring up `SC3336 -> RKCIF -> RKISP -> VI` at a verified native mode.
4. Validate direct `VI -> VENC`, then direct `VI -> VENC -> RTSP`.
5. Only then add bounded AI ingestion and RKNN model selection/benchmarking.
