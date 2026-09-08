# RV06 Driver UART Protocol

## RFID mode retained by firmware 0.6.0

The firmware 0.6.0 baseline retains the dedicated `/dev/ttyS3` RFID UART at 115200
8N1. Card input is push/event-driven: production no longer polls `READ_CARD`.
The RX loop continuously drains a persistent stream parser, accepts unsolicited
partial/combined frames, remains active during ACC OFF, and debounces duplicate
cards for 500 ms. Accepted cards generate LOGIN/LOGOUT/SWITCH and JT808
`0x0702`; reader LED FLASH is the hardware value `0x13`.

The physical outputs are POWER, NET, GPS, DRIVER, REC and BUZZER. Alarm and
CAM0..CAM3 fields are unused and forced to zero. The historical DRV1/debug
UART notes below remain for the local JSON-line test interface and do not
describe the production RFID card RX path.

Firmware 0.4.4 temporarily uses the RV06 debug UART `/dev/ttyFIQ0` for DRV1.
The launcher sets `RV06_DRIVER_UART_DEVICE=/dev/ttyFIQ0`; the manager accepts
only that path or the dedicated fallback `/dev/ttyS3`. Settings are `115200`,
8 data bits, no parity, one stop bit, and no flow control.

The rootfs console getty is disabled so it cannot consume DRV1 RX bytes. The
kernel console remains on `ttyFIQ0`, therefore boot/kernel text and DRV1 ACK
lines share the debug UART TX stream during this temporary mode. A controller
must select complete newline-delimited objects whose `protocol` is `DRV1`.
Restore the getty and select `/dev/ttyS3` when the dedicated driver UART is
available again.

Each UTF-8 message is one JSON object followed by `\n`. For temporary debug
terminal compatibility, the receiver also accepts `\r`, `\r\n`, or a complete
JSON object followed by 250 ms without another byte. The fixed DRV1 schema
contains exactly `protocol`, `id`, `event`, `driver_name`, and `license_no`.
`id` is a nonzero unsigned integer. `license_no` is always a string containing
6 through 20 digits.

Login:

```json
{"protocol":"DRV1","id":1001,"event":"driver_login","driver_name":"Nguyen Van An","license_no":"012345678901"}
```

Logout:

```json
{"protocol":"DRV1","id":1002,"event":"driver_logout","driver_name":"Nguyen Van An","license_no":"012345678901"}
```

Accepted messages receive:

```json
{"protocol":"DRV1","ack":1001,"status":"ok"}
```

Malformed messages receive `status=error` and a bounded error code. The parser
rejects comments, duplicate JSON keys, trailing data, unknown fields, invalid
UTF-8, unsupported name characters, numeric license values, and overlong
lines. A bounded 64-ID replay window acknowledges a retransmission without
applying its state a second time.

The active name and license are held only in process RAM. Logout clears both.
The 5x7 OSD rasterizer converts supported precomposed Vietnamese letters to
uppercase ASCII for display; protocol state retains the original UTF-8 name.
No identity value is written to the recorder log.

CAM0 renders `CH1 | <vehicle plate> | <driver>` and CAM1 renders
`CH2 | <vehicle plate> | <driver>` in their existing VENC Regions. When logged
out, the driver suffix is omitted. A login/logout only copies a
mutex-protected snapshot in the OSD worker and calls `RK_MPI_RGN_SetBitMap` on
the existing Region. It does not restart VI, ISP, VENC, recorder, RTSP, or
close the active recording segment.

UART open/read/write errors are isolated in the manager thread. It retries the
device every two seconds while the camera pipeline continues running.

The receiver logs the received frame byte count and terminator, but never logs
the driver name or license number. If the manager reports `state=READY` and no
subsequent `state=RX` after a PC transmit, no byte reached the Linux TTY; check
USB-UART TX-to-board-RX wiring, common ground, 3.3 V TTL level, and the selected
COM port before changing the JSON parser or OSD.

## Debug-only OSD injection

Firmware `0.4.4-debug` enables a root-only local test path so the active OSD can
be verified before the physical UART RX path is available. Create
`/tmp/dashcam/driver-uart-test.json` as a root-owned mode-0600 regular file. The
UART manager removes it, passes its contents through the same `ProcessLine()`
parser and replay protection as UART input, and writes the protocol response to
`/tmp/dashcam/driver-uart-test.ack`.

```sh
umask 077
printf '%s\n' '{"protocol":"DRV1","id":44001,"event":"driver_login","driver_name":"Nguyen Van An","license_no":"012345678901"}' > /tmp/dashcam/driver-uart-test.json
sleep 1
cat /tmp/dashcam/driver-uart-test.ack
grep -E 'TEST_INJECT|driver_bitmap_update' /tmp/dashcam/recorder.log | tail
```

The expected video identity rows are `CH1 | <plate> | NGUYEN VAN AN` on CAM0
and `CH2 | <plate> | NGUYEN VAN AN` on CAM1. Use a new nonzero `id` for logout.
The test changes only in-memory driver state and existing Region bitmaps. It
does not restart or reconfigure VI, ISP, VENC, recorder, RTSP, or the OSD
Regions. Remove `RV06_DRIVER_UART_TEST_ENABLED=on` from the production launcher
after physical UART validation.

Starting with firmware `0.4.9-debug`, every accepted non-duplicate login or
logout revision is sent through the active JT808 connection as message
`0x0702`. `/tmp/dashcam/jt808.status` and Web Config `/jt808` expose
`driver_state`, `driver_name`, `license_no`, and `driver_sync_state`. Logout
clears the name and license immediately. `ACKED` means CMSV6 returned a
successful platform common response for the exact `0x0702` sequence.
