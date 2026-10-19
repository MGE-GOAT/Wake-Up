// Shared plain data types and the interfaces the detector depends on.
// This header (and everything "core") does not include OpenCV: images are only passed around as
// opaque shared pointers, so the cascade logic can be unit-tested without any library.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cv { class Mat; }

namespace fd {

struct Box {  // [x1, y1, x2, y2] in pixels of the processing frame
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

using ImagePtr = std::shared_ptr<cv::Mat>;

// One frame of the canonical 16 FPS stream.
struct Frame {
    int index = 0;           // canonical frame index k (0-based)
    double time_s = 0;       // k / target_fps
    int src_index = 0;       // index of the source frame that was selected (-1 for padding)
    double src_time_s = 0;
    int width = 0, height = 0;
    ImagePtr image;          // BGR image at processing resolution
    bool padded = false;     // synthesised by tail padding
};

// Final (smoothed) person box of one frame.
struct FrameBox {
    int index = 0;
    std::optional<Box> box;
    std::string source;      // "yolo" | "bg" | "interp" | "" (none)
    double score = 0;
};

struct PoseResult {          // COCO-17 keypoints in pixels of the frame the pose was computed on
    double kp[17][2] = {};
    double score[17] = {};
};

using BoxMap = std::map<int, std::optional<Box>>;       // frame index -> final box (or none)
using ImageMap = std::map<int, ImagePtr>;

// ---- interfaces (implemented by the OpenCV / ONNX Runtime / ncnn classes, or by test stubs) ----
struct IBBoxModule {
    virtual ~IBBoxModule() = default;
    virtual std::vector<FrameBox> process_batch(const std::vector<Frame>& frames) = 0;
    virtual int lookahead_frames() const = 0;
    virtual bool multi_person_confirmed() const = 0;
};

struct ISvm {
    virtual ~ISvm() = default;
    // features: n x 80 row-major. Returns the probability of the "suspicious" class per row.
    virtual std::vector<double> predict_proba(const std::vector<float>& features, int n) = 0;
    virtual std::optional<double> threshold() const = 0;   // threshold stored next to the model
};

struct IPose {
    virtual ~IPose() = default;
    virtual std::vector<PoseResult> estimate(const std::vector<ImagePtr>& images, const std::vector<Box>& boxes) = 0;
};

struct ICnn {
    virtual ~ICnn() = default;
    // tensors: n x (T*15*C) row-major. Returns the fall probability per row.
    virtual std::vector<double> predict_proba(const std::vector<float>& tensors, int n) = 0;
    virtual std::optional<double> threshold() const = 0;
};

}  // namespace fd
