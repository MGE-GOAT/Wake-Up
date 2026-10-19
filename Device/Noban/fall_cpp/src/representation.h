// Feature definitions shared with the Python training code (falldet/fall/representation.py).
// Everything that turns 16 boxes (+ 16 poses) into SVM / CNN inputs lives here.
//
//  * reference square per window: union of the 16 boxes, side = longest side * batch_bbox_margin,
//    not clamped to the image; all coordinates are (v - origin) / side (one isotropic scale)
//  * SVM vector (80) : 16 x (y1, y2, x1, x2, w/h ratio)
//  * CNN tensor      : (T=16, P=15, C) = [bbox top-left, 13 body points, bbox bottom-right]
//                      in ``point_order``; C=2 (x, y) or C=3 (x, y, confidence)
//  * 13 body points  : head = confidence-weighted mean of keypoints 0..4, then keypoints 5..16
#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace fd {

struct FeatureSpec {
    double target_fps = 16.0;
    int window = 16;
    std::string point_order = "legacy";   // "legacy" | "chain"
    int channels = 2;                     // 2 or 3
    double batch_bbox_margin = 1.2;
    int smooth_half_window = 3;
    double smooth_sigma = 2.0;

    // Same 16-hex-digit signature as the Python FeatureSpec.signature() (SHA-1 of its JSON).
    std::string signature() const;
};

struct BatchRef { double x0 = 0, y0 = 0, side = 1; };

struct WindowFeatures {
    std::vector<float> features80;     // 80
    std::vector<float> cnn_tensor;     // window * 15 * channels, row-major (T, P, C)
    double mean_confidence = 0;        // mean keypoint confidence over the window
    double min_frame_confidence = 0;   // worst per-frame mean keypoint confidence
};

BatchRef batch_reference(const std::vector<Box>& boxes, double margin);
std::vector<float> window_features80(const std::vector<Box>& boxes, const BatchRef& ref);
WindowFeatures build_window(const std::vector<Box>& boxes, const std::vector<PoseResult>& poses, const FeatureSpec& spec);

// Exposed for tests.
std::string sha1_hex(const std::string& data);
std::string python_float_repr(double v);

}  // namespace fd
