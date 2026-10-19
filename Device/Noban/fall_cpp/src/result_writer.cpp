#include "result_writer.h"

#include <fstream>
#include <stdexcept>

#include "json_util.h"

namespace fd {
namespace {

std::string opt_num(const std::optional<double>& v) { return v ? json_num(*v) : "null"; }
std::string opt_int(const std::optional<int>& v) { return v ? std::to_string(*v) : "null"; }
const char* tf(bool b) { return b ? "true" : "false"; }

}  // namespace

void write_result_json(const std::string& path, const VideoRun& run, const FallDetector& det) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "{\n \"video_id\": " << json_str(run.video_id) << ",\n \"path\": " << json_str(run.path)
        << ",\n \"n_frames\": " << run.n_frames << ",\n \"target_fps\": " << json_num(run.target_fps)
        << ",\n \"lookahead_frames\": " << det.lookahead_frames() << ",\n \"latency_batches\": " << det.latency_batches()
        << ",\n \"src_index\": [";
    for (size_t i = 0; i < run.src_index.size(); ++i) out << (i ? "," : "") << run.src_index[i];
    out << "],\n \"records\": [";
    bool first = true;
    for (int k = 0; k < run.n_frames; ++k) {
        auto it = det.history().find(k);
        if (it == det.history().end()) continue;
        const FrameRecord& r = it->second;
        out << (first ? "\n  " : ",\n  ") << "{\"index\": " << r.index << ", \"box_norm\": ";
        first = false;
        if (r.box_norm) {
            out << "[" << json_num((*r.box_norm)[0]) << "," << json_num((*r.box_norm)[1]) << "," << json_num((*r.box_norm)[2])
                << "," << json_num((*r.box_norm)[3]) << "]";
        } else {
            out << "null";
        }
        out << ", \"box_source\": " << (r.box_source.empty() ? "null" : json_str(r.box_source))
            << ", \"box_score\": " << json_num(r.box_score) << ", \"svm_prob\": " << opt_num(r.svm_prob)
            << ", \"s1\": " << opt_int(r.s1) << ", \"s2\": " << opt_int(r.s2) << ", \"s3\": " << opt_int(r.s3)
            << ", \"cnn_prob\": " << opt_num(r.cnn_prob) << ", \"event\": " << tf(r.event)
            << ", \"padded\": " << tf(r.padded) << "}";
    }
    out << "\n ],\n \"events\": [";
    for (size_t i = 0; i < det.events().size(); ++i) {
        const FallEvent& e = det.events()[i];
        out << (i ? ",\n  " : "\n  ") << "{\"event_id\": " << e.event_id << ", \"window_start\": " << e.window_start
            << ", \"window_end\": " << e.window_end << ", \"event_frame\": " << e.event_frame
            << ", \"event_time_s\": " << json_num(e.event_time_s) << ", \"probability\": " << json_num(e.probability)
            << ", \"decided_at_frame\": " << e.decided_at_frame << ", \"latency_frames\": " << e.latency_frames
            << ", \"involves_padding\": " << tf(e.involves_padding) << "}";
    }
    out << "\n ],\n \"batches\": [";
    for (size_t i = 0; i < run.batches.size(); ++i) {
        const BatchResult& b = run.batches[i];
        out << (i ? ",\n  " : "\n  ") << "{\"batch\": " << b.batch_index << ", \"valid\": " << tf(b.valid)
            << ", \"target_batch\": " << opt_int(b.target_batch) << ", \"fall\": " << tf(b.fall_label)
            << ", \"suspended\": " << tf(b.suspended) << "}";
    }
    out << "\n ],\n \"deep_windows\": [";
    for (size_t i = 0; i < det.window_log().size(); ++i) {
        const DeepWindowResult& w = det.window_log()[i];
        out << (i ? ",\n  " : "\n  ") << "{\"start\": " << w.start << ", \"end\": " << w.end
            << ", \"probability\": " << opt_num(w.probability) << ", \"positive\": " << tf(w.positive)
            << ", \"status\": " << json_str(w.status) << ", \"mean_confidence\": " << json_num(w.mean_confidence)
            << ", \"min_frame_confidence\": " << json_num(w.min_frame_confidence) << "}";
    }
    out << "\n ]\n}\n";
}

}  // namespace fd
