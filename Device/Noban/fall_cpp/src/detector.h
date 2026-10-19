// Streaming fall detector: 16 consecutive frames in, Boolean ``fall_label`` out.
//
// STREAMING CONTRACT
//  * Input : push_batch() takes exactly B (=16) consecutive canonical frames (16 fps).
//  * Delay : decisions need future frames (smoothing, centred windows, post-processing); the total
//            look-ahead is lookahead_frames() and the answer is delayed by D = latency_batches()
//            = ceil(lookahead / B) batches (3 with the default settings).
//  * Output of call k (BatchResult):
//      valid         false for the first D calls (warm-up)
//      target_batch  k - D, the batch this answer is about
//      fall_label    THE required Boolean: true iff a fall event is attributed to target_batch
//                    (the batch that holds the LAST frame of the CNN window that fired);
//                    always false while valid is false
//      new_events    events decided during this call (final; may be earlier than fall_label)
//  * Cooldown after an event, suspension while several people are confirmed.
#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "classic_stage.h"
#include "deep_stage.h"
#include "types.h"

namespace fd {

struct DetectorConfig {
    int batch_size = 16;
    double fps = 16.0;
    int window_stride = 2;
    int cooldown_batches = 3;
    int multi_person_suspend_batches = 3;
};

struct FallEvent {
    int event_id = 0;
    int window_start = 0, window_end = 0;
    int event_frame = 0;            // == window_end
    double event_time_s = 0;
    double probability = 0;
    int decided_at_frame = 0;       // newest frame received when the event was decided
    int latency_frames = 0;
    bool involves_padding = false;
};

struct FrameRecord {                // per-frame diagnostics, filled progressively
    int index = 0;
    std::optional<std::array<double, 4>> box_norm;
    std::string box_source;         // empty = none
    double box_score = 0;
    std::optional<double> svm_prob;
    std::optional<int> s1, s2, s3;
    std::optional<double> cnn_prob;
    bool event = false;
    bool padded = false;
};

struct BatchResult {
    int batch_index = 0, first_frame = 0, last_frame = 0;
    bool valid = false;
    std::optional<int> target_batch;
    bool fall_label = false;
    std::vector<FallEvent> new_events;
    bool suspended = false;
    double bbox_ms = 0, classic_ms = 0, deep_ms = 0;   // wall time of each stage for this batch
};

class FallDetector {
public:
    FallDetector(const DetectorConfig& cfg, IBBoxModule& bbox, ClassicStage& classic, DeepStage& deep, bool keep_history);

    BatchResult push_batch(const std::vector<Frame>& frames);
    std::vector<BatchResult> flush(std::optional<int> pad_batches = std::nullopt);

    int batch_size() const { return cfg_.batch_size; }
    double config_fps() const { return cfg_.fps; }
    int lookahead_frames() const { return lookahead_; }
    int latency_batches() const { return latency_; }
    const std::vector<FallEvent>& events() const { return events_; }
    const std::map<int, FrameRecord>& history() const { return history_; }
    const std::vector<DeepWindowResult>& window_log() const { return window_log_; }

private:
    FrameRecord& record(int index);
    std::vector<FallEvent> ingest(const std::vector<FrameBox>& final_boxes, int newest, BatchResult& timing);
    void trim(int newest);

    DetectorConfig cfg_;
    IBBoxModule& bbox_;
    ClassicStage& classic_;
    DeepStage& deep_;
    RunScheduler scheduler_;
    bool keep_history_;
    int lookahead_, latency_, store_len_;

    std::map<int, FrameRecord> history_;
    std::vector<DeepWindowResult> window_log_;
    std::vector<FallEvent> events_;
    std::set<int> event_batches_;
    ImageMap images_;
    std::set<int> padded_;
    int frame_w_ = 0, frame_h_ = 0;
    int cooldown_until_ = -1, suspend_until_ = -1;
    int batches_seen_ = 0, next_frame_ = 0;
};

}  // namespace fd
