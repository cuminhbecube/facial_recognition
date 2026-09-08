# Face detector candidate

## Artifact

- Path: `project/app/capture_ai/model/yolov5n-face-rv1106.rknn`
- Size: 4,205,281 bytes
- SHA-256: `1a2c9368bcf0c076359bdfd0d280ce8909025e13202f2ca805ce790da4cbfde8`
- Embedded strings identify target `rv1106`, source platform `ONNX`, and RKNN
  compiler version `2.3.2` (`2025-04-07`).

## License and suitability

Model origin, source ONNX repository, license and commercial-use permission are
**LICENSE NOT VERIFIED**. It must not be included in a production facial
recognition image or used for access decisions until those facts are supplied
or independently verified.

The existing `capture_ai/main.c` hard-codes YOLO tensor layout, anchors and
thresholds. It is not reused by this component: this project will query the
artifact at runtime and only implement post-processing after its output contract
is verified on RV1106.

## Verified metadata on target

The artifact initializes on the RV1106 target with RKNN API `1.6.0` and driver
`0.9.2`. It exposes one `NHWC INT8` application input, named `images`, with
dimensions `[1, 640, 640, 3]`, affine quantization scale `0.00392157` and zero
point `-128`.

It exposes three `NCHW INT8` application outputs: `[1,48,80,80]`,
`[1,48,40,40]`, and `[1,48,20,20]`. Their native output layout is `NC1HWC2`.
Full scale/zero-point values are in [ai-runtime.md](ai-runtime.md).

The channel semantics have not been verified by inference and a reviewed model
specification. In particular, whether these outputs contain 5 facial landmarks
is **NOT VERIFIED**. No detector post-processing may be added from the existing
hard-coded sample or from an assumed YOLO layout.

## NPU-only execution check

`fr-rknn-benchmark` submits an all-zero tensor matching the queried input
attribute and measures complete RKNN execution. It is a runtime-health test,
not face detection: it does not use a camera frame, preprocessing, output
decoding, score threshold, bbox or landmarks. The candidate model remains
research-only until its provenance and license are verified.

On 2026-08-27, 100 target executions completed in 10,235.320 ms: average
102.353 ms / 9.77 FPS. The raw output buffers contained non-zero bytes. RTSP
and WebConfig processes remained running before and after the test. This proves
only NPU execution for a synthetic tensor; it does not validate a face result,
detector accuracy, memory stability, or production suitability.
