// Wires the models and the cascade together.  Pipeline loads the (expensive, stateless) models once;
// create() builds a fresh set of stateful objects (box module, stages, detector) for one stream/video.
#pragma once

#include <memory>
#include <string>

#include "classic_stage.h"
#include "config.h"
#include "deep_stage.h"
#include "detector.h"
#include "onnx_models.h"
#include "person_box_module.h"
#include "result_writer.h"
#include "video_source.h"
#include "yolo_ncnn.h"

namespace fd {

// Everything the detector references must live as long as the detector itself.
struct DetectorInstance {
    DetectorInstance(const Config& cfg, IPersonDetector* yolo, ISvm& svm, IPose& pose, ICnn& cnn, bool keep_history)
        : bbox(cfg.bbox, yolo), classic(cfg.classic, cfg.spec, svm), deep(cfg.deep, cfg.spec, pose, cnn),
          detector(cfg.detector, bbox, classic, deep, keep_history) {}
    PersonBoxModule bbox;
    ClassicStage classic;
    DeepStage deep;
    FallDetector detector;
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);
    std::unique_ptr<DetectorInstance> create(bool keep_history);
    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::unique_ptr<YoloNcnn> yolo_;
    std::unique_ptr<OrtPose> pose_;
    std::unique_ptr<OrtSvm> svm_;
    std::unique_ptr<OrtCnn> cnn_;
};

// Push a whole frame stream through ``detector`` (batches of B frames).  At the end the last partial
// batch is completed by repeating its last frame and flush() appends latency_batches padded batches,
// so that decisions about the real tail are emitted.  Returns the batch results.
std::vector<BatchResult> run_stream(FallDetector& detector, FrameSource& source, int& n_real_frames,
                                    std::vector<int>& src_index);

}  // namespace fd
