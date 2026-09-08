// fr-detector-verify: on-target verification harness for the YOLOv5n-face
// RKNN detector used by the Facial Recognition component.
//
// This tool is a diagnostic utility, not a product pipeline. It answers two
// questions that the production decoder currently leaves open (see
// docs/face-detector.md and docs/ai-runtime.md):
//
//   1. LAYOUT CONTRACT: does RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR actually return
//      NHWC [1, grid, grid, 48] tensors?  The production decoder in
//      fr_face_detector.cpp indexes the flat output buffer as NHWC with 48
//      contiguous bytes per cell.  If the runtime falls back to NC1HWC2, the
//      per-cell bytes for a pixel are scattered across separate planes and the
//      decode is garbage.  The tool prints the queried attrs and evaluates this
//      against the expected contract.
//
//   2. DECODE SANITY: with that contract satisfied, does detection on a real
//      face image produce plausible boxes (inside 640x640), 5 landmarks inside
//      the box, and geometrically sane landmark order (eyes above nose, nose
//      above mouth)?  The tool runs its own low-level inference session, decodes
//      with the same reference formula used by capture_ai/main.c and
//      fr_face_detector.cpp, and additionally runs fr::FaceDetector on the same
//      image to confirm the two paths agree.
//
// The tool deliberately does NOT do face recognition or enrollment.  It never
// writes /tmp/fr_ai_* or the face database.
//
// Output labels: [PASS]/[FAIL]/[WARN] for machine-parseable verification rows.
//
// Usage:
//   fr-detector-verify <model.rknn> [--image file.ppm] [--generate]
//                      [--threshold 0.45] [--iters 20] [--dump dir]

#include "fr_face_detector.hpp"

#include <rknn_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

inline float Sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

inline float Dequantize(int8_t val, int32_t zp, float scale) {
    return (static_cast<float>(val) - static_cast<float>(zp)) * scale;
}

inline float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

float IoU(const fr::FaceBBox& a, const fr::FaceBBox& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float uni = a.Area() + b.Area() - inter;
    return uni <= 0.0f ? 0.0f : inter / uni;
}

std::string FormatDims(const rknn_tensor_attr& attr) {
    std::string out = "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        out += std::to_string(attr.dims[i]);
        if (i + 1 < attr.n_dims) out += ", ";
    }
    out += "]";
    return out;
}

void PrintTensorAttr(const char* role, uint32_t index, const rknn_tensor_attr& attr) {
    std::printf("%s[%u]: name=%s dims=%s fmt=%s type=%s qnt=%s "
                "scale=%g zp=%d size=%u stride_size=%u\n",
                role, index, attr.name, FormatDims(attr).c_str(),
                get_format_string(attr.fmt), get_type_string(attr.type),
                get_qnt_type_string(attr.qnt_type), attr.scale, attr.zp,
                attr.size, attr.size_with_stride);
}

// Load the PPM P6 image and return a 640x640 RGB888 buffer, resizing with
// nearest-neighbour if needed.  Returns false on any parse error.
bool LoadRgb640(const std::string& path, std::vector<uint8_t>& out_rgb) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return false;
    }
    std::string header;
    for (int i = 0; i < 4; ++i) {
        std::string token;
        f >> token;
        if (token.empty() || (i == 0 && token != "P6")) return false;
        header += token + " ";
    }
    int width = 0, height = 0, maxval = 0;
    std::sscanf(header.c_str(), "P6 %d %d %d", &width, &height, &maxval);
    if (width <= 0 || height <= 0 || maxval <= 0) return false;
    f.get();  // single whitespace after maxval

    std::vector<uint8_t> raw(static_cast<size_t>(width) * height * 3);
    f.read(reinterpret_cast<char*>(raw.data()), raw.size());
    if (f.gcount() != static_cast<std::streamsize>(raw.size())) {
        std::fprintf(stderr, "error: truncated PPM %s\n", path.c_str());
        return false;
    }

    if (width == 640 && height == 640 && maxval == 255) {
        out_rgb = std::move(raw);
        return true;
    }

    out_rgb.assign(640 * 640 * 3, 0);
    for (int y = 0; y < 640; ++y) {
        const int sy = std::min(height - 1, y * height / 640);
        for (int x = 0; x < 640; ++x) {
            const int sx = std::min(width - 1, x * width / 640);
            const int src = (sy * width + sx) * 3;
            const int dst = (y * 640 + x) * 3;
            out_rgb[dst + 0] = static_cast<uint8_t>((raw[src + 0] * 255 + maxval / 2) / maxval);
            out_rgb[dst + 1] = static_cast<uint8_t>((raw[src + 1] * 255 + maxval / 2) / maxval);
            out_rgb[dst + 2] = static_cast<uint8_t>((raw[src + 2] * 255 + maxval / 2) / maxval);
        }
    }
    return true;
}

// Synthetic cartoon "face" test pattern (not a recognition test, only a decode
// exercise).  Mirrors the fallback pattern used by fr_ai_service.cpp.
void GenerateTestFrame(uint8_t* buffer, int frame_idx) {
    const int width = 640, height = 640;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 3;
            buffer[idx + 0] = static_cast<uint8_t>((x * 120 / width) + 30);
            buffer[idx + 1] = static_cast<uint8_t>((y * 120 / height) + 40);
            buffer[idx + 2] = static_cast<uint8_t>(60 + (frame_idx % 20));
        }
    }
    const int cx = width / 2, cy = height / 2, rx = 80, ry = 110;
    for (int y = cy - ry; y <= cy + ry; ++y) {
        if (y < 0 || y >= height) continue;
        for (int x = cx - rx; x <= cx + rx; ++x) {
            if (x < 0 || x >= width) continue;
            float dx = static_cast<float>(x - cx) / rx;
            float dy = static_cast<float>(y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) {
                int idx = (y * width + x) * 3;
                buffer[idx + 0] = 220;
                buffer[idx + 1] = 180;
                buffer[idx + 2] = 150;
            }
        }
    }
    const int eye_y = cy - 25;
    for (int dy = -6; dy <= 6; ++dy) {
        for (int dx = -6; dx <= 6; ++dx) {
            if (dx * dx + dy * dy > 36) continue;
            for (int ex = -30; ex <= 30; ex += 60) {
                int idx = ((eye_y + dy) * width + (cx + ex + dx)) * 3;
                buffer[idx + 0] = buffer[idx + 1] = buffer[idx + 2] = 30;
            }
        }
    }
    for (int x = cx - 25; x <= cx + 25; ++x) {
        for (int dy = -3; dy <= 3; ++dy) {
            int idx = ((cy + 40 + dy) * width + x) * 3;
            buffer[idx + 0] = 180;
            buffer[idx + 1] = 50;
            buffer[idx + 2] = 50;
        }
    }
}

uint64_t MonotonicUs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + now.tv_nsec / 1000ULL;
}

struct IoState {
    // Native-input attr as queried (before the UINT8/NHWC override used in
    // production) so the operator can see the real model quantisation.
    rknn_tensor_attr native_input_attr{};
    std::vector<rknn_tensor_attr> native_outputs;  // RKNN_QUERY_NATIVE_OUTPUT_ATTR
    std::vector<rknn_tensor_attr> nhwc_outputs;    // RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR
};

// Inspect the model metadata without running inference.  Reports every query
// result and evaluates whether the NHWC output contract assumed by the decoder
// is met.  Returns true only if all outputs satisfy the contract.
bool CheckOutputLayout(rknn_context context, IoState& io) {
    rknn_input_output_num num{};
    if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != RKNN_SUCC) {
        std::fprintf(stderr, "error: RKNN_QUERY_IN_OUT_NUM failed\n");
        return false;
    }
    std::printf("=== I/O contract ===\n");
    std::printf("inputs=%u outputs=%u\n\n", num.n_input, num.n_output);

    if (num.n_input != 1 || num.n_output < 3) {
        std::printf("[FAIL] expected 1 input and >=3 outputs, got %u/%u\n",
                    num.n_input, num.n_output);
        return false;
    }

    io.native_input_attr.index = 0;
    const bool native_input_ok =
        rknn_query(context, RKNN_QUERY_NATIVE_INPUT_ATTR, &io.native_input_attr,
                   sizeof(io.native_input_attr)) == RKNN_SUCC;
    if (!native_input_ok) {
        std::printf("[FAIL] RKNN_QUERY_NATIVE_INPUT_ATTR failed\n");
        return false;
    }
    std::printf("native input (as the model expects it, before UINT8 override):\n");
    PrintTensorAttr("input", 0, io.native_input_attr);
    if (io.native_input_attr.type != RKNN_TENSOR_UINT8) {
        std::printf("[WARN] native input type is NOT UINT8 (%s). Production "
                    "code sets type=UINT8 + fmt=NHWC for its zero-copy bind, "
                    "which relies on the runtime doing the u8->quant "
                    "conversion. Confirm the target actually detects faces "
                    "with a real image.\n",
                    get_type_string(io.native_input_attr.type));
    }
    std::printf("\n");

    io.native_outputs.clear();
    io.nhwc_outputs.clear();
    bool all_ok = true;

    for (uint32_t i = 0; i < num.n_output; ++i) {
        rknn_tensor_attr app{};
        app.index = i;
        if (rknn_query(context, RKNN_QUERY_OUTPUT_ATTR, &app, sizeof(app)) == RKNN_SUCC) {
            std::printf("application output[%u] (RKNN_QUERY_OUTPUT_ATTR):\n", i);
            PrintTensorAttr("app_out", i, app);
            std::printf("\n");
        } else {
            std::printf("[WARN] RKNN_QUERY_OUTPUT_ATTR unavailable for output %u\n", i);
        }

        rknn_tensor_attr native{};
        native.index = i;
        const bool native_ok =
            rknn_query(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &native, sizeof(native)) == RKNN_SUCC;
        if (!native_ok) {
            std::printf("[FAIL] RKNN_QUERY_NATIVE_OUTPUT_ATTR failed for output %u\n", i);
            all_ok = false;
            continue;
        }
        io.native_outputs.push_back(native);
        std::printf("native output[%u] (RKNN_QUERY_NATIVE_OUTPUT_ATTR):\n", i);
        PrintTensorAttr("native_out", i, native);
        if (native.fmt != RKNN_TENSOR_NC1HWC2 && native.fmt != RKNN_TENSOR_NHWC) {
            std::printf("[WARN] unexpected native format %s\n", get_format_string(native.fmt));
        }
        std::printf("\n");

        rknn_tensor_attr nhwc{};
        nhwc.index = i;
        const bool nhwc_ok =
            rknn_query(context, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &nhwc, sizeof(nhwc)) == RKNN_SUCC;
        if (!nhwc_ok) {
            std::printf("[FAIL] RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR failed for output %u. "
                        "The zero-copy decoder CANNOT assume NHWC; this runtime does not "
                        "offer the layout it needs.\n", i);
            all_ok = false;
            continue;
        }
        io.nhwc_outputs.push_back(nhwc);
        std::printf("NHWC native output[%u] (RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR):\n", i);
        PrintTensorAttr("nhwc_out", i, nhwc);

        // Expected YOLOv5n-face head contract: [1, grid, grid, 48] NHWC.
        // 48 = 3 anchors * (4 bbox + 1 obj + 10 landmarks + 1 cls), exactly the
        // PROPOSAL size assumed by fr_face_detector.cpp and capture_ai/main.c.
        const int expected_cells = 48;
        const bool dims_ok = nhwc.n_dims == 4 &&
                             nhwc.dims[0] == 1 && nhwc.dims[1] > 0 &&
                             nhwc.dims[2] > 0 && nhwc.dims[3] == static_cast<uint32_t>(expected_cells);
        const bool fmt_ok = nhwc.fmt == RKNN_TENSOR_NHWC;
        const uint32_t expected_linear = static_cast<uint32_t>(nhwc.n_dims == 4
            ? nhwc.dims[1] * nhwc.dims[2] * nhwc.dims[3] : 0);
        const bool size_ok = nhwc.size >= expected_linear && nhwc.size_with_stride >= expected_linear;

        std::printf("  expected NHWC dims [1, grid, grid, %d] -> dims_ok=%s fmt_ok(NHWC)=%s size_ok=%s\n",
                    expected_cells, dims_ok ? "true" : "false", fmt_ok ? "true" : "false",
                    size_ok ? "true" : "false");
        std::printf("  %s\n",
            (dims_ok && fmt_ok && size_ok)
                ? "[PASS] layout matches the decoder's NHWC indexing assumption"
                : (dims_ok && !fmt_ok)
                    ? "[FAIL] dims look right but fmt is not NHWC (%s). Do not trust any detection."
                    : "[WARN] layout contract not fully satisfied - investigate before trusting detections");

        if (!(dims_ok && fmt_ok && size_ok)) all_ok = false;
        std::printf("\n");
    }

    if (all_ok) {
        std::printf("[PASS] all %zu outputs satisfy the NHWC [1, grid, grid, 48] contract\n\n",
                    io.nhwc_outputs.size());
    } else {
        std::printf("[FAIL] output layout contract is NOT satisfied\n\n");
    }
    return all_ok;
}

// Reference decoder that mirrors fr_face_detector.cpp / capture_ai/main.c.
// Kept as an independent copy here so the verification harness stays a
// diagnostic that can run against a bare rknn session; if the production
// decoder changes, this copy must be updated or the comparison degrades.
void DecodeReference(const std::vector<rknn_tensor_attr>& attrs,
                     const std::vector<rknn_tensor_mem*>& mems,
                     const int anchors[3][6],
                     float threshold, float nms_threshold,
                     std::vector<fr::FaceDetection>& faces) {
    faces.clear();
    for (size_t out = 0; out < attrs.size(); ++out) {
        const auto& attr = attrs[out];
        const int grid_h = static_cast<int>(attr.dims[1]);
        const int grid_w = static_cast<int>(attr.dims[2]);
        const int stride = 640 / grid_h;
        const int32_t zp = attr.zp;
        const float scale = attr.scale;
        const auto* data = static_cast<const int8_t*>(mems[out]->virt_addr);
        const int tensor_len = 16 * 3;

        for (int a = 0; a < 3; ++a) {
            for (int i = 0; i < grid_h; ++i) {
                for (int j = 0; j < grid_w; ++j) {
                    const int base = tensor_len * (i * grid_w + j) + 16 * a;
                    const float obj = Sigmoid(Dequantize(data[base + 4], zp, scale));
                    if (obj < threshold) continue;
                    const float cls = Sigmoid(Dequantize(data[base + 15], zp, scale));
                    const float score = obj * cls;
                    if (score < threshold) continue;

                    float bx = Sigmoid(Dequantize(data[base + 0], zp, scale)) * 2.0f - 0.5f;
                    float by = Sigmoid(Dequantize(data[base + 1], zp, scale)) * 2.0f - 0.5f;
                    float bw = Sigmoid(Dequantize(data[base + 2], zp, scale)) * 2.0f;
                    float bh = Sigmoid(Dequantize(data[base + 3], zp, scale)) * 2.0f;

                    const float ax = static_cast<float>(anchors[out][a * 2]);
                    const float ay = static_cast<float>(anchors[out][a * 2 + 1]);
                    bx = (bx + j) * stride;
                    by = (by + i) * stride;
                    bw = bw * bw * ax;
                    bh = bh * bh * ay;

                    fr::FaceDetection det{};
                    det.score = score;
                    det.bbox.x1 = std::max(0.0f, bx - bw * 0.5f);
                    det.bbox.y1 = std::max(0.0f, by - bh * 0.5f);
                    det.bbox.x2 = std::min(640.0f, bx + bw * 0.5f);
                    det.bbox.y2 = std::min(640.0f, by + bh * 0.5f);

                    for (int k = 0; k < 5; ++k) {
                        det.landmarks[k].x =
                            Dequantize(data[base + 5 + k * 2], zp, scale) * ax + j * stride;
                        det.landmarks[k].y =
                            Dequantize(data[base + 5 + k * 2 + 1], zp, scale) * ay + i * stride;
                    }
                    faces.push_back(det);
                }
            }
        }
    }

    std::sort(faces.begin(), faces.end(), [](const fr::FaceDetection& a, const fr::FaceDetection& b) {
        return a.score > b.score;
    });

    std::vector<fr::FaceDetection> kept;
    std::vector<bool> suppressed(faces.size(), false);
    for (size_t i = 0; i < faces.size() && kept.size() < 32; ++i) {
        if (suppressed[i]) continue;
        kept.push_back(faces[i]);
        for (size_t j = i + 1; j < faces.size(); ++j) {
            if (!suppressed[j] && IoU(faces[i].bbox, faces[j].bbox) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }
    faces = std::move(kept);
}

bool RunSanityChecks(const std::vector<fr::FaceDetection>& faces) {
    bool ok = true;
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& f = faces[i];

        const bool box_inside = f.bbox.x1 >= 0.0f && f.bbox.y1 >= 0.0f &&
                                f.bbox.x2 <= 640.0f && f.bbox.y2 <= 640.0f &&
                                f.bbox.Width() > 0.0f && f.bbox.Height() > 0.0f;

        bool lms_inside = true, order_ok = true;
        for (int k = 0; k < 5; ++k) {
            const float x = f.landmarks[k].x, y = f.landmarks[k].y;
            if (x < f.bbox.x1 || x > f.bbox.x2 || y < f.bbox.y1 || y > f.bbox.y2) {
                lms_inside = false;
            }
        }
        // Expected semantics: eye 0/1 above nose 2 above mouth 3/4.
        const float eye_line = (f.landmarks[0].y + f.landmarks[1].y) * 0.5f;
        const float mouth_line = (f.landmarks[3].y + f.landmarks[4].y) * 0.5f;
        if (!(f.landmarks[2].y > eye_line && mouth_line > f.landmarks[2].y)) {
            order_ok = false;
        }

        std::printf("  face[%zu] score=%.3f bbox=(%.1f, %.1f)-%.1fx%.1f "
                    "landmarks=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    i, f.score, f.bbox.x1, f.bbox.y1, f.bbox.Width(), f.bbox.Height(),
                    f.landmarks[0].x, f.landmarks[0].y,
                    f.landmarks[1].x, f.landmarks[1].y,
                    f.landmarks[2].x, f.landmarks[2].y,
                    f.landmarks[3].x, f.landmarks[3].y,
                    f.landmarks[4].x, f.landmarks[4].y);
        std::printf("    box_inside=%s landmarks_inside=%s landmark_order=%s\n",
                    box_inside ? "true" : "false", lms_inside ? "true" : "false",
                    order_ok ? "true" : "false");

        if (!box_inside || !lms_inside || !order_ok) {
            std::printf("[FAIL] impossible face geometry above\n");
            ok = false;
        }
    }
    return ok;
}

uint64_t NonZeroBytes(rknn_tensor_mem* const* mems, const std::vector<rknn_tensor_attr>& attrs) {
    uint64_t total = 0;
    for (size_t i = 0; i < attrs.size(); ++i) {
        const auto* bytes = static_cast<const unsigned char*>(mems[i]->virt_addr);
        for (uint32_t o = 0; bytes && o < attrs[i].size_with_stride; ++o) total += bytes[o] != 0;
    }
    return total;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <model.rknn>\n"
            "  --image <file>    RGB input: PPM P6 (any size) or raw 640x640x3\n"
            "  --generate        use the synthetic cartoon face test pattern\n"
            "  --threshold <t>   confidence threshold (default 0.45)\n"
            "  --iters <N>       inference repetitions for FPS/stability (default 1)\n"
            "  --dump <dir>      write raw output tensors for offline layout analysis\n",
            argv[0]);
        return 64;
    }

    const std::string model_path = argv[1];
    std::string image_path;
    std::string dump_dir;
    bool generate = false;
    float threshold = 0.45f;
    int iters = 1;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc) image_path = argv[++i];
        else if (arg == "--generate") generate = true;
        else if (arg == "--threshold" && i + 1 < argc) threshold = std::atof(argv[++i]);
        else if (arg == "--iters" && i + 1 < argc) iters = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--dump" && i + 1 < argc) dump_dir = argv[++i];
        else { std::fprintf(stderr, "error: unknown option %s\n", arg.c_str()); return 64; }
    }

    std::printf("Facial Recognition detector verification\n");
    std::printf("model=%s image=%s generate=%d threshold=%.2f iters=%d dump=%s\n\n",
                model_path.c_str(),
                image_path.empty() ? "-" : image_path.c_str(), generate ? 1 : 0,
                threshold, iters, dump_dir.empty() ? "-" : dump_dir.c_str());

    // ---- Session A: metadata + own inference (bare rknn) ----
    rknn_context context = 0;
    if (rknn_init(&context, const_cast<char*>(model_path.c_str()), 0, 0, nullptr) != RKNN_SUCC) {
        std::fprintf(stderr, "[FAIL] rknn_init failed for %s\n", model_path.c_str());
        return 1;
    }

    rknn_sdk_version version{};
    if (rknn_query(context, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == RKNN_SUCC) {
        std::printf("RKNN API Version    : %s\n", version.api_version);
        std::printf("RKNN Driver Version : %s\n\n", version.drv_version);
    }

    IoState io;
    const bool layout_ok = CheckOutputLayout(context, io);
    if (io.nhwc_outputs.empty()) {
        rknn_destroy(context);
        return 1;
    }

    // ---- Prepare input ----
    std::vector<uint8_t> rgb(640 * 640 * 3, 0);
    if (!image_path.empty() && !LoadRgb640(image_path, rgb)) {
        std::fprintf(stderr, "[FAIL] could not load %s as RGB\n", image_path.c_str());
        rknn_destroy(context);
        return 2;
    }
    if (image_path.empty() || generate) {
        GenerateTestFrame(rgb.data(), 0);
        std::printf("input: synthetic cartoon test pattern (decode exercise only)\n\n");
    } else {
        std::printf("input: %s resized to 640x640 RGB\n\n", image_path.c_str());
    }

    // Input bind, mirroring production (UINT8/NHWC override).
    rknn_tensor_attr input_attr = io.native_input_attr;
    if (input_attr.type == RKNN_TENSOR_INT8) {
        std::printf("[WARN] native input is INT8 (qnt=%s). The UINT8/NHWC override "
                    "below is what production uses; the target result must be "
                    "confirmed against a real face photo, not this tool alone.\n\n",
                    get_qnt_type_string(input_attr.qnt_type));
    }
    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt = RKNN_TENSOR_NHWC;
    rknn_tensor_mem* input_mem = rknn_create_mem(context, input_attr.size_with_stride);
    std::vector<rknn_tensor_mem*> output_mems(io.nhwc_outputs.size(), nullptr);
    bool setup_ok = input_mem != nullptr &&
                    rknn_set_io_mem(context, input_mem, &input_attr) == RKNN_SUCC;
    for (size_t i = 0; setup_ok && i < io.nhwc_outputs.size(); ++i) {
        output_mems[i] = rknn_create_mem(context, io.nhwc_outputs[i].size_with_stride);
        if (!output_mems[i] ||
            rknn_set_io_mem(context, output_mems[i], &io.nhwc_outputs[i]) != RKNN_SUCC) {
            setup_ok = false;
        }
    }
    if (!setup_ok) {
        std::fprintf(stderr, "[FAIL] RKNN zero-copy setup failed\n");
        for (auto* m : output_mems) if (m) rknn_destroy_mem(context, m);
        if (input_mem) rknn_destroy_mem(context, input_mem);
        rknn_destroy(context);
        return 1;
    }

    // ---- Session B: the same image through the production detector ----
    std::printf("=== production decoder comparison ===\n");
    fr::FaceDetector detector;
    if (!detector.Init(model_path)) {
        std::printf("[WARN] fr::FaceDetector init failed; skipping decoder comparison\n");
    }

    // ---- Inference ----
    const int grid_anchors[3][6] = {
        {4, 5, 8, 10, 13, 16},
        {23, 29, 43, 55, 73, 105},
        {146, 217, 231, 300, 335, 433}
    };
    std::vector<fr::FaceDetection> reference_faces;

    uint64_t total_us = 0;
    for (int iter = 0; iter < iters; ++iter) {
        std::memcpy(input_mem->virt_addr, rgb.data(), std::min<size_t>(rgb.size(), input_attr.size_with_stride));

        const uint64_t t0 = MonotonicUs();
        const int ret = rknn_run(context, nullptr);
        const uint64_t dt = MonotonicUs() - t0;
        total_us += dt;

        if (ret != RKNN_SUCC) {
            std::fprintf(stderr, "[FAIL] rknn_run failed at iter %d (%d)\n", iter, ret);
            if (iter == 0) {
                for (std::vector<rknn_tensor_attr>::size_type i = 0; i < output_mems.size(); ++i) {
                    if (output_mems[i]) rknn_destroy_mem(context, output_mems[i]);
                }
                if (input_mem) rknn_destroy_mem(context, input_mem);
                rknn_destroy(context);
                return 1;
            }
            continue;
        }

        if (iter == 0) {
            reference_faces.clear();
            DecodeReference(io.nhwc_outputs, output_mems, grid_anchors,
                            threshold, 0.45f, reference_faces);
        }
    }

    if (iters > 1) {
        std::printf("infer: %d iters, avg=%.3f ms, fps=%.2f, output_nonzero_bytes=%llu\n\n",
                    iters, total_us / static_cast<double>(iters) / 1000.0,
                    iters * 1000000.0 / static_cast<double>(total_us),
                    static_cast<unsigned long long>(NonZeroBytes(output_mems.data(), io.nhwc_outputs)));
    } else {
        std::printf("output_nonzero_bytes=%llu\n",
                    static_cast<unsigned long long>(NonZeroBytes(output_mems.data(), io.nhwc_outputs)));
    }

    std::printf("\n=== reference decode (mirrors fr_face_detector/capture_ai) ===\n");
    std::printf("faces=%zu\n", reference_faces.size());
    const bool ref_sane = RunSanityChecks(reference_faces);

    std::printf("\n=== fr::FaceDetector decode (production) ===\n");
    std::vector<fr::FaceDetection> prod_faces;
    if (detector.IsInitialized()) {
        detector.Detect(rgb.data(), 640, 640, prod_faces, threshold, 0.45f);
        std::printf("faces=%zu\n", prod_faces.size());
        RunSanityChecks(prod_faces);
    }

    // Consistency between the two decoders.  For each production box find the
    // best IoU among reference boxes; all should be >= 0.8 when layout is OK.
    bool consistent = true;
    for (const auto& p : prod_faces) {
        float best = 0.0f;
        for (const auto& r : reference_faces) best = std::max(best, IoU(p.bbox, r.bbox));
        if (best < 0.8f) consistent = false;
    }
    if (prod_faces.size() == reference_faces.size() && prod_faces.empty()) consistent = true;
    std::printf("\n[%s] production and reference decoders agree on %zu boxes (IoU>=0.8 required)\n",
                consistent ? "PASS" : "FAIL", prod_faces.size());

    // ---- Dump raw tensors for offline layout analysis ----
    if (!dump_dir.empty()) {
        std::printf("dump: writing output tensors to %s\n", dump_dir.c_str());
        for (size_t i = 0; i < output_mems.size(); ++i) {
            const auto& a = io.nhwc_outputs[i];
            const std::string bin = dump_dir + "/out_" + std::to_string(i) + ".bin";
            const std::string txt = dump_dir + "/out_" + std::to_string(i) + ".attr";
            std::FILE* fb = std::fopen(bin.c_str(), "wb");
            if (fb) {
                std::fwrite(output_mems[i]->virt_addr, 1, a.size_with_stride, fb);
                std::fclose(fb);
            }
            std::FILE* ft = std::fopen(txt.c_str(), "w");
            if (ft) {
                std::fprintf(ft, "name=%s dims=%s fmt=%s type=%s qnt=%s scale=%g zp=%d size=%u stride=%u\n",
                             a.name, FormatDims(a).c_str(), get_format_string(a.fmt),
                             get_type_string(a.type), get_qnt_type_string(a.qnt_type),
                             a.scale, a.zp, a.size, a.size_with_stride);
                std::fclose(ft);
            }
        }
    }

    for (auto* m : output_mems) if (m) rknn_destroy_mem(context, m);
    if (input_mem) rknn_destroy_mem(context, input_mem);
    rknn_destroy(context);

    std::printf("\n=== verdict ===\n");
    const bool final = layout_ok && ref_sane && consistent &&
                       (!detector.IsInitialized() || !prod_faces.empty());
    std::printf("[%s] layout_ok=%s decode_geometry=%s decoder_agreement=%s\n",
                final ? (prod_faces.empty() ? "WARN" : "PASS") : "FAIL",
                layout_ok ? "true" : "false", ref_sane ? "true" : "false",
                consistent ? "true" : "false");
    if (final && prod_faces.empty()) {
        std::printf("[WARN] layout and decode agreed but no face found. Feed a real "
                    "face photo (--image) to verify recognition; the machine cannot "
                    "judge faces from the synthetic pattern.\n");
    }
    return final ? 0 : 1;
}