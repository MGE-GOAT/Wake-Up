#include "onnx_models.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include "json_util.h"
#include "vision_utils.h"

namespace fd {
namespace {

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "fall_detector");
    return env;
}

Ort::SessionOptions session_options(int threads) {
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetIntraOpNumThreads(std::max(1, threads));
    options.SetInterOpNumThreads(1);
    return options;
}

std::string meta_path_for(const std::string& model_path) {
    const size_t dot = model_path.rfind('.');
    return (dot == std::string::npos ? model_path : model_path.substr(0, dot)) + ".meta.json";
}

std::vector<std::string> input_names(Ort::Session& s) {
    Ort::AllocatorWithDefaultOptions alloc;
    std::vector<std::string> names;
    for (size_t i = 0; i < s.GetInputCount(); ++i) names.emplace_back(s.GetInputNameAllocated(i, alloc).get());
    return names;
}

std::vector<std::string> output_names(Ort::Session& s) {
    Ort::AllocatorWithDefaultOptions alloc;
    std::vector<std::string> names;
    for (size_t i = 0; i < s.GetOutputCount(); ++i) names.emplace_back(s.GetOutputNameAllocated(i, alloc).get());
    return names;
}

// The model must have been trained with the feature definition of the current configuration.
void check_signature(const std::string& meta, const FeatureSpec& spec, const std::string& name, bool strict) {
    const auto stored = json_string(meta, "feature_signature");
    if (!stored) {
        std::cerr << "warning: " << name << " has no feature_signature in its meta file; cannot verify it\n";
    } else if (*stored != spec.signature()) {
        const std::string msg = name + " was trained with feature signature " + *stored + " but the configuration gives " +
                                spec.signature() + " (point_order / channels / margins differ)";
        if (strict) throw std::runtime_error(msg);
        std::cerr << "warning: " << msg << "\n";
    }
}

std::string load_meta(const std::string& model_path) {
    try { return read_text_file(meta_path_for(model_path)); }
    catch (const std::exception&) { std::cerr << "warning: no meta file next to " << model_path << "\n"; return ""; }
}

// Run a single float input and fetch the requested outputs (names as given).
std::vector<Ort::Value> run(Ort::Session& session, const std::string& input, std::vector<float>& data,
                            const std::vector<int64_t>& shape, const std::vector<std::string>& outputs) {
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, data.data(), data.size(), shape.data(), shape.size());
    const char* in_names[] = {input.c_str()};
    std::vector<const char*> out_names;
    for (const std::string& o : outputs) out_names.push_back(o.c_str());
    return session.Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names.data(), out_names.size());
}

}  // namespace

// ------------------------------------------------------------------ RTMPose-t
struct OrtPose::Impl {
    explicit Impl(const std::string& path, int threads) : session(ort_env(), path.c_str(), session_options(threads)) {
        input = input_names(session).at(0);
        outputs = output_names(session);
    }
    Ort::Session session;
    std::string input;
    std::vector<std::string> outputs;   // simcc_x, simcc_y
};

OrtPose::OrtPose(const PoseConfig& cfg, const std::string& models_dir, int num_threads)
    : impl_(new Impl(models_dir + "/" + cfg.model, num_threads)), cfg_(cfg) {
    if (impl_->outputs.size() < 2) throw std::runtime_error("RTMPose model must have the outputs simcc_x and simcc_y");
}

OrtPose::~OrtPose() = default;

std::vector<PoseResult> OrtPose::estimate(const std::vector<ImagePtr>& images, const std::vector<Box>& boxes) {
    const int n = static_cast<int>(images.size());
    const int W = cfg_.input_w, H = cfg_.input_h;
    const double aspect = static_cast<double>(W) / H;
    const float mean[3] = {123.675f, 116.28f, 103.53f}, stdev[3] = {58.395f, 57.12f, 57.375f};
    std::vector<float> batch(static_cast<size_t>(n) * 3 * H * W);
    std::vector<CropGeometry> crops;
    for (int i = 0; i < n; ++i) {
        const CropGeometry g = pose_crop_geometry(boxes[i], aspect, cfg_.bbox_padding);
        crops.push_back(g);
        // uniform-scale affine warp: the crop is never squeezed (black padding outside the image)
        const double s = W / g.scale_w;
        cv::Mat matrix(2, 3, CV_32F);
        matrix.at<float>(0, 0) = static_cast<float>(s);
        matrix.at<float>(0, 1) = 0.f;
        matrix.at<float>(0, 2) = static_cast<float>(W / 2.0 - s * g.cx);
        matrix.at<float>(1, 0) = 0.f;
        matrix.at<float>(1, 1) = static_cast<float>(s);
        matrix.at<float>(1, 2) = static_cast<float>(H / 2.0 - s * g.cy);
        cv::Mat crop;
        cv::warpAffine(*images[i], crop, matrix, cv::Size(W, H), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        if (cfg_.rgb_input) cv::cvtColor(crop, crop, cv::COLOR_BGR2RGB);
        float* dst = batch.data() + static_cast<size_t>(i) * 3 * H * W;
        for (int y = 0; y < H; ++y) {
            const unsigned char* row = crop.ptr<unsigned char>(y);
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c)
                    dst[(static_cast<size_t>(c) * H + y) * W + x] = (static_cast<float>(row[x * 3 + c]) - mean[c]) / stdev[c];
        }
    }
    auto outs = run(impl_->session, impl_->input, batch, {n, 3, H, W}, {impl_->outputs[0], impl_->outputs[1]});
    const auto shape_x = outs[0].GetTensorTypeAndShapeInfo().GetShape();   // (n, 17, bins_x)
    const auto shape_y = outs[1].GetTensorTypeAndShapeInfo().GetShape();
    const int bins_x = static_cast<int>(shape_x.at(2)), bins_y = static_cast<int>(shape_y.at(2));
    const float* sx = outs[0].GetTensorData<float>();
    const float* sy = outs[1].GetTensorData<float>();
    std::vector<PoseResult> results;
    for (int i = 0; i < n; ++i)
        results.push_back(decode_simcc(sx + static_cast<size_t>(i) * 17 * bins_x, sy + static_cast<size_t>(i) * 17 * bins_y,
                                       bins_x, bins_y, cfg_.simcc_split_ratio, W, H, crops[i]));
    return results;
}

// ------------------------------------------------------------------ SVM
struct OrtSvm::Impl {
    Impl(const std::string& path, int threads) : session(ort_env(), path.c_str(), session_options(threads)) {
        input = input_names(session).at(0);
        const auto names = output_names(session);
        prob_name = std::find(names.begin(), names.end(), "probabilities") != names.end() ? "probabilities" : names.back();
    }
    Ort::Session session;
    std::string input, prob_name;
};

OrtSvm::OrtSvm(const std::string& model_path, const FeatureSpec& spec, bool strict_signature, int num_threads)
    : impl_(new Impl(model_path, num_threads)) {
    const std::string meta = load_meta(model_path);
    if (!meta.empty()) {
        check_signature(meta, spec, "svm", strict_signature);
        threshold_ = json_number(meta, "threshold");
        if (auto classes = json_int_list(meta, "classes")) {
            auto it = std::find(classes->begin(), classes->end(), 1);
            positive_column_ = it != classes->end() ? static_cast<int>(it - classes->begin()) : 1;
        }
    }
}

OrtSvm::~OrtSvm() = default;

std::vector<double> OrtSvm::predict_proba(const std::vector<float>& features, int n) {
    std::vector<float> x = features;
    auto outs = run(impl_->session, impl_->input, x, {n, 80}, {impl_->prob_name});
    const float* p = outs[0].GetTensorData<float>();                      // (n, classes)
    const int classes = static_cast<int>(outs[0].GetTensorTypeAndShapeInfo().GetShape().at(1));
    std::vector<double> probs(n);
    for (int i = 0; i < n; ++i) probs[i] = static_cast<double>(p[i * classes + positive_column_]);
    return probs;
}

// ------------------------------------------------------------------ CNN
struct OrtCnn::Impl {
    Impl(const std::string& path, int threads) : session(ort_env(), path.c_str(), session_options(threads)) {
        input = input_names(session).at(0);
        output = output_names(session).at(0);
    }
    Ort::Session session;
    std::string input, output;
};

OrtCnn::OrtCnn(const std::string& model_path, const FeatureSpec& spec, bool strict_signature, int num_threads)
    : impl_(new Impl(model_path, num_threads)) {
    const std::string meta = load_meta(model_path);
    if (!meta.empty()) {
        check_signature(meta, spec, "cnn", strict_signature);
        threshold_ = json_number(meta, "threshold");
    }
    const auto shape = impl_->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();   // (N, T, 15, C)
    if (shape.size() != 4 || shape[1] != spec.window || shape[2] != 15 || shape[3] != spec.channels)
        throw std::runtime_error("cnn input shape does not match the configured representation (expected N x " +
                                 std::to_string(spec.window) + " x 15 x " + std::to_string(spec.channels) + ")");
    row_size_ = spec.window * 15 * spec.channels;
}

OrtCnn::~OrtCnn() = default;

std::vector<double> OrtCnn::predict_proba(const std::vector<float>& tensors, int n) {
    std::vector<float> x = tensors;
    const auto shape = impl_->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    auto outs = run(impl_->session, impl_->input, x, {n, shape[1], shape[2], shape[3]}, {impl_->output});
    const float* p = outs[0].GetTensorData<float>();                      // (n, 1)
    std::vector<double> probs(n);
    for (int i = 0; i < n; ++i) probs[i] = static_cast<double>(p[i]);
    return probs;
}

}  // namespace fd
