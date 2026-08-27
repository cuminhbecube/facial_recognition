# AI testing

## Completed

| Test | Target result | Scope |
| --- | --- | --- |
| RKNN model initialization | Passed | Candidate detector model loaded on RV1106. |
| Tensor metadata query | Passed | One 640x640x3 input and three INT8 outputs recorded. |
| NPU-only execution | Passed | 100 synthetic-tensor inferences, 102.353 ms average / 9.77 FPS. |
| RTSP/Web regression | Passed | Both processes remained running after the benchmark. |

## Not tested

No camera frame has been submitted to the detector and no result has been
decoded. Consequently detection accuracy, bbox/landmark semantics, real AI FPS,
VPSS/RGA timing, CPU/RAM trends, queue behavior, leaks and long-run stability
are **NOT VERIFIED**.

Do not treat the NPU-only benchmark as face detection, recognition, liveness or
facial access control. The test model is temporary and not shipped in the
Facial Recognition package.
