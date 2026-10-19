#include "detector.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace fd {
namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
}  // namespace

FallDetector::FallDetector(const DetectorConfig& cfg, IBBoxModule& bbox, ClassicStage& classic, DeepStage& deep,
                           bool keep_history)
    : cfg_(cfg), bbox_(bbox), classic_(classic), deep_(deep), scheduler_(cfg.batch_size, cfg.window_stride),
      keep_history_(keep_history) {
    lookahead_ = bbox.lookahead_frames() + classic.lookahead_frames();
    latency_ = static_cast<int>(std::ceil(static_cast<double>(lookahead_) / cfg.batch_size));
    store_len_ = lookahead_ + 2 * cfg.batch_size + 16;
}

FrameRecord& FallDetector::record(int index) {
    auto it = history_.find(index);
    if (it == history_.end()) {
        FrameRecord r;
        r.index = index;
        r.padded = padded_.count(index) > 0;
        it = history_.emplace(index, r).first;
    }
    return it->second;
}

BatchResult FallDetector::push_batch(const std::vector<Frame>& frames) {
    if (static_cast<int>(frames.size()) != cfg_.batch_size)
        throw std::invalid_argument("expected " + std::to_string(cfg_.batch_size) + " frames per batch, got " +
                                    std::to_string(frames.size()));
    if (frames.front().index != next_frame_)
        throw std::invalid_argument("frames must be consecutive: expected " + std::to_string(next_frame_) + ", got " +
                                    std::to_string(frames.front().index));
    const int k = batches_seen_;
    frame_w_ = frames.front().width;
    frame_h_ = frames.front().height;
    for (const Frame& f : frames) {
        images_[f.index] = f.image;
        if (f.padded) padded_.insert(f.index);
    }
    const int newest = frames.back().index;

    BatchResult result;
    auto t0 = Clock::now();
    const std::vector<FrameBox> final_boxes = bbox_.process_batch(frames);
    result.bbox_ms = ms_since(t0);
    const std::vector<FallEvent> new_events = ingest(final_boxes, newest, result);
    trim(newest);
    ++batches_seen_;
    next_frame_ = newest + 1;

    const int target = k - latency_;
    const bool valid = target >= 0;
    for (const FallEvent& ev : new_events) {
        const int batch_of_event = ev.event_frame / cfg_.batch_size;
        if (batch_of_event < target)
            std::cerr << "warning: event at frame " << ev.event_frame << " decided too late for its aligned batch\n";
        event_batches_.insert(batch_of_event);
    }
    result.batch_index = k;
    result.first_frame = frames.front().index;
    result.last_frame = newest;
    result.valid = valid;
    if (valid) result.target_batch = target;
    result.fall_label = valid && event_batches_.count(target) > 0;
    result.new_events = new_events;
    result.suspended = newest <= suspend_until_;
    return result;
}

std::vector<BatchResult> FallDetector::flush(std::optional<int> pad_batches) {
    const int n = pad_batches ? *pad_batches : latency_;
    std::vector<BatchResult> results;
    const ImagePtr last = images_.at(next_frame_ - 1);
    for (int b = 0; b < n; ++b) {
        const int start = next_frame_;
        std::vector<Frame> pad(cfg_.batch_size);
        for (int i = 0; i < cfg_.batch_size; ++i) {
            Frame& f = pad[i];
            f.index = start + i;
            f.time_s = (start + i) / cfg_.fps;
            f.src_index = -1;
            f.src_time_s = -1.0;
            f.width = frame_w_;
            f.height = frame_h_;
            f.image = last;
            f.padded = true;
        }
        results.push_back(push_batch(pad));
    }
    return results;
}

std::vector<FallEvent> FallDetector::ingest(const std::vector<FrameBox>& final_boxes, int newest, BatchResult& timing) {
    if (keep_history_) {
        for (const FrameBox& fb : final_boxes) {
            FrameRecord& rec = record(fb.index);
            if (fb.box)
                rec.box_norm = std::array<double, 4>{fb.box->x1 / frame_w_, fb.box->y1 / frame_h_, fb.box->x2 / frame_w_,
                                                     fb.box->y2 / frame_h_};
            rec.box_source = fb.source;
            rec.box_score = fb.score;
        }
    }
    auto t0 = Clock::now();
    const ClassicUpdate update = classic_.advance(final_boxes);
    timing.classic_ms = ms_since(t0);
    if (keep_history_) {
        for (const auto& kv : update.svm_prob) record(kv.first).svm_prob = kv.second;
        for (const auto& kv : update.s1) record(kv.first).s1 = kv.second;
        for (const auto& kv : update.s2) record(kv.first).s2 = kv.second;
        for (const auto& kv : update.s3) record(kv.first).s3 = kv.second;
    }

    if (bbox_.multi_person_confirmed()) {
        suspend_until_ = newest + cfg_.multi_person_suspend_batches * cfg_.batch_size;
        scheduler_.reset();
    }
    const bool suspended = newest <= suspend_until_;

    std::vector<std::pair<int, int>> windows;
    for (const auto& kv : update.s3) {                     // std::map iterates in index order
        for (const auto& w : scheduler_.feed(kv.first, kv.second))
            if (!suspended && w.second > cooldown_until_) windows.push_back(w);
    }
    std::vector<FallEvent> events;
    if (!windows.empty()) {
        t0 = Clock::now();
        const std::vector<DeepWindowResult> results = deep_.evaluate(windows, images_, classic_.boxes());
        timing.deep_ms = ms_since(t0);
        for (const DeepWindowResult& res : results) {
            window_log_.push_back(res);
            if (keep_history_ && res.probability) {
                FrameRecord& rec = record(res.end);
                rec.cnn_prob = std::max(rec.cnn_prob.value_or(0.0), *res.probability);
            }
            if (res.positive && res.end > cooldown_until_) {
                bool padded = false;
                for (int i = res.start; i <= res.end; ++i) padded = padded || padded_.count(i) > 0;
                FallEvent ev;
                ev.event_id = static_cast<int>(events_.size());
                ev.window_start = res.start;
                ev.window_end = res.end;
                ev.event_frame = res.end;
                ev.event_time_s = res.end / cfg_.fps;
                ev.probability = *res.probability;
                ev.decided_at_frame = newest;
                ev.latency_frames = newest - res.end;
                ev.involves_padding = padded;
                events_.push_back(ev);
                events.push_back(ev);
                cooldown_until_ = res.end + cfg_.cooldown_batches * cfg_.batch_size;
                if (keep_history_) record(res.end).event = true;
            }
        }
    }
    return events;
}

void FallDetector::trim(int newest) {
    const int keep_from = newest - store_len_;
    images_.erase(images_.begin(), images_.lower_bound(keep_from));
    deep_.trim(keep_from);
}

}  // namespace fd
