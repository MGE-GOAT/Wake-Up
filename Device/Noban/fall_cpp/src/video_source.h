// Frame sources producing the canonical 16 FPS stream (see resampler.h):
//  * FileSource   - a video file, resampled by timestamps (j / fps)
//  * CameraSource - a live camera; if it already delivers target_fps frames per second
//                   (camera.trust_camera_rate) every frame becomes the next canonical frame
#pragma once

#include <deque>
#include <memory>
#include <string>

#include <opencv2/videoio.hpp>

#include "resampler.h"
#include "types.h"

namespace fd {

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool next(Frame& out) = 0;   // false at end of stream
};

class FileSource : public FrameSource {
public:
    FileSource(const std::string& path, double target_fps, double fallback_fps, int max_side);
    bool next(Frame& out) override;

private:
    struct Lazy;                         // decoded frame, resized at most once, only if selected
    Frame make_frame(const Selection& s, const std::shared_ptr<Lazy>& image) const;

    cv::VideoCapture cap_;
    double target_fps_, fps_;
    int max_side_, src_index_ = 0;
    Resampler resampler_;
    std::shared_ptr<Lazy> prev_, cur_;
    std::deque<Frame> pending_;
    bool eof_ = false;
};

class CameraSource : public FrameSource {
public:
    CameraSource(const std::string& source, int width, int height, double target_fps, int max_side, bool trust_rate);
    bool next(Frame& out) override;

private:
    cv::VideoCapture cap_;
    double target_fps_;
    int max_side_, src_index_ = 0, k_ = 0;
    bool trust_rate_;
    Resampler resampler_;
    std::deque<Frame> pending_;
    bool has_t0_ = false;
    double t0_ = 0;
    ImagePtr prev_image_;   // previous source frame (the resampler may select it)
};

// Downscale (never upscale) keeping the aspect ratio so that the longer side is ``max_side`` (0 = off).
ImagePtr resize_to_max_side(const cv::Mat& image, int max_side);

}  // namespace fd
