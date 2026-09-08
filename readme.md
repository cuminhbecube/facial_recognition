# LubanCat RV06_03 SDK

* This SDK is modified based on the SDK provided by Rockchip
* It provides a customized SDK specifically for LubanCat development boards 
* Aimed at providing developers with a better programming experience

## SDK Usage Instructions

* recommended operating system : Ubuntu 22.04,Ubuntu 20.04,Debian 11,Debian 12

### Installing Dependencies

```shell
sudo apt-get install -y git ssh make gcc gcc-multilib g++-multilib module-assistant expect g++ gawk texinfo libssl-dev bison flex fakeroot cmake unzip gperf autoconf device-tree-compiler libncurses5-dev pkg-config bc python-is-python3 passwd openssl openssh-server openssh-client vim file cpio rsync
```

### Get SDK

```shell
git clone https://github.com/LubanCat/RV06_03_Linux_SDK.git
```

### Environment Variables

* The cross-compilation toolchain needs to be set Environment Variables

```shell
cd {SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/
source env_install_toolchain.sh
```

### Instructions for build.sh

* The build.sh script is used to automate the compilation process. 
* Most of the compilation operations can be completed automatically through build.sh.

#### Options for build.sh

```shell
Usage: build.sh [OPTIONS]
Available options:
lunch              -Select Board Configure
env                -build env
meta               -build meta (optional)
uboot              -build uboot
kernel             -build kernel
kconfig            -modify kernel defconfig
rootfs             -build rootfs
driver             -build kernel's drivers
sysdrv             -build uboot, kernel, rootfs
brconfig           -modify buildroot defconfig
buildroot          -build buildroot
bbconfig           -modify busybox defconfig
busybox            -build busybox
media              -build rockchip media libraries
app                -build app
recovery           -build recovery
tool               -build tool
updateimg          -build update image
unpackimg          -unpack update image
factory            -build factory image
all                -build uboot, kernel, rootfs, recovery image
allsave            -build all & firmware & save

clean              -clean all
clean uboot        -clean uboot
clean kernel       -clean kernel
clean driver       -clean driver
clean rootfs       -clean rootfs
clean sysdrv       -clean uboot/kernel/rootfs
clean media        -clean rockchip media libraries
clean app          -clean app
clean recovery     -clean recovery

firmware           -pack all the image we need to boot up system
ota                -pack update_ota.tar
save               -save images, patches, commands used to debug
check              -check the environment of building
info               -see the current board building information
```

#### Select the referenced board configuration

```shell
./build.sh lunch
```

+ Output the corresponding LubanCat hardware model. Enter the corresponding number to proceed to the storage media options (press Enter to select option [0] directly).

  ```shell
  You're building on Linux
  Lunch menu...pick a combo:

  BoardConfig-*.mk naming rules:
  BoardConfig-"启动介质"-"电源方案"-"硬件版本"-"应用场景".mk
  BoardConfig-"boot medium"-"power solution"-"hardware version"-"application".mk

  ----------------------------------------------------------------
  1. BoardConfig_IPC/BoardConfig-SD_CARD-NONE-RV1106_LubanCat-RV06.mk
                              boot medium(启动介质): SD_CARD
                            power solution(电源方案): NONE
                          hardware version(硬件版本): RV1106_LubanCat
                              application(应用场景): RV06
  ----------------------------------------------------------------

  ----------------------------------------------------------------
  1. BoardConfig_IPC/BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
                              boot medium(启动介质): SPI_NAND
                            power solution(电源方案): NONE
                          hardware version(硬件版本): RV1106_LubanCat
                              application(应用场景): RV06
  ----------------------------------------------------------------

  Which would you like? [0]: 
  ```

  Enter the corresponding board support file number to complete the configuration.

#### One-click Automatic Compilation

```shell
./build.sh lunch   # Select the reference board configuration
./build.sh         # One-click automatic compilation
```

#### Build U-Boot

```shell
./build.sh clean uboot
./build.sh uboot
```

The path of the generated files:

```shell
output/image/MiniLoaderAll.bin
output/image/uboot.img
```

#### Build kernel

```shell
./build.sh clean kernel
./build.sh kernel
```

The path of the generated files:

```shell
output/image/boot.img
```

#### Build rootfs

```shell
./build.sh clean rootfs
./build.sh rootfs
```

* Note : After compilation, use the command ./build.sh firmware to repackage.

#### Build media

```shell
./build.sh clean media
./build.sh media
```

The path of the generated files:

```shell
output/out/media_out
```

* Note : After compilation, use the command ./build.sh firmware to repackage.

#### Build Reference Applications

```shell
./build.sh clean app
./build.sh app
```

* Note 1: The app depends on media.
* Note 2: After compilation, use the command ./build.sh firmware to repackage.

#### Firmware Packaging

```shell
./build.sh firmware
```

The path of the generated files:

```shell
output/image
```

#### Kernel Config

```shell
./build.sh kconfig
```

Open the menuconfig interface for the kernel.

#### Buildroot Config

```shell
./build.sh brconfig
```

Open the menuconfig interface for buildroot.

* Note: This is only applicable when selecting buildroot as the root file system.

## Notices

When copying the source code package under Windows, the executable file under Linux may become a non-executable file, or the soft link fails and cannot be compiled and used.
Therefore, please be careful not to copy the source code package under Windows.
