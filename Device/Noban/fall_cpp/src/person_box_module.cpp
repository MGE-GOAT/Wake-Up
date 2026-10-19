#include "person_box_module.h"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

#include "geometry.h"

namespace fd {
namespace {

cv::Mat to_gray(const cv::Mat& bgr) {
    cv::Mat g;
    cv::cvtColor(bgr, g, cv::COLOR_BGR2GRAY);
    return g;
}

// absdiff(a, b) > threshold  ->  CV_8U mask with 0 / 255
cv::Mat diff_above(const cv::Mat& a, const cv::Mat& b, int threshold) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return diff > threshold;
}

// Close the binary mask, return the bounding box of its largest blob (or nullopt).
std::optional<Box> largest_blob_box(const cv::Mat& mask, int kernel, int iterations, double min_area, double max_area) {
    cv::Mat m = mask.clone();
    if (kernel > 1) {
        const cv::Mat element = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kernel, kernel));
        cv::morphologyEx(m, m, cv::MORPH_CLOSE, element, cv::Point(-1, -1), iterations);
    }
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(m, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    int best = -1;
    double best_area = 0.0;
    for (size_t i = 0; i < contours.size(); ++i) {
        const double area = cv::contourArea(contours[i]);
        if (area > best_area) { best = static_cast<int>(i); best_area = area; }
    }
    if (best < 0 || best_area < min_area || best_area > max_area) return std::nullopt;
    const cv::Rect r = cv::boundingRect(contours[best]);
    return Box{double(r.x), double(r.y), double(r.x + r.width), double(r.y + r.height)};
}

cv::Rect int_rect(const Box& b) {
    const int x1 = static_cast<int>(std::nearbyint(b.x1)), y1 = static_cast<int>(std::nearbyint(b.y1));
    const int x2 = static_cast<int>(std::nearbyint(b.x2)), y2 = static_cast<int>(std::nearbyint(b.y2));
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

}  // namespace

PersonBoxModule::PersonBoxModule(const BBoxConfig& cfg, IPersonDetector* detector)
    : cfg_(cfg), detector_(cfg.yolo.enabled ? detector : nullptr), bg_(cfg.bg_grid), fusion_(cfg.fusion) {}

bool PersonBoxModule::multi_person_confirmed() const {
    if (static_cast<int>(multi_hist_.size()) < cfg_.multi_confirm_runs) return false;
    return std::all_of(multi_hist_.begin(), multi_hist_.end(), [](bool v) { return v; });
}

std::vector<FrameBox> PersonBoxModule::process_batch(const std::vector<Frame>& frames) {
    if (!bg_.ready()) initialise(frames.front());
    observe(frames);
    fusion_.n_received = frames.back().index + 1;
    return fusion_.finalize(fusion_.n_received - 1 - lookahead_frames());
}

void PersonBoxModule::initialise(const Frame& first) {
    std::optional<Box> person;
    if (detector_) {
        const auto dets = detector_->detect({*first.image})[0];
        if (!dets.empty()) {
            person = dets[0].box;
            last_yolo_box_ = person;
        }
    }
    bg_.init(*first.image, person);
}

void PersonBoxModule::observe(const std::vector<Frame>& frames) {
    const int h = frames.front().image->rows, w = frames.front().image->cols;
    GrayMap gray;
    for (const Frame& f : frames) gray[f.index] = to_gray(*f.image);
    for (const auto& kv : tail_) gray.emplace(kv.first, kv.second);

    std::vector<const Frame*> yolo_frames, coarse_frames;
    for (const Frame& f : frames)
        if ((f.index + 1) % cfg_.yolo.every_n_frames == 0) yolo_frames.push_back(&f);
    if (!yolo_frames.empty()) {
        coarse_frames = yolo_frames;
    } else {
        const int last = static_cast<int>(frames.size()) - 1;
        for (int i = 0; i < 4; ++i) coarse_frames.push_back(&frames[static_cast<int>(i * (last / 3.0))]);
    }

    // 1. coarse ROI from low-resolution background subtraction
    std::vector<Box> coarse_boxes;
    std::map<int, cv::Mat> coarse_masks;
    for (const Frame* f : coarse_frames) {
        cv::Mat mask;
        const auto box = coarse_foreground(*f, gray, h, w, mask);
        coarse_masks[f->index] = mask;
        if (box) coarse_boxes.push_back(*box);
    }
    const std::optional<Box> roi = make_roi(coarse_boxes, h, w, last_yolo_box_.has_value());

    // 2. YOLO on the ROI, 3. background update
    const std::map<int, Obs> yolo_obs = run_yolo(yolo_frames, roi, h, w);
    for (const Frame* f : yolo_frames) {
        auto it = yolo_obs.find(f->index);
        std::optional<Box> person;
        std::optional<Grid> occupied;
        if (it != yolo_obs.end()) person = it->second.box;
        else occupied = bg_.cells_from_mask(coarse_masks[f->index]);
        bg_.update(*f->image, person, occupied, cfg_.bg_update_margin);
    }
    for (const auto& kv : yolo_obs) fusion_.obs[kv.first] = kv.second;

    // 4. fine boxes for every frame inside the (re-computed) ROI
    std::vector<Box> parts = coarse_boxes;
    for (const auto& kv : yolo_obs) parts.push_back(kv.second.box);
    const std::optional<Box> roi2 = make_roi(parts, h, w, !yolo_obs.empty() || last_yolo_box_.has_value());
    if (roi2) {
        const cv::Mat unknown_full = bg_.any_unknown() ? bg_.unknown_mask(w, h) : cv::Mat();
        for (const Frame& f : frames) {
            if (fusion_.obs.count(f.index)) continue;
            const auto box = fine_box(f, gray, *roi2, h, w, unknown_full);
            if (box && passes_gate(f.index, *box)) fusion_.obs[f.index] = Obs{*box, "bg", cfg_.fusion.bg_score};
        }
    }

    tail_.clear();
    const int keep_from = frames.back().index - cfg_.coarse_diff_step;
    for (const auto& kv : gray)
        if (kv.first >= keep_from) tail_[kv.first] = kv.second;
}

cv::Mat PersonBoxModule::apply_unknown(const cv::Mat& mask, int index, const GrayMap& gray, cv::Size size,
                                       const cv::Mat& cur_small) {
    if (!bg_.any_unknown()) return mask;
    const cv::Mat unknown = bg_.unknown_mask(size.width, size.height) > 0;
    cv::Mat temporal = cv::Mat::zeros(mask.size(), CV_8U);
    auto ref = gray.find(index - cfg_.coarse_diff_step);
    if (ref != gray.end()) {
        cv::Mat ref_small;
        cv::resize(ref->second, ref_small, size, 0, 0, cv::INTER_AREA);
        temporal = diff_above(cur_small, ref_small, cfg_.fine_temporal_threshold);
    }
    cv::Mat out = mask.clone();
    temporal.copyTo(out, unknown);           // inside untrusted cells use frame-to-frame differences
    return out;
}

std::optional<Box> PersonBoxModule::coarse_foreground(const Frame& frame, const GrayMap& gray, int h, int w,
                                                      cv::Mat& mask_out) {
    const int cw = cfg_.coarse_width;
    const int ch = std::max(8, static_cast<int>(std::nearbyint(cw * static_cast<double>(h) / w)));
    const cv::Size size(cw, ch);
    cv::Mat cur, ref;
    cv::resize(gray.at(frame.index), cur, size, 0, 0, cv::INTER_AREA);
    cv::resize(bg_.gray(), ref, size, 0, 0, cv::INTER_AREA);
    cv::Mat mask = diff_above(cur, ref, cfg_.coarse_threshold);
    mask = apply_unknown(mask, frame.index, gray, size, cur);
    mask_out = mask;
    auto box = largest_blob_box(mask, cfg_.coarse_kernel, cfg_.coarse_close_iterations,
                                cfg_.coarse_min_area_ratio * cw * ch, cfg_.coarse_max_area_ratio * cw * ch);
    if (box) {
        const double sx = static_cast<double>(w) / cw, sy = static_cast<double>(h) / ch;
        box = Box{box->x1 * sx, box->y1 * sy, box->x2 * sx, box->y2 * sy};
    }
    return box;
}

std::optional<Box> PersonBoxModule::make_roi(const std::vector<Box>& boxes, int h, int w, bool with_yolo) const {
    std::vector<Box> parts = boxes;
    if (last_yolo_box_) parts.push_back(*last_yolo_box_);
    if (parts.empty()) return std::nullopt;
    const double margin = with_yolo ? cfg_.roi_margin_with_yolo : cfg_.roi_margin_without_yolo;
    return square_around(union_box(parts), margin, w, h, cfg_.roi_min_side_frac * std::min(h, w));
}

std::map<int, Obs> PersonBoxModule::run_yolo(const std::vector<const Frame*>& yolo_frames, const std::optional<Box>& roi,
                                             int h, int w) {
    std::map<int, Obs> found;
    if (!detector_ || yolo_frames.empty()) return found;
    std::vector<cv::Mat> crops;
    double ox = 0, oy = 0;
    if (!roi) {
        ++no_roi_runs_;
        if (no_roi_runs_ % cfg_.yolo.full_frame_every_n_runs != 0) return found;
        for (const Frame* f : yolo_frames) crops.push_back(*f->image);            // periodic full-frame search
    } else {
        const cv::Rect r = int_rect(*roi);
        for (const Frame* f : yolo_frames) crops.push_back((*f->image)(r));
        ox = r.x;
        oy = r.y;
    }
    const auto detections = detector_->detect(crops);
    bool crowd = false;
    for (size_t i = 0; i < yolo_frames.size(); ++i) {
        const auto& dets = detections[i];
        int confident = 0;
        for (const Detection& d : dets)
            if (d.score >= cfg_.multi_conf) ++confident;
        if (confident >= 2) crowd = true;
        if (!dets.empty()) {
            const Detection& best = dets.front();
            const Box shifted{best.box.x1 + ox, best.box.y1 + oy, best.box.x2 + ox, best.box.y2 + oy};
            found[yolo_frames[i]->index] = Obs{clip_box(shifted, w, h), "yolo", best.score};
        }
    }
    multi_hist_.push_back(crowd);
    while (static_cast<int>(multi_hist_.size()) > cfg_.multi_confirm_runs) multi_hist_.pop_front();
    if (!found.empty()) last_yolo_box_ = found.rbegin()->second.box;
    return found;
}

std::optional<Box> PersonBoxModule::fine_box(const Frame& frame, const GrayMap& gray, const Box& roi, int h, int w,
                                             const cv::Mat& unknown_full) {
    (void)h; (void)w;
    const cv::Rect r = int_rect(roi);
    const int side = r.width;
    if (side < 8 || r.height < 8) return std::nullopt;
    const cv::Size size(cfg_.fine_size, cfg_.fine_size);   // the ROI is square, so this resize is isotropic
    cv::Mat cur, ref;
    cv::resize(gray.at(frame.index)(r), cur, size, 0, 0, cv::INTER_AREA);
    cv::resize(bg_.gray()(r), ref, size, 0, 0, cv::INTER_AREA);
    cv::Mat mask = diff_above(cur, ref, cfg_.fine_threshold);
    if (!unknown_full.empty()) {
        cv::Mat unknown;
        cv::resize(unknown_full(r), unknown, size, 0, 0, cv::INTER_NEAREST);
        unknown = unknown > 0;
        cv::Mat temporal = cv::Mat::zeros(mask.size(), CV_8U);
        auto prev = gray.find(frame.index - cfg_.coarse_diff_step);
        if (prev != gray.end()) {
            cv::Mat prev_small;
            cv::resize(prev->second(r), prev_small, size, 0, 0, cv::INTER_AREA);
            temporal = diff_above(cur, prev_small, cfg_.fine_temporal_threshold);
        }
        temporal.copyTo(mask, unknown);
    }
    const double area = static_cast<double>(cfg_.fine_size) * cfg_.fine_size;
    auto box = largest_blob_box(mask, cfg_.fine_kernel, cfg_.fine_close_iterations, cfg_.fine_min_area_ratio * area, area);
    if (!box) return std::nullopt;
    const double pad = cfg_.fine_pad_ratio * cfg_.fine_size, hi = cfg_.fine_size;
    auto c = [hi](double v) { return std::min(std::max(v, 0.0), hi); };
    const double scale = side / static_cast<double>(cfg_.fine_size);
    return Box{c(box->x1 - pad) * scale + r.x, c(box->y1 - pad) * scale + r.y,
               c(box->x2 + pad) * scale + r.x, c(box->y2 + pad) * scale + r.y};
}

bool PersonBoxModule::passes_gate(int index, const Box& box) const {
    // Reject a background box that has no overlap with a nearby YOLO box (probably not the person).
    const int every = cfg_.yolo.every_n_frames;
    const Obs* nearest = nullptr;
    int best = 0;
    for (int j = index - every; j <= index + every; ++j) {
        auto it = fusion_.obs.find(j);
        if (it != fusion_.obs.end() && it->second.source == "yolo" && (!nearest || std::abs(j - index) < best)) {
            nearest = &it->second;
            best = std::abs(j - index);
        }
    }
    if (!nearest) return true;
    return iou(box, nearest->box) >= cfg_.fusion.bg_gate_iou;
}

}  // namespace fd
