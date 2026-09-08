#!/bin/bash
set -eE

NOW_DATE="`date +%Y%m%d`"

# 每次编译前清理
function sdk_clean {
    rm -rf output
}

# sd镜像复制到release前整理
function sd_image {
    local release_path=output/release
    mkdir -p $release_path
    cp -v output/image/* $release_path
    rm -rf $release_path/update.img
}

# nand镜像复制到release前整理
function nand_image {
    local release_path=output/release
    mkdir -p $release_path/分区烧录
    cp -v output/image/* $release_path/分区烧录
    rm -rf $release_path/分区烧录/rootfs*.ubi
    mv $release_path/分区烧录/update.img $release_path
}

function build {
    # 选择配置文件
    ./build.sh lunch $1

    # 每次编译前清理
    sdk_clean

    # 编译构建
    ./build.sh

    # 清理要复制到release的文件
    if [ $(echo $1 | grep SD_CARD) ]; then
        BOOT_MEDIUM=sd_card
        sd_image
    elif [ $(echo $1 | grep SPI_NAND) ]; then
        BOOT_MEDIUM=spi_nand
        nand_image
    fi

    # release路径整理
    BOARD_NAME=$(echo $1 | awk -F'_' '{print $3}' | awk -F'.' '{print $1}')

    # 判断是不是10寸屏
    if [ $(echo $1 | grep "10-inch") ]; then
        RELEASE_DIR=output-release/$BOARD_NAME/buildroot镜像/$NOW_DATE/${BOOT_MEDIUM}/10_inch
    else
        RELEASE_DIR=output-release/$BOARD_NAME/buildroot镜像/$NOW_DATE/${BOOT_MEDIUM}
    fi

    # 复制镜像文件到release目录
    mkdir -p $RELEASE_DIR
    cp -rfv output/release/* $RELEASE_DIR/
}

# 清理上次构建的release目录
rm -rf output-release

# 按配置文件构建镜像
build BoardConfig-SD_CARD-NONE-RV1106_LubanCat-RV06.mk
build BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
build BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06_10-inch.mk
