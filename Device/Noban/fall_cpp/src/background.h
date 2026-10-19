// Grid-based background model.
// The image is divided into grid x grid cells; a cell of the stored background is refreshed from the
// current frame only when the person is NOT inside it, so the person never becomes background while
// the rest of the scene keeps adapting.  Cells that contain the person in the very first frame are
// flagged "unknown": their background is untrustworthy until refreshed once.
#pragma once

#include <optional>
#include <vector>

#include <opencv2/core.hpp>

#include "types.h"

namespace fd {

using Grid = std::vector<unsigned char>;   // grid*grid flags, row-major

class BackgroundModel {
public:
    explicit BackgroundModel(int grid) : grid_(grid), unknown_(grid * grid, 0) {}

    bool ready() const { return !image_.empty(); }
    bool any_unknown() const;
    const cv::Mat& gray() const { return gray_; }

    void init(const cv::Mat& frame, const std::optional<Box>& person);
    // ``person``: detector box.  If the detector found nobody the caller passes ``occupied`` (cells with
    // visible foreground) so that a person the detector missed is not absorbed into the background.
    void update(const cv::Mat& frame, const std::optional<Box>& person, const std::optional<Grid>& occupied, double margin);

    cv::Mat unknown_mask(int width, int height) const;   // CV_8U, 255 = background untrusted
    Grid cells_from_mask(const cv::Mat& mask) const;     // cells containing any foreground pixel

private:
    Grid cells_hit_by(const Box& box, double margin) const;

    int grid_;
    cv::Mat image_, gray_;
    Grid unknown_;
    std::vector<int> ys_, xs_;
};

}  // namespace fd
