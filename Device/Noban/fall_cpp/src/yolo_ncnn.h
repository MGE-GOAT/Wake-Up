// YOLO11n person detector on ncnn (yolo11n-320.ncnn.param / .bin exported with imgsz=320).
// Pre-processing is the same as in the Python/ONNX version: aspect-preserving letterbox (cv::resize,
// bilinear, padding value 114), RGB, scale 1/255; decoding is shared code in vision_utils.cpp.
#pragma once

#include <memory>
#include <string>

#include "config.h"
#include "person_detector.h"

namespace fd {

class YoloNcnn : public IPersonDetector {
public:
    YoloNcnn(const YoloConfig& cfg, const std::string& models_dir, int num_threads);
    ~YoloNcnn() override;

    std::vector<std::vector<Detection>> detect(const std::vector<cv::Mat>& images) override;

private:
    struct Impl;                       // keeps the ncnn headers out of this header
    std::unique_ptr<Impl> impl_;
    YoloConfig cfg_;
};

}  // namespace fd
