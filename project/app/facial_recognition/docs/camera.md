# Camera bring-up

The local `sc3336.c` driver declares `smartsens,sc3336` and supports 2304x1296
RAW10 at 25 or 30 fps. These are sensor modes; 1920x1080, 1280x720, and
640x360 are prospective stream outputs and must be scaled by a verified media
block rather than selected as non-existent sensor modes.

For the declared Luckfox Pico Pro Max target, the official Luckfox DTS provides
a concrete baseline: SC3336 is on I2C4 at `0x30`, uses `MCLK_REF_MIPI0`, has
PWDN on GPIO3_C5 (active high), and connects through lanes 1 and 2. It also
selects the CMK-OT2119-PC1 / 30IRC-F16 module identity, matching the SC3336 IQ
file present in this SDK.

Those values are an external board reference, not proof that this checkout has
the same pinctrl labels, base DTS dependencies, or deployed hardware revision.
Before application code, port/compare the official Pro Max DTS and verify its
supplies, xvclk, reset/PWDN GPIOs, CSI endpoint, and lane mapping on the board.

After boot, capture and attach the output of:

```sh
media-ctl -p
v4l2-ctl --list-devices
v4l2-ctl --all
v4l2-ctl --list-formats-ext -d /dev/videoX
```

Replace `/dev/videoX` only after it is identified in the first two commands.
