#include "deep_stage.h"

#include <set>

namespace fd {

std::vector<std::pair<int, int>> RunScheduler::feed(int index, int suspicious) {
    std::vector<std::pair<int, int>> ready;
    if (!suspicious) {
        in_run_ = false;
        return ready;
    }
    if (!in_run_) {
        in_run_ = true;
        next_start_ = index;
    }
    while (index - next_start_ + 1 >= window_) {
        ready.emplace_back(next_start_, next_start_ + window_ - 1);
        next_start_ += stride_;
    }
    return ready;
}

DeepStage::DeepStage(const DeepConfig& cfg, const FeatureSpec& spec, IPose& pose, ICnn& cnn)
    : cfg_(cfg), spec_(spec), pose_(pose), cnn_(cnn) {
    const auto model_threshold = cnn.threshold();
    threshold_ = (model_threshold && cfg.use_model_threshold) ? *model_threshold : cfg.cnn_threshold;
}

std::vector<DeepWindowResult> DeepStage::evaluate(const std::vector<std::pair<int, int>>& windows,
                                                  const ImageMap& images, const BoxMap& boxes) {
    // 1. pose for every frame of every window that has no cached pose yet (one batched call)
    std::set<int> needed;
    for (const auto& w : windows)
        for (int i = w.first; i <= w.second; ++i) needed.insert(i);
    std::vector<int> missing;
    std::vector<ImagePtr> imgs;
    std::vector<Box> bxs;
    for (int i : needed) {
        auto b = boxes.find(i);
        auto im = images.find(i);
        if (poses_.count(i) || b == boxes.end() || !b->second || im == images.end()) continue;
        missing.push_back(i);
        imgs.push_back(im->second);
        bxs.push_back(*b->second);
    }
    if (!missing.empty()) {
        const std::vector<PoseResult> results = pose_.estimate(imgs, bxs);
        for (size_t n = 0; n < missing.size(); ++n) poses_[missing[n]] = results[n];
    }

    // 2. window features + pose-quality gate
    std::vector<DeepWindowResult> results(windows.size());
    std::vector<float> tensors;
    std::vector<size_t> slots;
    for (size_t n = 0; n < windows.size(); ++n) {
        const int start = windows[n].first, end = windows[n].second;
        DeepWindowResult& r = results[n];
        r.start = start; r.end = end;
        std::vector<Box> wb;
        std::vector<PoseResult> wp;
        bool complete = true;
        for (int i = start; i <= end && complete; ++i) {
            auto b = boxes.find(i);
            auto p = poses_.find(i);
            if (b == boxes.end() || !b->second || p == poses_.end()) complete = false;
            else { wb.push_back(*b->second); wp.push_back(p->second); }
        }
        if (!complete) { r.status = "missing_data"; continue; }
        const WindowFeatures feats = build_window(wb, wp, spec_);
        r.mean_confidence = feats.mean_confidence;
        r.min_frame_confidence = feats.min_frame_confidence;
        if (feats.mean_confidence < cfg_.min_mean_confidence || feats.min_frame_confidence < cfg_.min_frame_confidence) {
            r.status = "low_pose_confidence";
            continue;
        }
        tensors.insert(tensors.end(), feats.cnn_tensor.begin(), feats.cnn_tensor.end());
        slots.push_back(n);
    }

    // 3. one batched CNN call
    if (!slots.empty()) {
        const std::vector<double> probs = cnn_.predict_proba(tensors, static_cast<int>(slots.size()));
        for (size_t k = 0; k < slots.size(); ++k) {
            DeepWindowResult& r = results[slots[k]];
            r.probability = probs[k];
            r.positive = probs[k] >= threshold_;
            r.status = "ok";
        }
    }
    return results;
}

}  // namespace fd
