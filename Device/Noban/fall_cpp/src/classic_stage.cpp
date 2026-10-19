#include "classic_stage.h"

#include <algorithm>

namespace fd {

int apply_high_sensitive(const std::vector<float>& vec80, int base, const ClassicConfig& cfg, int window) {
    // Rows are (y1, y2, x1, x2, ratio).  Vc: downward speed of the box centre, Vr: growth of w/h,
    // between the frame before the window centre and the centre (float arithmetic, as in Python).
    const float* cur = &vec80[(window / 2) * 5];
    const float* prev = &vec80[(window / 2 - 1) * 5];
    const float vc = ((cur[0] + cur[1]) - (prev[0] + prev[1])) / 2.0f;
    const float vr = cur[4] - prev[4];
    int out = base;
    // if (vr < static_cast<float>(cfg.suppress_vr_below) && vc < static_cast<float>(cfg.suppress_vc_below)) out = 0;
    // if (vc > static_cast<float>(cfg.force_vc_above) && vr > static_cast<float>(cfg.force_vr_above) && cur[1] != prev[1]) out = 1;
    return out;
}

ClassicStage::ClassicStage(const ClassicConfig& cfg, const FeatureSpec& spec, ISvm& svm)
    : cfg_(cfg), spec_(spec), svm_(svm), T_(spec.window), before_(spec.window / 2), after_(spec.window - 1 - spec.window / 2) {
    const auto model_threshold = svm.threshold();
    threshold_ = (model_threshold && cfg.use_model_threshold) ? *model_threshold : cfg.svm_threshold;
}

ClassicUpdate ClassicStage::advance(const std::vector<FrameBox>& new_boxes) {
    ClassicUpdate upd;
    for (const FrameBox& fb : new_boxes) {
        boxes_[fb.index] = fb.box;
        n_boxes_ = fb.index + 1;
    }

    // s1: one batched SVM call over every newly computable window centre.
    std::vector<int> centres;
    std::vector<float> features;
    while (next_center_ + after_ < n_boxes_) {
        const int c = next_center_;
        const int start = c - before_;
        bool complete = start >= 0;
        std::vector<Box> window;
        for (int i = start; complete && i < start + T_; ++i) {
            auto it = boxes_.find(i);
            if (it == boxes_.end() || !it->second) complete = false;
            else window.push_back(*it->second);
        }
        if (complete) {
            const std::vector<float> vec = window_features80(window, batch_reference(window, spec_.batch_bbox_margin));
            centres.push_back(c);
            features.insert(features.end(), vec.begin(), vec.end());
        } else {
            s1_[c] = upd.s1[c] = 0;
        }
        ++next_center_;
    }
    if (!centres.empty()) {
        const std::vector<double> probs = svm_.predict_proba(features, static_cast<int>(centres.size()));
        for (size_t n = 0; n < centres.size(); ++n) {
            const std::vector<float> vec(features.begin() + n * 80, features.begin() + (n + 1) * 80);
            const int value = apply_high_sensitive(vec, probs[n] >= threshold_ ? 1 : 0, cfg_, T_);
            s1_[centres[n]] = upd.s1[centres[n]] = value;
            upd.svm_prob[centres[n]] = probs[n];
        }
    }

    auto window_sum = [&](const std::map<int, int>& m, int c) {
        int total = 0;
        for (int i = c - before_; i <= c + after_; ++i) {
            auto it = m.find(i);
            if (it != m.end()) total += it->second;
        }
        return total;
    };
    while (next_s2_ + after_ < next_center_) {
        const int c = next_s2_;
        s2_[c] = upd.s2[c] = window_sum(s1_, c) >= cfg_.post1_min_count ? 1 : 0;
        ++next_s2_;
    }
    while (next_s3_ + after_ < next_s2_) {
        const int c = next_s3_;
        upd.s3[c] = window_sum(s2_, c) >= cfg_.post2_min_count ? 1 : 0;
        ++next_s3_;
    }
    trim();
    return upd;
}

void ClassicStage::trim() {
    const int keep_from = next_s3_ - 2 * T_;
    boxes_.erase(boxes_.begin(), boxes_.lower_bound(keep_from));
    s1_.erase(s1_.begin(), s1_.lower_bound(keep_from));
    s2_.erase(s2_.begin(), s2_.lower_bound(keep_from));
}

}  // namespace fd
