# Build and firmware integration

The dedicated profile is
`BoardConfig-SPI_NAND-NONE-RV1106_Luckfox_Pico_Pro_Max-FACIAL_RECOGNITION.mk`.
It uses the official Pro Max SPI-NAND partition layout and contains only the
SC3336 IQ file. The first build should be performed with:

```sh
cd RV06_03_Linux_SDK
./build.sh lunch project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-NONE-RV1106_Luckfox_Pico_Pro_Max-FACIAL_RECOGNITION.mk
./build.sh media
./build.sh app
./build.sh rootfs
./build.sh firmware
```

`./build.sh` also documents `all`, `allsave`, `check`, and `info`. Do not flash
an image until its generated partition table and camera media graph have been
reviewed on the physical device.

The project app makefile discovers subdirectories with Makefiles, so this
dedicated component can be integrated later without touching dashcam sources.
Its package layout, init service, and rootfs destination remain **NOT
VERIFIED** until the component build is designed against the selected board.
