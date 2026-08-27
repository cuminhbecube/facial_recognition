#include "fr_face_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fr {
namespace {

constexpr int kPropBoxSize = 16;  // 4 (bbox) + 1 (obj) + 10 (5 landmarks x 2) + 1 (cls)
constexpr int kAnchor[3][6] = {
    {4, 5, 8, 10, 13, 16},
    {23, 29, 43, 55, 73, 105},
    {146, 217, 231, 300, 335, 433}
};

inline float Sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

inline float Dequantize(int8_t val, int32_t zp, float scale) {
    return (static_cast<float>(val) - static_cast<float>(zp)) * scale;
}

inline float CalculateIoU(const FaceBBox& a, const FaceBBox& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);

    float intersection_w = std::max(0.0f, x2 - x1);
    float intersection_h = std::max(0.0f, y2 - y1);
    float intersection_area = intersection_w * intersection_h;

    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - intersection_area;

    return union_area <= 0.0f ? 0.0f : (intersection_area / union_area);
}

}  // namespace

FaceDetector::FaceDetector() = default;

FaceDetector::~FaceDetector() {
    Release();
}

bool FaceDetector::Init(const std::string& model_path) {
    Release();

    int ret = rknn_init(&context_, const_cast<char*>(model_path.c_str()), 0, 0, nullptr);
    if (ret != RKNN_SUCC) {
        std::fprintf(stderr, "[FaceDetector] rknn_init failed for %s: %d\n", model_path.c_str(), ret);
        return false;
    }

    rknn_input_output_num io_num{};
    ret = rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input != 1 || io_num.n_output < 3) {
        std::fprintf(stderr, "[FaceDetector] unexpected model I/O: input=%u, output=%u\n",
                     io_num.n_input, io_num.n_output);
        Release();
        return false;
    }

    // Configure Input
    input_attr_.index = 0;
    ret = rknn_query(context_, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attr_, sizeof(input_attr_));
    if (ret != RKNN_SUCC) {
        std::fprintf(stderr, "[FaceDetector] rknn_query native input attr failed: %d\n", ret);
        Release();
        return false;
    }

    input_attr_.type = RKNN_TENSOR_UINT8;
    input_attr_.fmt = RKNN_TENSOR_NHWC;
    input_height_ = input_attr_.dims[1];
    input_width_ = input_attr_.dims[2];
    input_channels_ = input_attr_.dims[3];

    input_mem_ = rknn_create_mem(context_, input_attr_.size_with_stride);
    if (!input_mem_ || rknn_set_io_mem(context_, input_mem_, &input_attr_) != RKNN_SUCC) {
        std::fprintf(stderr, "[FaceDetector] failed to create/set input memory\n");
        Release();
        return false;
    }

    // Configure Outputs
    output_attrs_.resize(io_num.n_output);
    output_mems_.resize(io_num.n_output, nullptr);

    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        output_attrs_[i].index = i;
        ret = rknn_query(context_, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            std::fprintf(stderr, "[FaceDetector] rknn_query native NHWC output attr failed for [%u]: %d\n", i, ret);
            Release();
            return false;
        }

        output_mems_[i] = rknn_create_mem(context_, output_attrs_[i].size_with_stride);
        if (!output_mems_[i] || rknn_set_io_mem(context_, output_mems_[i], &output_attrs_[i]) != RKNN_SUCC) {
            std::fprintf(stderr, "[FaceDetector] failed to create/set output memory [%u]\n", i);
            Release();
            return false;
        }
    }

    initialized_ = true;
    std::printf("[FaceDetector] Initialized YOLOv5n-face model successfully (Input: %dx%dx%d)\n",
                input_width_, input_height_, input_channels_);
    return true;
}

void FaceDetector::Release() {
    if (context_ != 0) {
        for (auto* mem : output_mems_) {
            if (mem) rknn_destroy_mem(context_, mem);
        }
        output_mems_.clear();
        output_attrs_.clear();

        if (input_mem_) {
            rknn_destroy_mem(context_, input_mem_);
            input_mem_ = nullptr;
        }
        rknn_destroy(context_);
        context_ = 0;
    }
    initialized_ = false;
}

bool FaceDetector::Detect(const uint8_t* rgb_data, int width, int height,
                          std::vector<FaceDetection>& faces,
                          float threshold, float nms_threshold) {
    faces.clear();
    if (!initialized_ || !rgb_data) return false;

    // Copy RGB data into input memory buffer
    const size_t expected_size = static_cast<size_t>(input_width_) * input_height_ * input_channels_;
    if (width == input_width_ && height == input_height_) {
        std::memcpy(input_mem_->virt_addr, rgb_data, expected_size);
    } else {
        // Fast nearest-neighbor or bilinear resample if sizes differ
        const uint8_t* src = rgb_data;
        uint8_t* dst = static_cast<uint8_t*>(input_mem_->virt_addr);
        for (int y = 0; y < input_height_; ++y) {
            int src_y = y * height / input_height_;
            for (int x = 0; x < input_width_; ++x) {
                int src_x = x * width / input_width_;
                int src_idx = (src_y * width + src_x) * 3;
                int dst_idx = (y * input_width_ + x) * 3;
                dst[dst_idx + 0] = src[src_idx + 0];
                dst[dst_idx + 1] = src[src_idx + 1];
                dst[dst_idx + 2] = src[src_idx + 2];
            }
        }
    }

    // Run NPU Inference
    int ret = rknn_run(context_, nullptr);
    if (ret != RKNN_SUCC) {
        std::fprintf(stderr, "[FaceDetector] rknn_run failed: %d\n", ret);
        return false;
    }

    // Decode candidates from 3 feature pyramids
    std::vector<FaceDetection> candidates;
    candidates.reserve(128);

    for (size_t out_idx = 0; out_idx < 3 && out_idx < output_attrs_.size(); ++out_idx) {
        const auto& attr = output_attrs_[out_idx];
        const int grid_h = attr.dims[1];
        const int grid_w = attr.dims[2];
        const int stride = input_height_ / grid_h;
        const int32_t zp = attr.zp;
        const float scale = attr.scale;
        const auto* layer_data = static_cast<const int8_t*>(output_mems_[out_idx]->virt_addr);

        const int tensor_len = kPropBoxSize * 3;

        for (int a = 0; a < 3; ++a) {
            for (int i = 0; i < grid_h; ++i) {
                for (int j = 0; j < grid_w; ++j) {
                    const int base_idx = tensor_len * (i * grid_w + j) + kPropBoxSize * a;
                    const float obj_conf = Sigmoid(Dequantize(layer_data[base_idx + 4], zp, scale));
                    if (obj_conf < threshold) continue;

                    const float cls_conf = Sigmoid(Dequantize(layer_data[base_idx + 15], zp, scale));
                    const float total_score = obj_conf * cls_conf;
                    if (total_score < threshold) continue;

                    // Decode Bounding Box
                    float box_x = Sigmoid(Dequantize(layer_data[base_idx + 0], zp, scale)) * 2.0f - 0.5f;
                    float box_y = Sigmoid(Dequantize(layer_data[base_idx + 1], zp, scale)) * 2.0f - 0.5f;
                    float box_w = Sigmoid(Dequantize(layer_data[base_idx + 2], zp, scale)) * 2.0f;
                    float box_h = Sigmoid(Dequantize(layer_data[base_idx + 3], zp, scale)) * 2.0f;

                    box_x = (box_x + static_cast<float>(j)) * static_cast<float>(stride);
                    box_y = (box_y + static_cast<float>(i)) * static_cast<float>(stride);
                    box_w = box_w * box_w * static_cast<float>(kAnchor[out_idx][a * 2]);
                    box_h = box_h * box_h * static_cast<float>(kAnchor[out_idx][a * 2 + 1]);

                    FaceDetection det{};
                    det.score = total_score;
                    det.bbox.x1 = std::max(0.0f, box_x - (box_w * 0.5f));
                    det.bbox.y1 = std::max(0.0f, box_y - (box_h * 0.5f));
                    det.bbox.x2 = std::min(static_cast<float>(input_width_), box_x + (box_w * 0.5f));
                    det.bbox.y2 = std::min(static_cast<float>(input_height_), box_y + (box_h * 0.5f));

                    // Decode 5 Facial Landmarks
                    for (int k = 0; k < 5; ++k) {
                        const int lm_idx = base_idx + 5 + k * 2;
                        det.landmarks[k].x = Dequantize(layer_data[lm_idx], zp, scale) *
                                             static_cast<float>(kAnchor[out_idx][a * 2]) +
                                             static_cast<float>(j * stride);
                        det.landmarks[k].y = Dequantize(layer_data[lm_idx + 1], zp, scale) *
                                             static_cast<float>(kAnchor[out_idx][a * 2 + 1]) +
                                             static_cast<float>(i * stride);
                    }

                    candidates.push_back(det);
                }
            }
        }
    }

    if (candidates.empty()) return true;

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(), [](const FaceDetection& a, const FaceDetection& b) {
        return a.score > b.score;
    });

    // NMS
    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        faces.push_back(candidates[i]);
        if (faces.size() >= 32) break;  // limit max detected faces

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j]) continue;
            if (CalculateIoU(candidates[i].bbox, candidates[j].bbox) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }

    return true;
}

}  // namespace fr
