#include "yolo_ncnn.h"

#include <stdexcept>
#include <string>

#include <opencv2/imgproc.hpp>

#include "ncnn/net.h"

namespace fd {

struct YoloNcnn::Impl {
    ncnn::Net net;
};

YoloNcnn::YoloNcnn(const YoloConfig& cfg, const std::string& models_dir, int num_threads)
    : impl_(new Impl), cfg_(cfg) {
    impl_->net.opt.use_vulkan_compute = false;
    impl_->net.opt.num_threads = num_threads;
    const std::string param = models_dir + "/" + cfg.param, bin = models_dir + "/" + cfg.bin;
    if (impl_->net.load_param(param.c_str()) != 0) throw std::runtime_error("cannot load ncnn param file: " + param);
    if (impl_->net.load_model(bin.c_str()) != 0) throw std::runtime_error("cannot load ncnn model file: " + bin);
}

YoloNcnn::~YoloNcnn() = default;

std::vector<std::vector<Detection>> YoloNcnn::detect(const std::vector<cv::Mat>& images) {
    std::vector<std::vector<Detection>> results;
    const int size = cfg_.input_size;
    for (const cv::Mat& image : images) {
        // letterbox: uniform resize + padding, exactly like the Python pre-processing
        const Letterbox lb = letterbox_params(image.cols, image.rows, size);
        cv::Mat resized;
        cv::resize(image, resized, cv::Size(lb.new_w, lb.new_h), 0, 0, cv::INTER_LINEAR);
        cv::Mat canvas(size, size, CV_8UC3, cv::Scalar(114, 114, 114));
        resized.copyTo(canvas(cv::Rect(lb.pad_x, lb.pad_y, lb.new_w, lb.new_h)));
        cv::Mat rgb;
        cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

        ncnn::Mat in = ncnn::Mat::from_pixels(rgb.data, ncnn::Mat::PIXEL_RGB, size, size);
        const float norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
        in.substract_mean_normalize(nullptr, norm);

        ncnn::Extractor ex = impl_->net.create_extractor();
        ex.input(cfg_.input_name.c_str(), in);
        ncnn::Mat out;                                    // (h = 4 + classes, w = anchors)
        ex.extract(cfg_.output_name.c_str(), out);
        // expected layout: h = 4 box values + class scores (84 for COCO), w = anchors (2100 at 320 px)
        if (out.dims != 2 || out.h < 5 || out.w <= out.h)
            throw std::runtime_error("unexpected ncnn output shape " + std::to_string(out.w) + "x" + std::to_string(out.h) +
                                     " (dims " + std::to_string(out.dims) + "); expected channels x anchors, e.g. 84 x 2100");
        results.push_back(decode_yolo(static_cast<const float*>(out.data), out.h, out.w, /*person_class=*/0,
                                      static_cast<float>(cfg_.conf), static_cast<float>(cfg_.iou_nms), lb, image.cols,
                                      image.rows));
    }
    return results;
}

}  // namespace fd
