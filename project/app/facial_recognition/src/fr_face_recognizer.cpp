#include "fr_face_recognizer.hpp"

#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstring>

namespace fr {

namespace {

inline float Clamp(float val, float min_val, float max_val) {
    return std::max(min_val, std::min(max_val, val));
}

inline float Distance(const Point2f& a, const Point2f& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

FaceRecognizer::FaceRecognizer() = default;
FaceRecognizer::~FaceRecognizer() = default;

bool FaceRecognizer::CheckFaceQuality(const FaceDetection& face, int img_w, int img_h,
                                      std::string* reason) {
    float bw = face.bbox.Width();
    float bh = face.bbox.Height();

    if (face.score < 0.50f) {
        if (reason) *reason = "Độ tin cậy khuôn mặt quá thấp (< 50%)";
        return false;
    }

    if (bw < 50.0f || bh < 50.0f) {
        if (reason) *reason = "Khuôn mặt quá nhỏ (cần đứng gần camera hơn)";
        return false;
    }

    if (face.bbox.x1 < 5.0f || face.bbox.y1 < 5.0f ||
        face.bbox.x2 > img_w - 5.0f || face.bbox.y2 > img_h - 5.0f) {
        if (reason) *reason = "Khuôn mặt bị cắt một phần ở viền khung hình";
        return false;
    }

    // Check landmark sanity (0: left eye, 1: right eye, 2: nose, 3: left mouth, 4: right mouth)
    float eye_dist = Distance(face.landmarks[0], face.landmarks[1]);
    if (eye_dist < 15.0f) {
        if (reason) *reason = "Khoảng cách hai mắt không rõ ràng";
        return false;
    }

    // Angle roll check: atan2(dy, dx)
    float dy = face.landmarks[1].y - face.landmarks[0].y;
    float dx = face.landmarks[1].x - face.landmarks[0].x;
    float angle = std::atan2(dy, dx) * 180.0f / 3.14159265f;
    if (std::abs(angle) > 60.0f) {
        if (reason) *reason = "Đầu bị nghiêng quá nhiều (> 60 độ)";
        return false;
    }

    return true;
}

bool FaceRecognizer::AlignAndCropFace(const uint8_t* rgb_data, int width, int height,
                                      const FaceDetection& face,
                                      std::vector<uint8_t>& out_cropped_rgb,
                                      int out_w, int out_h) {
    if (!rgb_data || width <= 0 || height <= 0) return false;

    out_cropped_rgb.resize(out_w * out_h * 3, 0);

    // Bounding box with margin
    float margin_x = face.bbox.Width() * 0.15f;
    float margin_y = face.bbox.Height() * 0.15f;
    float src_x1 = std::max(0.0f, face.bbox.x1 - margin_x);
    float src_y1 = std::max(0.0f, face.bbox.y1 - margin_y);
    float src_x2 = std::min(static_cast<float>(width - 1), face.bbox.x2 + margin_x);
    float src_y2 = std::min(static_cast<float>(height - 1), face.bbox.y2 + margin_y);

    float src_w = src_x2 - src_x1;
    float src_h = src_y2 - src_y1;
    if (src_w <= 0.0f || src_h <= 0.0f) return false;

    // Resample cropped face to (out_w x out_h)
    for (int y = 0; y < out_h; ++y) {
        float sy = src_y1 + (y * src_h / static_cast<float>(out_h));
        int iy = static_cast<int>(Clamp(sy, 0.0f, static_cast<float>(height - 1)));
        for (int x = 0; x < out_w; ++x) {
            float sx = src_x1 + (x * src_w / static_cast<float>(out_w));
            int ix = static_cast<int>(Clamp(sx, 0.0f, static_cast<float>(width - 1)));

            int src_idx = (iy * width + ix) * 3;
            int dst_idx = (y * out_w + x) * 3;

            out_cropped_rgb[dst_idx + 0] = rgb_data[src_idx + 0];
            out_cropped_rgb[dst_idx + 1] = rgb_data[src_idx + 1];
            out_cropped_rgb[dst_idx + 2] = rgb_data[src_idx + 2];
        }
    }

    return true;
}

bool FaceRecognizer::ExtractFeature(const uint8_t* rgb_data, int width, int height,
                                    const FaceDetection& face,
                                    FaceFeature& out_feature) {
    if (!rgb_data || width <= 0 || height <= 0) return false;

    out_feature.data.assign(kFeatureDim, 0.0f);

    // 1. Geometric Landmark Ratios (Dimensions 0..31)
    const auto& lm = face.landmarks;
    float eye_dist = std::max(1.0f, Distance(lm[0], lm[1]));
    float mouth_dist = std::max(1.0f, Distance(lm[3], lm[4]));
    Point2f eye_center{(lm[0].x + lm[1].x) * 0.5f, (lm[0].y + lm[1].y) * 0.5f};
    Point2f mouth_center{(lm[3].x + lm[4].x) * 0.5f, (lm[3].y + lm[4].y) * 0.5f};

    float nose_to_eye = Distance(lm[2], eye_center);
    float nose_to_mouth = Distance(lm[2], mouth_center);
    float face_h = std::max(1.0f, face.bbox.Height());
    float face_w = std::max(1.0f, face.bbox.Width());

    out_feature.data[0] = eye_dist / face_w;
    out_feature.data[1] = mouth_dist / face_w;
    out_feature.data[2] = mouth_dist / eye_dist;
    out_feature.data[3] = nose_to_eye / eye_dist;
    out_feature.data[4] = nose_to_mouth / eye_dist;
    out_feature.data[5] = Distance(lm[0], lm[2]) / eye_dist;
    out_feature.data[6] = Distance(lm[1], lm[2]) / eye_dist;
    out_feature.data[7] = Distance(lm[0], lm[3]) / eye_dist;
    out_feature.data[8] = Distance(lm[1], lm[4]) / eye_dist;
    out_feature.data[9] = (eye_center.y - face.bbox.y1) / face_h;
    out_feature.data[10] = (lm[2].y - face.bbox.y1) / face_h;
    out_feature.data[11] = (mouth_center.y - face.bbox.y1) / face_h;
    out_feature.data[12] = (lm[2].x - face.bbox.x1) / face_w;
    out_feature.data[13] = face_w / face_h;

    // Cross ratios
    out_feature.data[14] = Distance(lm[0], lm[4]) / eye_dist;
    out_feature.data[15] = Distance(lm[1], lm[3]) / eye_dist;

    // Relative landmark coordinates normalized to face bbox
    for (int k = 0; k < 5; ++k) {
        out_feature.data[16 + k * 2] = (lm[k].x - face.bbox.x1) / face_w;
        out_feature.data[17 + k * 2] = (lm[k].y - face.bbox.y1) / face_h;
    }
    out_feature.data[26] = (eye_center.x - face.bbox.x1) / face_w;
    out_feature.data[27] = (eye_center.y - face.bbox.y1) / face_h;
    out_feature.data[28] = (mouth_center.x - face.bbox.x1) / face_w;
    out_feature.data[29] = (mouth_center.y - face.bbox.y1) / face_h;
    out_feature.data[30] = std::abs(Distance(lm[0], lm[2]) - Distance(lm[1], lm[2])) / eye_dist;
    out_feature.data[31] = std::abs(Distance(lm[3], lm[2]) - Distance(lm[4], lm[2])) / mouth_dist;

    // 2. Spatial Patch Texture & Intensity Descriptors (Dimensions 32..127)
    std::vector<uint8_t> aligned_face;
    if (AlignAndCropFace(rgb_data, width, height, face, aligned_face, 64, 64)) {
        // Divide 64x64 into 4x4 blocks (16 blocks, each 16x16)
        // Extract block average luminance, horizontal gradient, vertical gradient, and standard deviation
        size_t feat_idx = 32;
        for (int by = 0; by < 4 && feat_idx < kFeatureDim; ++by) {
            for (int bx = 0; bx < 4 && feat_idx < kFeatureDim; ++bx) {
                float sum_lum = 0.0f;
                float sum_dx = 0.0f;
                float sum_dy = 0.0f;
                float sum_sq = 0.0f;
                int count = 0;

                for (int y = by * 16; y < (by + 1) * 16; ++y) {
                    for (int x = bx * 16; x < (bx + 1) * 16; ++x) {
                        int idx = (y * 64 + x) * 3;
                        // Gray luminance: 0.299R + 0.587G + 0.114B
                        float gray = aligned_face[idx] * 0.299f +
                                     aligned_face[idx + 1] * 0.587f +
                                     aligned_face[idx + 2] * 0.114f;
                        sum_lum += gray;
                        sum_sq += gray * gray;

                        if (x > 0 && x < 63) {
                            int idx_l = (y * 64 + x - 1) * 3;
                            int idx_r = (y * 64 + x + 1) * 3;
                            float gl = aligned_face[idx_l] * 0.299f + aligned_face[idx_l + 1] * 0.587f + aligned_face[idx_l + 2] * 0.114f;
                            float gr = aligned_face[idx_r] * 0.299f + aligned_face[idx_r + 1] * 0.587f + aligned_face[idx_r + 2] * 0.114f;
                            sum_dx += std::abs(gr - gl);
                        }
                        if (y > 0 && y < 63) {
                            int idx_u = ((y - 1) * 64 + x) * 3;
                            int idx_d = ((y + 1) * 64 + x) * 3;
                            float gu = aligned_face[idx_u] * 0.299f + aligned_face[idx_u + 1] * 0.587f + aligned_face[idx_u + 2] * 0.114f;
                            float gd = aligned_face[idx_d] * 0.299f + aligned_face[idx_d + 1] * 0.587f + aligned_face[idx_d + 2] * 0.114f;
                            sum_dy += std::abs(gd - gu);
                        }
                        ++count;
                    }
                }

                float mean_lum = count > 0 ? (sum_lum / count) / 255.0f : 0.0f;
                float mean_dx = count > 0 ? (sum_dx / count) / 255.0f : 0.0f;
                float mean_dy = count > 0 ? (sum_dy / count) / 255.0f : 0.0f;
                float variance = count > 0 ? ((sum_sq / count) - (sum_lum / count) * (sum_lum / count)) / (255.0f * 255.0f) : 0.0f;
                float stddev = variance > 0.0f ? std::sqrt(variance) : 0.0f;

                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = mean_lum;
                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = mean_dx;
                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = mean_dy;
                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = stddev;
                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = (mean_dx + mean_dy) * 0.5f;
                if (feat_idx < kFeatureDim) out_feature.data[feat_idx++] = std::abs(mean_dx - mean_dy);
            }
        }
    }

    // L2-Normalize the 128-D vector
    float norm_sq = 0.0f;
    for (float v : out_feature.data) {
        norm_sq += v * v;
    }
    float norm = std::sqrt(norm_sq);
    if (norm > 1e-6f) {
        for (float& v : out_feature.data) {
            v /= norm;
        }
    }

    return true;
}

float FaceRecognizer::ComputeSimilarity(const FaceFeature& a, const FaceFeature& b) {
    if (!a.IsValid() || !b.IsValid()) return 0.0f;

    float dot = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (size_t i = 0; i < kFeatureDim; ++i) {
        dot += a.data[i] * b.data[i];
        norm_a += a.data[i] * a.data[i];
        norm_b += b.data[i] * b.data[i];
    }

    float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    if (denom <= 1e-6f) return 0.0f;

    float sim = dot / denom;
    return Clamp(sim, -1.0f, 1.0f);
}

}  // namespace fr
