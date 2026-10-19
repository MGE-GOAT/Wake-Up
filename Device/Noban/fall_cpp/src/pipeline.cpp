#include "pipeline.h"

namespace fd {

Pipeline::Pipeline(const Config& cfg) : cfg_(cfg) {
    if (cfg.bbox.yolo.enabled) yolo_.reset(new YoloNcnn(cfg.bbox.yolo, cfg.models_dir, cfg.yolo_threads));
    pose_.reset(new OrtPose(cfg.pose, cfg.models_dir, cfg.pose_threads));
    svm_.reset(new OrtSvm(cfg.models_dir + "/" + cfg.svm_model, cfg.spec, cfg.strict_signature, cfg.classifier_threads));
    cnn_.reset(new OrtCnn(cfg.models_dir + "/" + cfg.cnn_model, cfg.spec, cfg.strict_signature, cfg.classifier_threads));
}

std::unique_ptr<DetectorInstance> Pipeline::create(bool keep_history) {
    return std::unique_ptr<DetectorInstance>(new DetectorInstance(cfg_, yolo_.get(), *svm_, *pose_, *cnn_, keep_history));
}

std::vector<BatchResult> run_stream(FallDetector& detector, FrameSource& source, int& n_real_frames,
                                    std::vector<int>& src_index) {
    const int B = detector.batch_size();
    std::vector<BatchResult> results;
    std::vector<Frame> batch;
    n_real_frames = 0;
    Frame f;
    while (source.next(f)) {
        src_index.push_back(f.src_index);
        batch.push_back(f);
        ++n_real_frames;
        if (static_cast<int>(batch.size()) == B) {
            results.push_back(detector.push_batch(batch));
            batch.clear();
        }
    }
    if (n_real_frames == 0) return results;
    if (!batch.empty()) {                                  // complete the last partial batch by repetition
        const Frame last = batch.back();
        while (static_cast<int>(batch.size()) < B) {
            Frame pad = last;
            pad.index = batch.back().index + 1;
            pad.time_s = pad.index / detector.config_fps();
            pad.src_index = -1;
            pad.src_time_s = -1.0;
            pad.padded = true;
            batch.push_back(pad);
        }
        results.push_back(detector.push_batch(batch));
    }
    for (const BatchResult& r : detector.flush()) results.push_back(r);
    return results;
}

}  // namespace fd
