// Deep cascade stage: RTMPose-t keypoints -> CNN, run only on runs of suspicious frames.
//
// RunScheduler turns the stream of final s3 flags into CNN windows: inside every maximal run of
// suspicious frames a window of T frames slides with ``window_stride``; a window is emitted as soon
// as its last frame has a final flag.  DeepStage evaluates a list of windows with the pose model
// (batched, per-frame cache so overlapping windows reuse poses) and the CNN (one batched call).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "representation.h"
#include "types.h"

namespace fd {

class RunScheduler {
public:
    RunScheduler(int window, int stride) : window_(window), stride_(stride) {}
    void reset() { in_run_ = false; }
    // Consume the final flag of frame ``index`` (in order); returns the windows that became ready.
    std::vector<std::pair<int, int>> feed(int index, int suspicious);

private:
    int window_, stride_;
    bool in_run_ = false;
    int next_start_ = 0;
};

struct DeepConfig {
    double cnn_threshold = 0.5;
    bool use_model_threshold = true;
    int window_stride = 2;
    int cooldown_batches = 3;
    double min_mean_confidence = 0.2;
    double min_frame_confidence = 0.085;
};

struct DeepWindowResult {
    int start = 0, end = 0;
    std::optional<double> probability;
    bool positive = false;
    std::string status;               // "ok" | "low_pose_confidence" | "missing_data"
    double mean_confidence = 0, min_frame_confidence = 0;
};

class DeepStage {
public:
    DeepStage(const DeepConfig& cfg, const FeatureSpec& spec, IPose& pose, ICnn& cnn);
    std::vector<DeepWindowResult> evaluate(const std::vector<std::pair<int, int>>& windows, const ImageMap& images,
                                           const BoxMap& boxes);
    void trim(int keep_from) { poses_.erase(poses_.begin(), poses_.lower_bound(keep_from)); }

private:
    DeepConfig cfg_;
    FeatureSpec spec_;
    IPose& pose_;
    ICnn& cnn_;
    double threshold_;
    std::map<int, PoseResult> poses_;
};

}  // namespace fd
