#include "fall_adapter.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "pipeline.h"        // fall_cpp/src: fd::Pipeline, fd::DetectorInstance
#include "video_source.h"    // fd::resize_to_max_side

namespace fall_adapter {

// Optionally pin the calling thread (the fall worker) and the threads it creates afterwards (ncnn OpenMP
// workers, ONNX Runtime pools) to the cores in FALL_CPU_AFFINITY, for example "2,3". Off by default, because the
// models are meant to use up to 4 threads and the worker runs at a low priority instead (see lower_priority()).
static void pin_fall_threads() {
    const char* env = std::getenv("FALL_CPU_AFFINITY");
    const std::string spec = env ? env : "none";
    if (spec == "none" || spec.empty()) return;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 4) return;
    cpu_set_t set; CPU_ZERO(&set);
    std::stringstream ss(spec); std::string tok; int n = 0;
    while (std::getline(ss, tok, ',')) {
        int c = std::atoi(tok.c_str());
        if (c >= 0 && c < ncpu) { CPU_SET(c, &set); ++n; }
    }
    if (n == 0) return;
    if (sched_setaffinity(0, sizeof(set), &set) != 0) std::cerr << "[fall] could not pin threads to " << spec << "\n";
    else std::cout << "[fall] fall threads pinned to cores " << spec << "\n";
}

// Run the calling thread, and the model thread pools it creates afterwards, at a lower priority than the
// rest of the device. The fall detector then uses the cores the wake word and the camera do not need.
// FALL_NICE sets the nice value (default 10, 0 disables).
static void lower_priority() {
    const char* env = std::getenv("FALL_NICE");
    const int nice_value = env ? std::atoi(env) : 10;
    if (nice_value <= 0) return;
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice_value) != 0)
        std::cerr << "[fall] could not set nice " << nice_value << "\n";
}

struct FallEngine::Impl {
    fd::Config cfg;
    std::unique_ptr<fd::Pipeline> pipeline;          // loads the (stateless) models once
    std::unique_ptr<fd::DetectorInstance> instance;  // stateful: box module, stages, detector
    int next_index = 0;
    // Inference runs on its own worker thread so the camera loop never waits for the models (same design as
    // fall_cpp's own camera mode: a frame queue with a bounded backlog, oldest frames dropped when behind).
    std::mutex m;
    std::condition_variable cv_work, cv_idle;
    std::deque<std::vector<fall::FrameClass>> pending;   // full batches waiting for the worker
    bool busy = false, stop = false;
    Result acc;                                          // results finished since the last frame_inference() call
    std::thread worker;
    const bool log_stats = std::getenv("FALL_STATS") != nullptr;   // per-stage timing, every kStatsEvery batches
    int stats_batches = 0;
    double sum_bbox = 0, sum_classic = 0, sum_deep = 0;

    // The worker thread loads the models, so every thread pool they create (ncnn, ONNX Runtime) inherits the
    // worker's priority and core pinning. The constructor waits until the models are loaded.
    explicit Impl(const std::string& path) : cfg(fd::load_config(path)) {
        std::promise<void> ready;
        std::future<void> loaded = ready.get_future();
        worker = std::thread([this, &ready] { run(ready); });
        try { loaded.get(); }
        catch (...) { worker.join(); throw; }
    }

    ~Impl() {
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv_work.notify_all();
        worker.join();
    }

    void run(std::promise<void>& ready) {
        lower_priority();
        pin_fall_threads();
        try {
            pipeline = std::make_unique<fd::Pipeline>(cfg);
            instance = pipeline->create(/*keep_history=*/false);   // bounded memory
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        ready.set_value();
        for (;;) {
            std::vector<fall::FrameClass> batch;
            {
                std::unique_lock<std::mutex> lk(m);
                cv_work.wait(lk, [this] { return stop || !pending.empty(); });
                if (stop) return;
                batch = std::move(pending.front());
                pending.pop_front();
                busy = true;
            }
            Result r;
            try { r = infer(std::move(batch)); }
            catch (const std::exception& e) { std::cerr << "[fall] frame_inference threw: " << e.what() << "\n"; }
            {
                std::lock_guard<std::mutex> lk(m);
                busy = false;
                acc.fall_label = acc.fall_label || r.fall_label;
                acc.probability = std::max(acc.probability, r.probability);
                acc.valid = r.valid;
                acc.suspended = r.suspended;
                acc.bbox_ms = r.bbox_ms; acc.classic_ms = r.classic_ms; acc.deep_ms = r.deep_ms;
            }
            cv_idle.notify_all();
        }
    }

    Result infer(std::vector<fall::FrameClass> batch);   // one batch through the detector, worker thread only
};

FallEngine::FallEngine(const std::string& config_path) : impl_(std::make_unique<Impl>(config_path)) {}
FallEngine::~FallEngine() = default;

int FallEngine::batch_size() const { return impl_->cfg.batch_size; }
int FallEngine::latency_batches() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->instance->detector.latency_batches();
}

void FallEngine::insertGapSentinel() {
    std::unique_lock<std::mutex> lk(impl_->m);
    impl_->pending.clear();
    impl_->cv_idle.wait(lk, [this] { return !impl_->busy; });
    impl_->instance = impl_->pipeline->create(false);
    impl_->next_index = 0;
    impl_->acc = Result{};
}

// Non-blocking: queues the batch for the worker and returns whatever the worker finished since the last call
// (so a decision can arrive up to one batch later than the call that produced it).
Result FallEngine::frame_inference(std::vector<fall::FrameClass> batch) {
    if (static_cast<int>(batch.size()) != impl_->cfg.batch_size)
        throw std::runtime_error("fall_adapter: batch must hold exactly batch_size frames");
    Result out;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->pending.push_back(std::move(batch));
        const size_t max_backlog = static_cast<size_t>(std::max(1, impl_->cfg.max_backlog_batches));
        while (impl_->pending.size() > max_backlog) { impl_->pending.pop_front(); ++dropped; }
        out = impl_->acc;
        impl_->acc.fall_label = false;
        impl_->acc.probability = 0.0;
    }
    impl_->cv_work.notify_one();
    if (dropped) std::cerr << "[fall] processing is behind real time, dropped " << dropped << " batch(es)\n";
    return out;
}

Result FallEngine::drain() {
    std::unique_lock<std::mutex> lk(impl_->m);
    impl_->cv_idle.wait(lk, [this] { return impl_->pending.empty() && !impl_->busy; });
    Result out = impl_->acc;
    impl_->acc.fall_label = false;
    impl_->acc.probability = 0.0;
    return out;
}

Result FallEngine::Impl::infer(std::vector<fall::FrameClass> batch) {
    const double fps = cfg.target_fps;
    std::vector<fd::Frame> frames;
    frames.reserve(batch.size());
    for (auto& fc : batch) {
        fd::Frame f;
        f.index = next_index++;
        f.time_s = f.index / fps;
        f.src_index = f.index;
        f.src_time_s = f.time_s;
        f.image = fd::resize_to_max_side(fc.frame, cfg.max_side);   // no-op for 640x480
        f.width = f.image->cols;
        f.height = f.image->rows;
        f.padded = false;
        frames.push_back(std::move(f));
    }
    const fd::BatchResult r = instance->detector.push_batch(frames);
    Result out;
    out.valid = r.valid;
    out.suspended = r.suspended;
    out.bbox_ms = r.bbox_ms;
    out.classic_ms = r.classic_ms;
    out.deep_ms = r.deep_ms;
    if (log_stats) {
        constexpr int kStatsEvery = 10;
        sum_bbox += r.bbox_ms;
        sum_classic += r.classic_ms;
        sum_deep += r.deep_ms;
        if (++stats_batches == kStatsEvery) {
            std::cout << "[fall] " << kStatsEvery << " batches, mean ms per batch: bbox "
                      << sum_bbox / kStatsEvery << ", classic " << sum_classic / kStatsEvery
                      << ", deep " << sum_deep / kStatsEvery << (r.valid ? "" : " (warming up)") << "\n";
            stats_batches = 0;
            sum_bbox = sum_classic = sum_deep = 0;
        }
    }
    for (const fd::FallEvent& e : r.new_events) out.probability = std::max(out.probability, e.probability);
    out.fall_label = !r.new_events.empty();
    return out;
}

}  // namespace fall_adapter
