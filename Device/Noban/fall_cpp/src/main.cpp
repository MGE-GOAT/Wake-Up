// Fall detector for Raspberry Pi: camera frames (16 FPS) -> full cascade -> Boolean fall_label.
//
//   fall_detector <config.yaml> [camera|infer]
//
//  camera : read the camera, print one line per 16-frame batch with the Boolean fall_label
//  infer  : run the pipeline on video files and write one JSON result per video (same fields as the
//           Python framework's infer results) for comparison with tools/compare_with_python.py
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>
#include <thread>

#include "config.h"
#include "json_util.h"
#include "pipeline.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string safe_name(const std::string& path) {   // same convention as the Python results (spaces/brackets removed)
    std::string name = std::filesystem::path(path).filename().string(), out;
    for (char c : name) {
        if (c == ' ') out.push_back('_');
        else if (c != '(' && c != ')') out.push_back(c);
    }
    return out;
}

void run_infer(fd::Config& cfg) {
    std::vector<std::string> videos = cfg.videos;
    if (!cfg.video_list_file.empty()) {
        std::istringstream lines(fd::read_text_file(cfg.video_list_file));
        std::string line;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') videos.push_back(line);
        }
    }
    if (videos.empty()) throw std::runtime_error("infer mode needs infer.videos or infer.video_list_file");
    std::filesystem::create_directories(cfg.output_dir);

    fd::Pipeline pipeline(cfg);
    int video_no = 0;
    for (const std::string& path : videos) {
        ++video_no;
        auto t0 = std::chrono::steady_clock::now();
        fd::FileSource source(path, cfg.target_fps, cfg.fallback_fps, cfg.max_side);
        auto instance = pipeline.create(/*keep_history=*/true);
        fd::VideoRun run;
        run.video_id = std::filesystem::path(path).filename().string();
        run.path = path;
        run.target_fps = cfg.target_fps;
        run.batches = fd::run_stream(instance->detector, source, run.n_frames, run.src_index);
        const std::string out_path = cfg.output_dir + "/" + safe_name(path) + ".json";
        fd::write_result_json(out_path, run, instance->detector);

        double bbox_ms = 0, classic_ms = 0, deep_ms = 0;
        for (const fd::BatchResult& b : run.batches) { bbox_ms += b.bbox_ms; classic_ms += b.classic_ms; deep_ms += b.deep_ms; }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%d/%zu] %s: %d frames (%.1f s of video) in %.1f s | events: %zu", video_no, videos.size(),
                    run.video_id.c_str(), run.n_frames, run.n_frames / cfg.target_fps, wall,
                    instance->detector.events().size());
        for (const fd::FallEvent& e : instance->detector.events()) std::printf(" [frame %d p=%.2f]", e.event_frame, e.probability);
        std::printf(" | ms: bbox %.0f classic %.0f deep %.0f -> %s\n", bbox_ms, classic_ms, deep_ms, out_path.c_str());
        std::fflush(stdout);
    }
}

// Camera frames are read by a separate thread so that capture never waits for the models.
struct FrameQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<fd::Frame> frames;
    bool closed = false;
};

void run_camera(fd::Config& cfg) {
    fd::Pipeline pipeline(cfg);
    auto instance = pipeline.create(/*keep_history=*/false);        // no history: bounded memory
    fd::CameraSource camera(cfg.camera_source, cfg.camera_width, cfg.camera_height, cfg.target_fps, cfg.max_side,
                            cfg.trust_camera_rate);
    const int B = cfg.batch_size;
    const size_t max_backlog = static_cast<size_t>(cfg.max_backlog_batches) * B;

    FrameQueue queue;
    std::thread capture([&] {
        fd::Frame f;
        while (!g_stop && camera.next(f)) {
            std::lock_guard<std::mutex> lock(queue.mutex);
            queue.frames.push_back(f);
            if (queue.frames.size() > max_backlog) {               // processing is slower than real time
                const size_t drop = queue.frames.size() - max_backlog;
                queue.frames.erase(queue.frames.begin(), queue.frames.begin() + drop);
                std::fprintf(stderr, "warning: processing is behind real time, dropped %zu frames\n", drop);
            }
            queue.cv.notify_one();
        }
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.closed = true;
        queue.cv.notify_all();
    });

    int next_index = 0;
    std::vector<fd::Frame> batch;
    while (!g_stop) {
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !queue.frames.empty() || queue.closed; });
            if (queue.frames.empty()) { if (queue.closed) break; continue; }
            fd::Frame f = queue.frames.front();
            queue.frames.pop_front();
            f.index = next_index++;                                // keep the detector's stream consecutive
            f.time_s = f.index / cfg.target_fps;
            batch.push_back(f);
        }
        if (static_cast<int>(batch.size()) < B) continue;
        const fd::BatchResult r = instance->detector.push_batch(batch);
        batch.clear();
        for (const fd::FallEvent& e : r.new_events)
            std::printf("FALL event: frame %d (t=%.1f s) p=%.2f, decided %.1f s later\n", e.event_frame, e.event_time_s,
                        e.probability, e.latency_frames / cfg.target_fps);
        std::printf("batch %d valid=%d about_batch=%d fall_label=%d%s", r.batch_index, r.valid ? 1 : 0,
                    r.target_batch.value_or(-1), r.fall_label ? 1 : 0, r.suspended ? " (suspended: several people)" : "");
        if (cfg.log_timing)
            std::printf("  | ms bbox %.0f classic %.0f deep %.0f (budget %.0f)", r.bbox_ms, r.classic_ms, r.deep_ms,
                        1000.0 * B / cfg.target_fps);
        std::printf("\n");
        std::fflush(stdout);
    }
    g_stop = true;
    capture.join();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <config.yaml> [camera|infer]\n";
        return 2;
    }
    try {
        fd::Config cfg = fd::load_config(argv[1]);
        if (argc > 2) cfg.mode = argv[2];
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        if (cfg.mode == "infer") run_infer(cfg);
        else if (cfg.mode == "camera") run_camera(cfg);
        else throw std::runtime_error("mode must be 'camera' or 'infer'");
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
