// Interface of the person detector used by the box module (implemented by YoloNcnn).
#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "vision_utils.h"

namespace fd {

struct IPersonDetector {
    virtual ~IPersonDetector() = default;
    // One list of detections (best first) per image, in the pixel space of that image.
    virtual std::vector<std::vector<Detection>> detect(const std::vector<cv::Mat>& images) = 0;
};

}  // namespace fd
