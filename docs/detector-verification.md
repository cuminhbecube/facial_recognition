# Detector verification (`fr-detector-verify`)

This checks the two questions that `docs/face-detector.md` and
`docs/ai-runtime.md` left open about the YOLOv5n-face RKNN detector: whether the
runtime actually exposes the output layout the decoder assumes, and whether
decoded detections are geometrically plausible.

`fr-detector-verify` is a diagnostic utility only. It never runs face
recognition or enrollment and never writes the face database or `/tmp/fr_ai_*`.

## What the production decoder assumes

`fr_face_detector.cpp::Detect()` binds zero-copy output memory, then indexes the
flat `int8` buffer as:

```text
base_idx = 48 * (i * grid_w + j) + 16 * anchor
  [0..3]   box dx,dy,dw,dh (sigmoid, anchor-scaled)
  [4]      objectness        (sigmoid)
  [5..14]  5 landmarks x,y   (anchor/grid-scaled)
  [15]     class             (sigmoid)
```

That is exactly an **NHWC `[1, grid, grid, 48]`** tensor with 48 contiguous
bytes per grid cell (`48 = 3 anchors * 16`). It is identical to the SDK vendor
demo `project/app/capture_ai/main.c::process_output_layer()`, so the decode
formula itself is not novel. The open question is whether
`RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR` returns that layout on the running target.

`docs/ai-runtime.md` recorded only the **application** outputs
(`[1,48,80,80]` NCHW) and the native `NC1HWC2` outputs. `NC1HWC2` does **not**
store the 48 channels of a pixel contiguously: channels of pixel `(i,j)` are
scattered across 24 separate planes, so the same flat indexing decodes garbage.
That is why this tool must record what the NHWC query actually returns.

## PASS/FAIL criteria

A row is `[PASS]` only when:

1. **Layout contract.** For every output, `RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR`
   returns `fmt == NHWC`, `n_dims == 4`, `dims == [1, grid, grid, 48]`, and
   `size_with_stride >= grid*grid*48`. If `fmt` is `NC1HWC2` or `dims[3] != 48`,
   the decoder is reading the wrong layout and **no detection can be trusted**.
2. **Decode agreement.** The tool's own low-level decode and
   `fr::FaceDetector::Detect` return the same boxes (`IoU >= 0.8`).
3. **Plausible geometry.** Every kept box is inside 640x640, all five landmarks
   are inside its box, and the landmark order is eyes above nose above mouth.

Feeding only the synthetic pattern cannot produce a meaningful verdict about
recognition; a real face photo is required to judge detection.

## Build

The binary is part of the component build (`make`), or standalone:

```sh
CROSS=arm-rockchip830-linux-uclibcgnueabihf-g++
RKNN=project/app/capture_ai/3rdparty/rknpu2
$CROSS -O2 -std=c++17 -Isrc -I$RKNN/include src/fr_detector_verify.cpp \
    src/fr_face_detector.cpp src/fr_face_recognizer.cpp src/fr_face_db.cpp \
    -o fr-detector-verify -L$RKNN/lib -lrknnmrt -lpthread
```

## Run on target

Copy a reviewed model (the shipped detector candidate is **LICENSE NOT
VERIFIED**, see `docs/face-detector.md`) and the binary, then:

```sh
# 1. Layout contract only (no camera required)
LD_LIBRARY_PATH=/oem/usr/lib ./fr-detector-verify /tmp/model.rknn

# 2. With a real, front-facing photo converted to PPM P6
#    e.g.  ffmpeg -i selfie.jpg -vf scale=640:640 face.ppm
LD_LIBRARY_PATH=/oem/usr/lib ./fr-detector-verify /tmp/model.rknn \
    --image /tmp/face.ppm --dump /tmp/dump

# 3. NPU throughput/stability under load
LD_LIBRARY_PATH=/oem/usr/lib ./fr-detector-verify /tmp/model.rknn \
    --image /tmp/face.ppm --iters 100
```

A clean run on a real face prints, among others:

```text
[PASS] all 3 outputs satisfy the NHWC [1, grid, grid, 48] contract
[PASS] production and reference decoders agree on 1 boxes (IoU>=0.8 required)
[PASS] layout_ok=true decode_geometry=true decoder_agreement=true
```

## Record results back here

Run the steps above on the physical target and record the exact output. Until
step 1 (layout) and step 2 (plausible face) are documented here, detector
correctness remains **NOT VERIFIED** no matter what the NPU-only benchmark
(`fr-rknn-benchmark`) shows.

Result history:

| Date | Model SHA-256 | Layout | Real-face boxes | Notes |
| --- | --- | --- | --- | --- |
| 2026-08-28 | yolov5n-face-rv1106.rknn (4,205,281 B) | **PASS** | **PASS** | On target 192.168.1.233 (RV1106), all 3 outputs NHWC `[1,grid,grid,48]`, `fmt==NHWC`, size ok. Real photos (4160x3120 & 960x960) decoded score 0.865/0.693 with correct eye>nose>mouth geometry; reference & production decoders agree IoU>=0.8. Native input INT8 AFFINE (production override UINT8+NHWC depends on u8->quant conversion — see below). |

**Deployment detail (recorded 2026-08-28):** target native input attr is
`INT8 AFFINE (scale=0.00392157, zp=-128)` while the production
`fr_face_detector.cpp` overrides the input to `UINT8 + NHWC`. Real-photo runs
decode correct geometry, so the runtime's u8->quant bridge behaves as
expected, but the override path is behaviourally verified, not documented by
the SDK (`ai-runtime.md` records only NCHW app outputs and NC1HWC2 native
outputs). Flag as **NOT VERIFIED** if this needs to be proven rather than
observed.