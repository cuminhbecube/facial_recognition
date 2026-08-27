# Architecture

The component keeps capture/streaming and a future AI branch independent:

```text
SC3336 -> MIPI -> RKCIF -> RKISP -> VI -> VENC -> RTSP
                                      \
                                       -> VPSS/RGA -> Detector -> Quality -> Align
                                                        -> Recognizer -> Embedding
                                                        -> Face database -> Decision
```

The SC3336 media graph and RTSP branch were observed on the target. The AI
branch remains unimplemented. It must consume frames from the camera pipeline,
never by decoding RTSP. Its queue will be bounded and retain the latest frame;
AI overload may drop frames but must not block VI/VENC.

The planned service boundaries are a media service, AI worker, Web/API service,
and a small supervisor. Final process boundaries depend on measured RAM and IPC
cost. RTSP continuity is higher priority than AI and WebConfig.
