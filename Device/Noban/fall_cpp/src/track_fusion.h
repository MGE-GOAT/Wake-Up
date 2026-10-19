// Fusion and temporal smoothing of the per-frame person-box observations.
//
// Every frame may carry one observation: a YOLO box (weight 1.0), a background-subtraction box
// (weight 0.25) or none.  Short gaps are filled by linear interpolation; the final box of a frame is
// a Gaussian, weight-aware average over +-half_window frames.  Because of this look-ahead the final
// box of frame i is available only after frame i + lookahead() has been received.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "types.h"

namespace fd {

struct Obs {
    Box box;
    std::string source;   // "yolo" | "bg"
    double score = 0;
};

struct FusionConfig {
    double weight_yolo = 1.0, weight_bg = 0.25, weight_interp = 0.25, bg_score = 0.3, bg_gate_iou = 0.1;
    int max_gap_frames = 12;
    int half_window = 6;
    double sigma = 3.0;
    int every_n_frames = 8;   // only used to decide when stale observations can be dropped
};

class TrackFusion {
public:
    explicit TrackFusion(const FusionConfig& cfg);

    int lookahead() const { return cfg_.half_window + cfg_.max_gap_frames; }

    std::map<int, Obs> obs;   // frame index -> observation (filled by the box module)
    int n_received = 0;       // frames received so far
    int final_next = 0;       // first frame whose final box has not been produced yet

    // Produce the final boxes of frames [final_next, upto].
    std::vector<FrameBox> finalize(int upto);

private:
    FusionConfig cfg_;
    std::vector<double> gauss_;
};

}  // namespace fd
