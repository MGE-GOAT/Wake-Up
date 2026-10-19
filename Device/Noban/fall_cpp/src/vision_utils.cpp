#include "vision_utils.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace fd {

Letterbox letterbox_params(int width, int height, int size) {
    Letterbox lb;
    lb.scale = size / static_cast<double>(std::max(height, width));
    lb.new_w = std::max(1, static_cast<int>(std::nearbyint(width * lb.scale)));
    lb.new_h = std::max(1, static_cast<int>(std::nearbyint(height * lb.scale)));
    lb.pad_x = (size - lb.new_w) / 2;
    lb.pad_y = (size - lb.new_h) / 2;
    return lb;
}

std::vector<int> nms(const std::vector<float>& b, const std::vector<float>& scores, float iou_thr) {
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int i, int j) { return scores[i] > scores[j]; });
    std::vector<int> keep;
    while (!order.empty()) {
        const int i = order.front();
        keep.push_back(i);
        std::vector<int> rest;
        for (size_t k = 1; k < order.size(); ++k) {
            const int j = order[k];
            const float ix1 = std::max(b[4 * i], b[4 * j]), iy1 = std::max(b[4 * i + 1], b[4 * j + 1]);
            const float ix2 = std::min(b[4 * i + 2], b[4 * j + 2]), iy2 = std::min(b[4 * i + 3], b[4 * j + 3]);
            const float inter = std::max(ix2 - ix1, 0.0f) * std::max(iy2 - iy1, 0.0f);
            const float area_i = (b[4 * i + 2] - b[4 * i]) * (b[4 * i + 3] - b[4 * i + 1]);
            const float area_j = (b[4 * j + 2] - b[4 * j]) * (b[4 * j + 3] - b[4 * j + 1]);
            const float iou = inter / std::max(area_i + area_j - inter, 1e-9f);
            if (iou <= iou_thr) rest.push_back(j);
        }
        order = rest;
    }
    return keep;
}

std::vector<Detection> decode_yolo(const float* data, int channels, int anchors, int person_class, float conf,
                                   float iou_thr, const Letterbox& lb, int img_w, int img_h) {
    (void)channels;
    std::vector<float> boxes, scores;
    for (int a = 0; a < anchors; ++a) {
        const float s = data[(4 + person_class) * anchors + a];
        if (!(s >= conf)) continue;
        const float cx = data[0 * anchors + a], cy = data[1 * anchors + a];
        const float w = data[2 * anchors + a], h = data[3 * anchors + a];
        boxes.insert(boxes.end(), {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2});
        scores.push_back(s);
    }
    std::vector<Detection> out;
    for (int i : nms(boxes, scores, iou_thr)) {
        Detection d;
        const double x1 = (static_cast<double>(boxes[4 * i]) - lb.pad_x) / lb.scale;
        const double y1 = (static_cast<double>(boxes[4 * i + 1]) - lb.pad_y) / lb.scale;
        const double x2 = (static_cast<double>(boxes[4 * i + 2]) - lb.pad_x) / lb.scale;
        const double y2 = (static_cast<double>(boxes[4 * i + 3]) - lb.pad_y) / lb.scale;
        auto clip = [](double v, double hi) { return std::min(std::max(v, 0.0), hi); };
        d.box = {clip(x1, img_w), clip(y1, img_h), clip(x2, img_w), clip(y2, img_h)};
        d.score = scores[i];
        out.push_back(d);
    }
    return out;  // nms() already returns descending-score order
}

CropGeometry pose_crop_geometry(const Box& box, double aspect, double padding) {
    CropGeometry g;
    g.cx = (box.x1 + box.x2) / 2.0;
    g.cy = (box.y1 + box.y2) / 2.0;
    double w = std::max(box.x2 - box.x1, 1.0), h = std::max(box.y2 - box.y1, 1.0);
    if (w > h * aspect) h = w / aspect; else w = h * aspect;
    g.scale_w = w * padding;
    g.scale_h = h * padding;
    return g;
}

PoseResult decode_simcc(const float* sx, const float* sy, int bins_x, int bins_y, double split_ratio, int in_w,
                        int in_h, const CropGeometry& crop) {
    PoseResult r;
    const float split = static_cast<float>(split_ratio);
    for (int k = 0; k < 17; ++k) {
        const float* row_x = sx + static_cast<size_t>(k) * bins_x;
        const float* row_y = sy + static_cast<size_t>(k) * bins_y;
        const float* mx = std::max_element(row_x, row_x + bins_x);
        const float* my = std::max_element(row_y, row_y + bins_y);
        const float score = std::min(*mx, *my);
        float lx = static_cast<float>(mx - row_x) / split;
        float ly = static_cast<float>(my - row_y) / split;
        if (score <= 0.0f) { lx = -1.0f; ly = -1.0f; }
        const float qx = lx / static_cast<float>(in_w), qy = ly / static_cast<float>(in_h);
        r.kp[k][0] = static_cast<double>(qx) * crop.scale_w + crop.cx - crop.scale_w / 2.0;
        r.kp[k][1] = static_cast<double>(qy) * crop.scale_h + crop.cy - crop.scale_h / 2.0;
        r.score[k] = score;
    }
    return r;
}

}  // namespace fd
