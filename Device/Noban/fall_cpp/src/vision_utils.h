// Pure (library-free) pieces of the YOLO and RTMPose pre/post-processing.
#pragma once

#include <vector>

#include "types.h"

namespace fd {

struct Detection {
    Box box;       // in the pixel space of the image given to the detector
    double score = 0;
};

// Letterbox geometry: the image is scaled uniformly to fit ``size`` and padded (value 114).
struct Letterbox {
    int new_w = 0, new_h = 0, pad_x = 0, pad_y = 0;
    double scale = 1.0;
};
Letterbox letterbox_params(int width, int height, int size);

// Greedy NMS; returns kept indices ordered by descending score.
std::vector<int> nms(const std::vector<float>& boxes_xyxy, const std::vector<float>& scores, float iou_thr);

// Decode a YOLOv8/11 output stored channels-first (channels x anchors, row-major, as ncnn gives it).
// Returns person detections in the original image pixel space, best score first.
std::vector<Detection> decode_yolo(const float* data, int channels, int anchors, int person_class, float conf,
                                   float iou_thr, const Letterbox& lb, int img_w, int img_h);

// RTMPose crop: person box -> centre/scale of the crop whose w:h equals the model aspect ratio.
struct CropGeometry {
    double cx = 0, cy = 0, scale_w = 0, scale_h = 0;
};
CropGeometry pose_crop_geometry(const Box& box, double aspect, double padding);

// SimCC arg-max decoding of one person (17 keypoints) back to image pixels.
PoseResult decode_simcc(const float* simcc_x, const float* simcc_y, int bins_x, int bins_y, double split_ratio,
                        int in_w, int in_h, const CropGeometry& crop);

}  // namespace fd
