// Canonical temporal policy: every input becomes a stream at ``target_fps`` (16).
// Canonical frame k sits at time k/16 s and is the source frame whose timestamp is nearest
// (ties -> the earlier frame).  No frame blending.
#pragma once

#include <cmath>
#include <vector>

namespace fd {

struct Selection {
    int k = 0;             // canonical frame index
    int src_index = 0;     // chosen source frame
    double src_time = 0;
};

class Resampler {
public:
    explicit Resampler(double target_fps, double source_interval = 0.0)
        : period_(1.0 / target_fps), interval_(source_interval) {}

    // Feed source frames in time order; returns the canonical frames that became decidable.
    std::vector<Selection> push(int src_index, double timestamp) {
        std::vector<Selection> out;
        const double eps = 1e-9;
        while (next_k_ * period_ <= timestamp + eps) {
            const double tau = next_k_ * period_;
            const bool take_current = !has_prev_ || std::fabs(tau - prev_time_) > std::fabs(timestamp - tau) + eps;
            out.push_back(take_current ? Selection{next_k_, src_index, timestamp}
                                       : Selection{next_k_, prev_index_, prev_time_});
            ++next_k_;
        }
        has_prev_ = true;
        prev_index_ = src_index;
        prev_time_ = timestamp;
        return out;
    }

    // End of a file: canonical frames within half a source interval after the last frame.
    std::vector<Selection> finish() {
        std::vector<Selection> out;
        if (!has_prev_ || interval_ <= 0) return out;
        while (next_k_ * period_ <= prev_time_ + 0.5 * interval_ + 1e-9) {
            out.push_back({next_k_, prev_index_, prev_time_});
            ++next_k_;
        }
        return out;
    }

private:
    double period_, interval_;
    int next_k_ = 0;
    bool has_prev_ = false;
    int prev_index_ = 0;
    double prev_time_ = 0;
};

}  // namespace fd
