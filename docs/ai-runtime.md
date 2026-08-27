# AI runtime verification

## Scope

`fr-rknn-inspect` is a diagnostic utility. It loads a caller-supplied RKNN
model once, queries RKNN runtime/model metadata, and destroys the context. It
does not create a camera pipeline, submit an inference, or start an AI service.

The utility queries `RKNN_QUERY_SDK_VERSION`, `RKNN_QUERY_IN_OUT_NUM`, input
and output attributes, plus their native attributes. A query that the deployed
runtime does not support is reported as unavailable; it is not inferred.

## SDK evidence

- Header: `project/app/capture_ai/3rdparty/rknpu2/include/rknn_api.h`.
- Runtime library: `project/app/capture_ai/3rdparty/rknpu2/lib/librknnmrt.so`.
- The target was observed with `/dev/rknpu`, `/oem/usr/lib/librknnmrt.so`,
  `/oem/usr/lib/librga.so`, and Rockit VPSS libraries.

## Target procedure

Copy a reviewed model temporarily, then run:

```sh
LD_LIBRARY_PATH=/oem/usr/lib /oem/usr/bin/fr-rknn-inspect /tmp/model.rknn
```

## Target result — 2026-08-27

The command completed on Luckfox Pico Pro Max at `192.168.1.233` using a
temporary copy of the candidate detector:

```text
RKNN API Version    : 1.6.0 (2de554906@2024-01-17T14:53:41)
RKNN Driver Version : 0.9.2
Inputs : 1
Outputs: 3
input[0] : images, [1, 640, 640, 3], NHWC, INT8, AFFINE,
           scale=0.00392157, zero_point=-128
output[0]: output0, [1, 48, 80, 80], NCHW, INT8, AFFINE,
           scale=0.11971, zero_point=68
output[1]: 952, [1, 48, 40, 40], NCHW, INT8, AFFINE,
           scale=0.111825, zero_point=58
output[2]: 953, [1, 48, 20, 20], NCHW, INT8, AFFINE,
           scale=0.123553, zero_point=93
```

The native outputs are `NC1HWC2`; application output queries are NCHW. The
model initializes successfully with this runtime, but no inference has been
submitted yet. Therefore detector correctness, output semantics, landmarks,
latency and memory stability remain **NOT VERIFIED**.
