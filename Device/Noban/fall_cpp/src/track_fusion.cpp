#include "track_fusion.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fd {

TrackFusion::TrackFusion(const FusionConfig& cfg) : cfg_(cfg) {
    for (int off = -cfg.half_window; off <= cfg.half_window; ++off)
        gauss_.push_back(std::exp(-0.5 * std::pow(off / cfg.sigma, 2)));
}

std::vector<FrameBox> TrackFusion::finalize(int upto) {
    std::vector<FrameBox> out;
    if (upto < final_next) return out;
    const int half = cfg_.half_window;
    const int back = half + cfg_.max_gap_frames;     // a gap's left anchor may lie this far back
    const int lo = std::max(0, final_next - back), hi = n_received - 1;
    const int n = hi - lo + 1;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::array<double, 4>> vals(n, {nan, nan, nan, nan});
    std::vector<double> weight(n, 0.0), score(n, 0.0);
    std::vector<std::string> source(n);

    for (int i = lo; i <= hi; ++i) {
        auto it = obs.find(i);
        if (it == obs.end()) continue;
        const Obs& o = it->second;
        vals[i - lo] = {o.box.x1, o.box.y1, o.box.x2, o.box.y2};
        weight[i - lo] = o.source == "yolo" ? cfg_.weight_yolo : cfg_.weight_bg;
        score[i - lo] = o.score;
        source[i - lo] = o.source;
    }
    std::vector<int> valid;
    for (int i = 0; i < n; ++i)
        if (!std::isnan(vals[i][0])) valid.push_back(i);
    for (size_t v = 0; v + 1 < valid.size(); ++v) {          // fill short gaps linearly
        const int a = valid[v], b = valid[v + 1];
        if (b - a > 1 && b - a <= cfg_.max_gap_frames + 1) {
            for (int j = a + 1; j < b; ++j) {
                const double t = (j - a) / static_cast<double>(b - a);
                for (int c = 0; c < 4; ++c) vals[j][c] = (1 - t) * vals[a][c] + t * vals[b][c];
                weight[j] = cfg_.weight_interp;
                score[j] = 0.5 * std::min(score[a], score[b]);
                source[j] = "interp";
            }
        }
    }
    for (int i = final_next; i <= upto; ++i) {
        const int c = i - lo;
        FrameBox fb;
        fb.index = i;
        if (std::isnan(vals[c][0])) {
            out.push_back(fb);
            continue;
        }
        const int s = std::max(0, c - half), e = std::min(n, c + half + 1);
        double wsum = 0, acc[4] = {0, 0, 0, 0};
        for (int k = s; k < e; ++k) {
            if (std::isnan(vals[k][0])) continue;
            const double w = weight[k] * gauss_[(k - c) + half];
            wsum += w;
            for (int d = 0; d < 4; ++d) acc[d] += vals[k][d] * w;
        }
        fb.box = Box{acc[0] / wsum, acc[1] / wsum, acc[2] / wsum, acc[3] / wsum};
        fb.source = source[c];
        fb.score = score[c];
        out.push_back(fb);
    }
    final_next = upto + 1;
    std::vector<int> stale;
    for (const auto& kv : obs)
        if (kv.first < final_next - back - 2 * cfg_.every_n_frames - 1) stale.push_back(kv.first);
    for (int k : stale) obs.erase(k);
    return out;
}

}  // namespace fd
