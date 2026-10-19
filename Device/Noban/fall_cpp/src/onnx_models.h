// ONNX Runtime models: RTMPose-t (pose), SVM and CNN (cascade classifiers).
// Each classifier reads its <model>.meta.json sidecar (written by the Python training code):
// feature signature (must match the current configuration), decision threshold, class order.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "config.h"
#include "types.h"

namespace fd {

class OrtPose : public IPose {
public:
    OrtPose(const PoseConfig& cfg, const std::string& models_dir, int num_threads);
    ~OrtPose() override;
    std::vector<PoseResult> estimate(const std::vector<ImagePtr>& images, const std::vector<Box>& boxes) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PoseConfig cfg_;
};

class OrtSvm : public ISvm {
public:
    OrtSvm(const std::string& model_path, const FeatureSpec& spec, bool strict_signature, int num_threads);
    ~OrtSvm() override;
    std::vector<double> predict_proba(const std::vector<float>& features, int n) override;
    std::optional<double> threshold() const override { return threshold_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::optional<double> threshold_;
    int positive_column_ = 1;
};

class OrtCnn : public ICnn {
public:
    OrtCnn(const std::string& model_path, const FeatureSpec& spec, bool strict_signature, int num_threads);
    ~OrtCnn() override;
    std::vector<double> predict_proba(const std::vector<float>& tensors, int n) override;
    std::optional<double> threshold() const override { return threshold_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::optional<double> threshold_;
    int row_size_ = 0;
};

}  // namespace fd
