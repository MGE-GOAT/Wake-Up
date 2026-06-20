// =========================================================
// extract_background — direct port of extract_background.py
//
// 16x16 grid motion-aware background extractor. Each grid cell tracks
// whether it is currently "moving" (frame-to-frame absdiff above a
// patch-area-scaled threshold). A cell that returns to stillness for
// STATIC_SCORE_THRESHOLD consecutive frames is copied into the
// running static_frame, which represents "the room without the person."
// =========================================================
#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace fall {

class ExtractBackground {
public:
    // frame_size = (W, H) — matches Python's call site: extract_background((W, H))
    ExtractBackground(int W, int H);

    // Update background with current frame. Returns the current static_frame
    // (same Mat that was passed on the very first call; mutated in place
    // across frames once any patch has gone still).
    cv::Mat update_bg(const cv::Mat& current_frame);

    // Reset: next update_bg() will treat its input as the new initial frame.
    void reset();

private:
    static constexpr int GRID_SIZE = 16;
    static constexpr int STATIC_SCORE_THRESHOLD = 5;

    int H_, W_;
    std::vector<int> y_grid_;   // length GRID_SIZE+1
    std::vector<int> x_grid_;   // length GRID_SIZE+1
    int moving_threshold_;      // int(0.0075 * patch_area), clamped to >=1

    // patch_states[i][j] and patch_static_counter[i][j] — flat row-major.
    std::vector<int> patch_states_;
    std::vector<int> patch_static_counter_;

    bool init_ = false;
    cv::Mat prev_gray_;
    cv::Mat static_frame_;   // BGR, same dims as input
};

} // namespace fall
