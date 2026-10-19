// Box helpers.  Rounding uses std::nearbyint (round-half-to-even) to match Python's round().
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "types.h"

namespace fd {

inline double iou(const Box& a, const Box& b) {
    const double ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    const double ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    const double inter = std::max(0.0, ix2 - ix1) * std::max(0.0, iy2 - iy1);
    const double uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0 ? inter / uni : 0.0;
}

inline Box union_box(const std::vector<Box>& boxes) {
    Box u = boxes.front();
    for (const Box& b : boxes) {
        u.x1 = std::min(u.x1, b.x1); u.y1 = std::min(u.y1, b.y1);
        u.x2 = std::max(u.x2, b.x2); u.y2 = std::max(u.y2, b.y2);
    }
    return u;
}

inline Box clip_box(const Box& b, int width, int height) {
    auto c = [](double v, double hi) { return std::min(std::max(v, 0.0), hi); };
    return {c(b.x1, width), c(b.y1, height), c(b.x2, width), c(b.y2, height)};
}

// Exactly square, integer-aligned region centred on ``box`` (side = max(w,h)*margin, >= min_side,
// capped at min(width,height)), shifted - never cropped - to lie inside the image.
inline Box square_around(const Box& box, double margin, int width, int height, double min_side) {
    double side_f = std::min(std::max(std::max(box.x2 - box.x1, box.y2 - box.y1) * margin, min_side),
                             static_cast<double>(std::min(width, height)));
    int side = std::max(static_cast<int>(std::nearbyint(side_f)), 1);
    int left = static_cast<int>(std::nearbyint((box.x1 + box.x2) / 2.0 - side / 2.0));
    int top = static_cast<int>(std::nearbyint((box.y1 + box.y2) / 2.0 - side / 2.0));
    left = std::min(std::max(left, 0), width - side);
    top = std::min(std::max(top, 0), height - side);
    return {double(left), double(top), double(left + side), double(top + side)};
}

}  // namespace fd
