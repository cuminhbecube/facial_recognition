#pragma once

#include <rknn_api.h>
#include <cstdint>
#include <string>
#include <vector>

namespace fr {

struct Point2f {
    float x{0.0f};
    float y{0.0f};
};

struct FaceBBox {
    float x1{0.0f};
    float y1{0.0f};
    float x2{0.0f};
    float y2{0.0f};

    float Width() const { return x2 - x1; }
    float Height() const { return y2 - y1; }
    float Area() const { return (x2 > x1 && y2 > y1) ? (x2 - x1) * (y2 - y1) : 0.0f; }
};

struct FaceDetection {
    FaceBBox bbox;
    float score{0.0f};
    Point2f landmarks[5];  // 0: left eye, 1: right eye, 2: nose, 3: left mouth corner, 4: right mouth corner
};

class FaceDetector {
public:
    FaceDetector();
    ~FaceDetector();

    // Initialize detector with YOLOv5n-face RKNN model
    bool Init(const std::string& model_path);
    void Release();
    bool IsInitialized() const { return initialized_; }

    // Run face detection on 640x640 RGB image buffer
    // image_data: raw RGB888 buffer of size 640 * 640 * 3
    // threshold: confidence threshold (default 0.45)
    // nms_threshold: IoU threshold (default 0.45)
    bool Detect(const uint8_t* rgb_data, int width, int height,
                std::vector<FaceDetection>& faces,
                float threshold = 0.45f, float nms_threshold = 0.45f);

    int GetInputWidth() const { return input_width_; }
    int GetInputHeight() const { return input_height_; }

private:
    rknn_context context_{0};
    bool initialized_{false};

    int input_width_{640};
    int input_height_{640};
    int input_channels_{3};

    rknn_tensor_attr input_attr_{};
    rknn_tensor_mem* input_mem_{nullptr};

    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<rknn_tensor_mem*> output_mems_;
};

}  // namespace fr
