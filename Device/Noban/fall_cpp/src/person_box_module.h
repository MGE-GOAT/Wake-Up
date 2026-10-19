// Person bounding-box sub-module (single-person assumption).
//
// Per batch of 16 frames:
//  1. coarse motion ROI   low-resolution background subtraction on the YOLO frames (+ the last known
//                         box) gives a square region where the person must be (the "batch bbox");
//  2. YOLO on the ROI     the small YOLO runs only every ``every_n_frames`` frames, only on the ROI
//                         crop (letterboxed, aspect preserved);
//  3. background update   cells away from the person are refreshed;
//  4. fine boxes          background subtraction inside the ROI gives a box for every frame;
//  5. fusion + smoothing  see track_fusion.h (YOLO boxes weigh more than background boxes).
// The box of frame i is final only after frame i + lookahead_frames() has been received.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <vector>

#include <opencv2/core.hpp>

#include "background.h"
#include "config.h"
#include "person_detector.h"
#include "track_fusion.h"
#include "types.h"

namespace fd {

class PersonBoxModule : public IBBoxModule {
public:
    // ``detector`` may be null (background-only boxes).  It must outlive the module.
    PersonBoxModule(const BBoxConfig& cfg, IPersonDetector* detector);

    std::vector<FrameBox> process_batch(const std::vector<Frame>& frames) override;
    int lookahead_frames() const override { return fusion_.lookahead(); }
    bool multi_person_confirmed() const override;

private:
    using GrayMap = std::map<int, cv::Mat>;

    void initialise(const Frame& first);
    void observe(const std::vector<Frame>& frames);
    std::optional<Box> coarse_foreground(const Frame& frame, const GrayMap& gray, int h, int w, cv::Mat& mask_out);
    cv::Mat apply_unknown(const cv::Mat& mask, int index, const GrayMap& gray, cv::Size size, const cv::Mat& cur_small);
    std::optional<Box> make_roi(const std::vector<Box>& boxes, int h, int w, bool with_yolo) const;
    std::map<int, Obs> run_yolo(const std::vector<const Frame*>& yolo_frames, const std::optional<Box>& roi, int h, int w);
    std::optional<Box> fine_box(const Frame& frame, const GrayMap& gray, const Box& roi, int h, int w, const cv::Mat& unknown_full);
    bool passes_gate(int index, const Box& box) const;

    BBoxConfig cfg_;
    IPersonDetector* detector_;
    BackgroundModel bg_;
    TrackFusion fusion_;
    std::optional<Box> last_yolo_box_;
    int no_roi_runs_ = 0;
    std::deque<bool> multi_hist_;
    GrayMap tail_;   // gray frames kept for temporal differencing
};

}  // namespace fd
