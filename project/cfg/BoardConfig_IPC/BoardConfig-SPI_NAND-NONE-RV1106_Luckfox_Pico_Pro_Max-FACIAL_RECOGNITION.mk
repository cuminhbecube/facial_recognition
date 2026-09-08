#!/bin/bash

# Dedicated Facial Recognition firmware for Luckfox Pico Pro Max (RV1106).
# This profile is derived from the official Luckfox SPI-NAND Pro Max layout
# and uses only the SC3336 camera path.
export RK_ARCH=arm
export RK_CHIP=rv1106
export RK_TARGET_ROOTFS=buildroot
export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf
export RK_BOOT_MEDIUM=spi_nand

export RK_UBOOT_DEFCONFIG=rv1106_defconfig
export RK_UBOOT_DEFCONFIG_FRAGMENT=rk-sfc.config
export RK_KERNEL_DEFCONFIG=rv1106_lbc_defconfig
export RK_KERNEL_DEFCONFIG_FRAGMENT=rv1106-pm.config
export RK_KERNEL_DTS=rv1106g-luckfox-pico-pro-max-facial-recognition.dts
export RK_BUILDROOT_DEFCONFIG=rv1106_lbc_defconfig
export RK_MISC=wipe_all-misc.img

# The local SC3336 driver and the selected Pico Pro Max DTS identify this lens.
export RK_CAMERA_SENSOR_IQFILES="sc3336_CMK-OT2119-PC1_30IRC-F16.json"
export RK_CAMERA_SENSOR_CAC_BIN="CAC_sc3336_CMK-OT2119-PC1_30IRC-F16"
export RK_BOOTARGS_CMA_SIZE="66M"

# Official Luckfox Pico Pro Max SPI-NAND partition layout.
export RK_PARTITION_CMD_IN_ENV="256K(env),256K@256K(idblock),512K(uboot),4M(boot),60M(oem),10M(userdata),180M(rootfs)"
export RK_PARTITION_FS_TYPE_CFG=rootfs@IGNORE@ubifs,oem@/oem@ubifs,userdata@/userdata@ubifs

# Build only the standalone Facial Recognition component; never package dashcam.
export RK_FACIAL_RECOGNITION_APP=y
export RK_BUILD_APP_TO_OEM_PARTITION=y
export RK_ENABLE_ROCKCHIP_TEST=y
# This device uses wired Ethernet only. Do not add any Wi-Fi userspace or KOs.
export RK_ENABLE_WIFI=n
export ENABLE_WIFI=n
export RK_ENABLE_ADBD=y
