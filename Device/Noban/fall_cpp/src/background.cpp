#include "background.h"

#include <opencv2/imgproc.hpp>

namespace fd {

bool BackgroundModel::any_unknown() const {
    for (unsigned char v : unknown_)
        if (v) return true;
    return false;
}

void BackgroundModel::init(const cv::Mat& frame, const std::optional<Box>& person) {
    image_ = frame.clone();
    cv::cvtColor(image_, gray_, cv::COLOR_BGR2GRAY);
    // Cell edges like numpy.linspace(0, size, grid + 1).astype(int).
    auto edges = [&](int size) {
        std::vector<int> e(grid_ + 1);
        const double step = size / static_cast<double>(grid_);
        for (int i = 0; i < grid_; ++i) e[i] = static_cast<int>(i * step);
        e[grid_] = size;
        return e;
    };
    ys_ = edges(frame.rows);
    xs_ = edges(frame.cols);
    unknown_.assign(grid_ * grid_, 0);
    if (person) unknown_ = cells_hit_by(*person, 0.0);
}

Grid BackgroundModel::cells_hit_by(const Box& box, double margin) const {
    const double mx = (box.x2 - box.x1) * margin, my = (box.y2 - box.y1) * margin;
    const double x1 = box.x1 - mx, y1 = box.y1 - my, x2 = box.x2 + mx, y2 = box.y2 + my;
    Grid hit(grid_ * grid_, 0);
    for (int i = 0; i < grid_; ++i) {
        const bool row = ys_[i + 1] > y1 && ys_[i] < y2;
        for (int j = 0; j < grid_; ++j) {
            const bool col = xs_[j + 1] > x1 && xs_[j] < x2;
            hit[i * grid_ + j] = (row && col) ? 1 : 0;
        }
    }
    return hit;
}

void BackgroundModel::update(const cv::Mat& frame, const std::optional<Box>& person, const std::optional<Grid>& occupied,
                             double margin) {
    Grid keep(grid_ * grid_, 0);
    if (person) keep = cells_hit_by(*person, margin);
    else if (occupied) keep = *occupied;
    if (occupied && person)
        for (size_t i = 0; i < keep.size(); ++i) keep[i] = keep[i] || (*occupied)[i];
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    for (int i = 0; i < grid_; ++i) {
        for (int j = 0; j < grid_; ++j) {
            if (keep[i * grid_ + j]) continue;
            const cv::Rect cell(xs_[j], ys_[i], xs_[j + 1] - xs_[j], ys_[i + 1] - ys_[i]);
            frame(cell).copyTo(image_(cell));
            gray(cell).copyTo(gray_(cell));
        }
    }
    for (size_t i = 0; i < unknown_.size(); ++i) unknown_[i] = unknown_[i] && keep[i];
}

cv::Mat BackgroundModel::unknown_mask(int width, int height) const {
    cv::Mat small(grid_, grid_, CV_8U);
    for (int i = 0; i < grid_; ++i)
        for (int j = 0; j < grid_; ++j) small.at<unsigned char>(i, j) = unknown_[i * grid_ + j] ? 255 : 0;
    cv::Mat out;
    cv::resize(small, out, cv::Size(width, height), 0, 0, cv::INTER_NEAREST);
    return out;
}

Grid BackgroundModel::cells_from_mask(const cv::Mat& mask) const {
    cv::Mat binary = mask > 0;                            // 0 / 255
    cv::Mat small;
    cv::resize(binary, small, cv::Size(grid_, grid_), 0, 0, cv::INTER_AREA);
    Grid cells(grid_ * grid_, 0);
    for (int i = 0; i < grid_; ++i)
        for (int j = 0; j < grid_; ++j) cells[i * grid_ + j] = small.at<unsigned char>(i, j) > 0 ? 1 : 0;
    return cells;
}

}  // namespace fd
