// Writes the per-video result of infer mode as JSON, using the same field names as the Python
// framework's workspace/results/<video>.json so that tools/compare_with_python.py can diff them.
#pragma once

#include <string>
#include <vector>

#include "detector.h"

namespace fd {

struct VideoRun {
    std::string video_id;               // file name
    std::string path;
    int n_frames = 0;                   // real (non padded) canonical frames
    double target_fps = 16;
    std::vector<int> src_index;         // source frame of each real canonical frame
    std::vector<BatchResult> batches;
};

void write_result_json(const std::string& path, const VideoRun& run, const FallDetector& detector);

}  // namespace fd
