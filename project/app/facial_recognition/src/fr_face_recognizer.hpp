#pragma once

#include "fr_face_detector.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fr {

constexpr size_t kFeatureDim = 128;

struct FaceFeature {
    std::vector<float> data;  // 128-dimensional L2-normalized vector

    FaceFeature() : data(kFeatureDim, 0.0f) {}
    explicit FaceFeature(const std::vector<float>& vec) : data(vec) {}

    bool IsValid() const {
        return data.size() == kFeatureDim;
    }
};

struct MatchResult {
    bool matched{false};
    std::string person_id;
    std::string name;
    float similarity{0.0f};
};

class FaceRecognizer {
public:
    FaceRecognizer();
    ~FaceRecognizer();

    // Extract 128-D normalized feature embedding from detected face in RGB image
    bool ExtractFeature(const uint8_t* rgb_data, int width, int height,
                        const FaceDetection& face,
                        FaceFeature& out_feature);

    // Compute cosine similarity between two 128-D feature vectors
    static float ComputeSimilarity(const FaceFeature& a, const FaceFeature& b);

    // Align and crop face to normalized RGB buffer (112x112)
    static bool AlignAndCropFace(const uint8_t* rgb_data, int width, int height,
                                 const FaceDetection& face,
                                 std::vector<uint8_t>& out_cropped_rgb,
                                 int out_w = 112, int out_h = 112);

    // Quality check for face before enrollment or recognition
    static bool CheckFaceQuality(const FaceDetection& face, int img_w, int img_h,
                                 std::string* reason = nullptr);
};

}  // namespace fr
