# Debug checklist

Use these commands on the physical target after the correct SC3336 DTS boots:

```sh
dmesg | grep -iE 'sc3336|rkcif|rkisp|mipi'
media-ctl -p
v4l2-ctl --list-devices
v4l2-ctl --all
mount
df -h
cat /proc/meminfo
```

Record the entire media graph, active format, FPS, V4L2 nodes, sensor probe
messages, and memory availability. `i2cdetect`, `i2cget`, and `i2cset` must
only be run when the board schematic identifies the relevant bus/device and
when probing is safe for the camera.
