#include "video_source.h"

#include <chrono>
#include <cmath>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace fd {

ImagePtr resize_to_max_side(const cv::Mat& image, int max_side) {
    if (max_side > 0) {
        const double scale = max_side / static_cast<double>(std::max(image.rows, image.cols));
        if (scale < 1.0) {
            auto out = std::make_shared<cv::Mat>();
            cv::resize(image, *out,
                       cv::Size(std::max(1, static_cast<int>(std::nearbyint(image.cols * scale))),
                                std::max(1, static_cast<int>(std::nearbyint(image.rows * scale)))),
                       0, 0, cv::INTER_AREA);
            return out;
        }
    }
    return std::make_shared<cv::Mat>(image);
}

// ------------------------------------------------------------------ FileSource
struct FileSource::Lazy {
    cv::Mat raw;
    ImagePtr resized;
    ImagePtr get(int max_side) {
        if (!resized) resized = resize_to_max_side(raw, max_side);
        return resized;
    }
};

FileSource::FileSource(const std::string& path, double target_fps, double fallback_fps, int max_side)
    : cap_(path), target_fps_(target_fps), fps_(fallback_fps), max_side_(max_side), resampler_(target_fps) {
    if (!cap_.isOpened()) throw std::runtime_error("cannot open video: " + path);
    const double fps = cap_.get(cv::CAP_PROP_FPS);
    if (fps > 0 && fps <= 240) fps_ = fps;
    resampler_ = Resampler(target_fps, 1.0 / fps_);
}

Frame FileSource::make_frame(const Selection& s, const std::shared_ptr<Lazy>& image) const {
    Frame f;
    f.index = s.k;
    f.time_s = s.k / target_fps_;
    f.src_index = s.src_index;
    f.src_time_s = s.src_time;
    f.image = image->get(max_side_);
    f.width = f.image->cols;
    f.height = f.image->rows;
    return f;
}

bool FileSource::next(Frame& out) {
    while (pending_.empty()) {
        if (eof_) return false;
        auto lazy = std::make_shared<Lazy>();
        if (!cap_.read(lazy->raw)) {                       // end of file: flush the remaining selections
            eof_ = true;
            for (const Selection& s : resampler_.finish()) pending_.push_back(make_frame(s, cur_));
            continue;
        }
        prev_ = cur_;
        cur_ = lazy;
        for (const Selection& s : resampler_.push(src_index_, src_index_ / fps_))
            pending_.push_back(make_frame(s, s.src_index == src_index_ ? cur_ : prev_));
        ++src_index_;
    }
    out = pending_.front();
    pending_.pop_front();
    return true;
}

// ------------------------------------------------------------------ CameraSource
CameraSource::CameraSource(const std::string& source, int width, int height, double target_fps, int max_side,
                           bool trust_rate)
    : target_fps_(target_fps), max_side_(max_side), trust_rate_(trust_rate), resampler_(target_fps) {
    const bool is_index = !source.empty() && source.find_first_not_of("0123456789") == std::string::npos;
    if (is_index) cap_.open(std::stoi(source)); //, cv::CAP_V4L2
    else cap_.open(source); //, cv::CAP_V4L2
    if (!cap_.isOpened()) throw std::runtime_error("cannot open camera: " + source);
    if (width > 0) cap_.set(cv::CAP_PROP_FRAME_WIDTH, width);
    if (height > 0) cap_.set(cv::CAP_PROP_FRAME_HEIGHT, height);
}

bool CameraSource::next(Frame& out) {
    while (pending_.empty()) {
        cv::Mat raw;
        if (!cap_.read(raw) || raw.empty()) return false;
        const ImagePtr image = resize_to_max_side(raw.clone(), max_side_);
        Frame f;
        f.image = image;
        f.width = image->cols;
        f.height = image->rows;
        if (trust_rate_) {
            f.index = k_++;
            f.time_s = f.src_time_s = f.index / target_fps_;
            f.src_index = src_index_;
            pending_.push_back(f);
        } else {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!has_t0_) { t0_ = now; has_t0_ = true; }
            for (const Selection& s : resampler_.push(src_index_, now - t0_)) {
                Frame g = f;
                if (s.src_index != src_index_ && prev_image_) g.image = prev_image_;   // the previous frame was nearer
                g.index = s.k;
                g.time_s = s.k / target_fps_;
                g.src_index = s.src_index;
                g.src_time_s = s.src_time;
                pending_.push_back(g);
            }
        }
        prev_image_ = image;
        ++src_index_;
    }
    out = pending_.front();
    pending_.pop_front();
    return true;
}

}  // namespace fd
