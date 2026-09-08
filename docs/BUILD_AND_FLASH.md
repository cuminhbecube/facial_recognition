# Build And Flash

## Target

- Board: LubanCat-RV06 / RV1106
- Boot medium: SPI NAND
- Board config: `BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk`
- DTS: `rv1106g-lubancat-rv06.dts`
- Root filesystem: UBIFS

## Build

From the SDK root:

```sh
./build.sh lunch BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
./build.sh
```

Expected output is `output/image/update.img`. The package must also contain
`download.bin`, `env.img`, `idblock.img`, `uboot.img`, `boot.img` and
`rootfs.img`.

After a successful package, the same image is published as:

```text
Upgrade/upgrade-<version>.img
```

The default version is read from `FIRMWARE_VERSION`. Automated builds may set
`RV06_FIRMWARE_VERSION`; only letters, digits, `.`, `_`, and `-` are accepted.

## Flash

Put the board in Rockchip Loader or Maskrom mode, then run:

```sh
tools/linux/Linux_Upgrade_Tool/rkdownload.sh -d output/image
```

If the flash layout is stale, erase it once and retry:

```sh
tools/linux/Linux_Upgrade_Tool/rkdownload.sh -e output/image/download.bin
tools/linux/Linux_Upgrade_Tool/rkdownload.sh -d output/image
```

On Windows, use the Rockchip upgrade tool in Loader/Maskrom mode and flash
`output/image/update.img`.

## Verify package

```sh
mkdir -p /tmp/rv06-update-check
tools/linux/Linux_Pack_Firmware/mk-update_unpack.sh \
  -i output/image/update.img -o /tmp/rv06-update-check
find /tmp/rv06-update-check -maxdepth 2 -type f -ls
```
