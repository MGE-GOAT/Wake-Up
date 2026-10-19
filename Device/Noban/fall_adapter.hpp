// =========================================================
// fall_adapter — thin bridge from our camera loop to the vendored fall_cpp detector
// (yolo11n + RTMPose-t + SVM + CNN, see fall_cpp/UPSTREAM.md).
//
// It keeps the call shape of the old fall::FallDetection used by fallPipelineThread
// (frame_inference / insertGapSentinel) so main.cpp changes stay small:
//   * frame_inference(16 frames) -> queues the batch for a worker thread and returns at once, so the camera
//     loop never waits for the models. The returned .fall_label is true when the detector DECIDED a fall
//     event in any batch finished since the previous call (so up to one batch later than the call that
//     produced it). If the worker falls behind, the oldest queued batches are dropped (fall_cpp's own camera
//     mode does the same, see camera.max_backlog_batches).
//   * insertGapSentinel() starts a fresh stream (after a call or guest-mode pause) — the models stay loaded,
//     the detector re-warms for latency_batches() batches.
// Compiled only when USE_FALL_CPP=1 (CMake option, default ON).
// =========================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fall_detection.hpp"   // fall::FrameClass (frame + fps), the type the capture loop already builds

namespace fall_adapter {

struct Result {
    bool fall_label  = false;   // a fall event was decided during this batch
    bool valid       = false;   // detector past its warm-up
    bool suspended   = false;   // several people confirmed — detector is muted
    double probability = 0.0;   // CNN probability of the newest event (0 if none)
    double bbox_ms = 0, classic_ms = 0, deep_ms = 0;   // wall time per stage for this batch
};

class FallEngine {
public:
    // config_path: fall_cpp/noban.yaml (relative to the working directory) or an absolute path.
    explicit FallEngine(const std::string& config_path);
    ~FallEngine();
    FallEngine(const FallEngine&) = delete;
    FallEngine& operator=(const FallEngine&) = delete;

    // Exactly batch_size() consecutive BGR frames (16 at 16 fps). Non-blocking, see the note at the top.
    Result frame_inference(std::vector<fall::FrameClass> batch);
    void insertGapSentinel();
    // Waits until every queued batch is processed, then returns what finished since the last call
    // (like frame_inference without queueing a batch). Used by the offline test mode, never by the live path.
    Result drain();
    int batch_size() const;
    int latency_batches() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fall_adapter
