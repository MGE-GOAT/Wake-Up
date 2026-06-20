// =========================================================
// fall_detection — direct port of fall_detection.py.
//
// 16-frame batched pipeline:
//   1. ExtractBackground motion-aware bg
//   2. Coarse bbox over downsampled frames (extract_approximate_batch_bbox)
//   3. Per-frame fine bbox inside the coarse crop (extract_main_bboxes)
//   4. Interpolate missing + Gaussian-smooth bboxes across time
//   5. Build 80-dim relative feature, push through SVM (ONNX)
//   6. Two postprocess windows (out1 -> out2 -> out3)
//   7. On all-1 batch: build 416-dim MoVeNet-keypoint feature,
//      run MLP confirmer (ONNX), fire fall if final_predict > 0.9
//
// Models:
//   SVM_rbf.onnx           — ONNX,    input "X" [16,80], output[1] probabilities
//   Last_stage_Model.onnx  — ONNX,    input "inputs" [1,15,16,2], output prob
//   4.tflite               — TFLite,  MoVeNet input [1,192,192,3] uint8,
//                                       output [1,1,17,3] float32
// =========================================================
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

// ONNX Runtime
#include "onnxruntime_cxx_api.h"

// TFLite
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

#include "extract_background.hpp"

namespace fall {

// ─── Public types ──────────────────────────────────────────────────────────

// A captured camera frame plus its measured per-frame FPS. Mirrors
// all_together.py's Frame_class (which is `frame` + `fps`).
struct FrameClass {
    cv::Mat frame;
    double  fps;
};

// 5-component bbox in (y1, y2, x1, x2, ratio) normalized form. Mirrors
// the per-frame bbox arrays returned by extract_main_bboxes /
// extract_approximate_batch_bbox / interpolate_and_smooth_bboxes.
struct Bbox5 {
    double y1, y2, x1, x2, ratio;
};
// Optional bbox — None in Python ↔ empty std::optional here.
using OptBbox5 = std::optional<Bbox5>;

// Per-frame state carried through the pipeline. Mirrors
// fall_detection.py:frame_info.
struct FrameInfo {
    cv::Mat frame_img;          // the BGR frame (mutable, gets annotations)
    cv::Mat background_img;     // copy of the static background at this batch
    OptBbox5 smoothed_bbox;     // None until interpolate_and_smooth_bboxes
    cv::Vec4i batch_bbox{};     // (y1, y2, x1, x2) — pixel space
    bool batch_bbox_set = false;
    std::vector<double> relative_feature_vector80;  // empty until set
    cv::Mat movenet_result;     // (13, 3) double; rows are [y, x, score]
    bool movenet_result_set = false;
    double human_conf = 0.0;
    double fps = 0.0;
};

// ─── FallDetection (the runtime equivalent of `class fall_detection`) ───────
class FallDetection {
public:
    // Construct with full frame (H, W). Loads SVM, MLP and MoVeNet models
    // from CWD-relative paths SVM_rbf.onnx, Last_stage_Model.onnx, 4.tflite.
    FallDetection(int H, int W);

    // Process one 16-frame batch. Returns:
    //   .annotated_frames — vector<Mat> the annotated target batch (frame
    //                       images at -int(batch_size*4.5) .. +batch_size),
    //                       OR empty if batch_number < 7.
    //   .fall_label       — true iff a fall was confirmed in this call.
    // Direct mirror of `frame_inference(frame_obj_batch)`.
    struct InferenceResult {
        std::vector<cv::Mat> annotated_frames;  // empty == Python's `None`
        bool fall_label = false;
    };
    InferenceResult frame_inference(std::vector<FrameClass> frame_obj_batch);

    // Test mode: "normal" (default), "Full_load" (force out1=all 1s),
    // "light_load" (force out1=all 0s). Mirrors self.test_type.
    std::string test_type = "normal";

    // Per-batch state snapshot used by the --fall-video parity harness.
    // Mirrors what run_fall_python.py extracts from the Python detector's
    // deques after each frame_inference() call.
    struct StateSummary {
        int out1_sum;          // last 16 of out1 deque
        int out2_sum;          // last 16 of out2 deque
        int out3_sum;          // last 16 of out3 deque
        int bbox_present;      // count of non-None in last 16 of bboxes
        int smoothed_present;  // count of frames in [-32:-16] window with smoothed_bbox
        int ignore;            // current self.ignore counter
    };
    StateSummary getStateSummary() const;

    // Snapshot the last 16 raw bboxes (post extract_main_bboxes, pre-smoothing)
    // for parity dumping. Each row is (y1,y2,x1,x2,ratio) in normalized coords,
    // or all NaN if the slot is None.
    std::vector<std::array<double,5>> getLast16Bboxes() const;

    // Insert a "data gap" marker into the bbox deque so the smoothing /
    // interpolation stages won't bridge across a multi-second pause (e.g.
    // an incoming WebRTC call that took the camera away). Call this once
    // when capture resumes after such a gap.
    void insertGapSentinel();

private:
    // ─── State (sizes mirror Python's deque(maxlen=128)) ───────────────────
    static constexpr int    DEQUE_LEN          = 128;
    static constexpr int    BATCH_SIZE         = 16;
    static constexpr double PAD                = 0.01;     // unused in Python too
    static constexpr int    SMOOTHING_WINDOW   = 13;

    int H_, W_;

    // Morphology kernels (Python: cv2.getStructuringElement)
    cv::Mat kernel1_;   // 7x7 ellipse  — used in extract_approximate_batch_bbox
    cv::Mat kernel2_;   // 5x5 ellipse  — used in extract_main_bboxes

    // Gaussian smoothing weights, length SMOOTHING_WINDOW
    std::vector<double> gaussian_weights_;

    // Per-batch rolling state.
    std::deque<OptBbox5> smoothed_bboxes_;  // unused stash (Python keeps it too)
    std::deque<FrameInfo> frame_info_list_;
    std::deque<OptBbox5>  bboxes_;
    std::deque<int>       out1_;
    std::deque<int>       out2_;
    std::deque<int>       out3_;

    // Motion-aware background subtraction
    ExtractBackground extract_bg_;

    // SVM cutoff constants (frame_inference / high_sensitive)
    static constexpr double Vc0 = -0.001;
    static constexpr double Vc1 =  0.02;
    static constexpr double Vr0 =  0.001;
    static constexpr double Vr1 =  0.01;
    static constexpr int    T   = BATCH_SIZE / 2;  // 8
    static constexpr int    L   = BATCH_SIZE / 2;  // 8
    static constexpr double w0  = BATCH_SIZE / 4.0; // 4.0

    // ONNX models
    Ort::Env                        ort_env_;
    Ort::SessionOptions             ort_opts_;
    std::unique_ptr<Ort::Session>   svm_session_;
    std::unique_ptr<Ort::Session>   mlp_session_;
    std::string                     mlp_output_name_;

    // TFLite MoVeNet
    std::unique_ptr<tflite::FlatBufferModel> tflite_model_;
    std::unique_ptr<tflite::Interpreter>     tflite_interp_;

    int  batch_number_ = 0;
    int  ignore_       = 0;
    bool fall_label_   = false;

    // Timing reservoirs (kept for parity with Python; not currently printed).
    std::deque<double> detect_bbox_bg_time_;
    std::deque<double> interpolate_smooth_time_;
    std::deque<double> svm_inference_time_;
    std::deque<double> post_process_time_;
    std::deque<double> deep_classifier_time_;
    std::deque<double> all_time_;

    // ─── Helpers (one per Python method, same names) ─────────────────────
    static std::vector<double> gaussian_window(int size, double sigma);

    // Coarse bbox over 4 downsampled frames; returns pixel-space (y1,y2,x1,x2).
    // std::optional empty == Python None.
    std::optional<cv::Vec4i> extract_approximate_batch_bbox(
        const std::vector<cv::Mat>& frame_batch,
        const cv::Mat& background);

    // Per-frame bbox inside the coarse crop; len == frame_batch.size().
    std::vector<OptBbox5> extract_main_bboxes(
        const std::vector<cv::Mat>& frame_batch,
        const cv::Mat& background,
        const cv::Vec4i& batch_bbox);

    // Convert axis-aligned rect to a square (or near-square) of side
    // = min(max(w,h)*1.4, H, W), centered and clamped.
    cv::Vec4i bounding_square_box(const std::array<double,4>& bbox, int H, int W);

    // Interpolate Nones (linear) then 13-tap Gaussian smooth across time.
    std::vector<OptBbox5> interpolate_and_smooth_bboxes(
        const std::vector<OptBbox5>& bbox_list);

    // Build 80-dim feature + frame_bbox from 16 smoothed bboxes.
    struct Feat80Result {
        std::vector<double> feat;   // length 80
        cv::Vec4i           frame_bbox; // (y1, y2, x1, x2)
    };
    Feat80Result extract_feature_vector80(const std::vector<FrameInfo*>& frame_info_list);

    // Re-express feature_vector_80 relative to frame_bbox.
    std::vector<double> relativalize_feature_vector_80(
        const std::vector<double>& feature_vector_80,
        const cv::Vec4i& frame_bbox,
        int H, int W);

    // Compute the per-batch bbox from a feature_vector_80 average.
    cv::Vec4i calculate_frame_bbox(
        const std::vector<double>& feature_vector_80, int H, int W);

    // Bounding rect (not square) box for MoVeNet input cropping.
    cv::Vec4i calculate_bounding_rect_box(
        const Bbox5& bbox, int H, int W);

    // SVM stage. Output[i] = "this frame is suspicious".
    std::array<int, 16> high_sensitive(
        const std::vector<std::vector<double>>& feature_matrix_16x80,
        double Vc0_, double Vc1_, double Vr0_, double Vr1_);

    // SVM model inference. Returns the [16,2] probability matrix.
    // (Python: self.SVM_model.run(None,{"X": ...})[1])
    std::vector<std::array<double, 2>> svm_predict(
        const std::vector<std::vector<double>>& feature_matrix_16x80);

    // MoVeNet inference: returns (17, 3) keypoints (y, x, score) in [0..1].
    // Image is BGR; converted to RGB + resized with pad to 192x192 uint8.
    cv::Mat get_movenet_keypoints(const cv::Mat& image_bgr);

    // Average the first 5 keypoints into one, append remaining 12.
    // Input (17, 3) -> Output (13, 3).
    cv::Mat process_keypoints(const cv::Mat& keypoints);

    // Run MoVeNet on a cropped person from a smoothed_bbox.
    // Output: (13, 3) keypoints in full-frame pixel coords (y, x, score).
    cv::Mat apply_movenet(const cv::Mat& frame, const Bbox5& bbox);

    // Gaussian smoothing of a 1D signal, weighted by per-sample confidence.
    std::vector<double> smooth_signal(
        const std::vector<double>& signal,
        const std::vector<double>& score_signal);

    // Build 416-dim MoVeNet-derived signal feature from 16-frame batch.
    // Returns std::nullopt if mean conf < 0.2 or min conf < 0.1.
    std::optional<std::vector<double>> extract_feature_vector416(
        std::vector<FrameInfo*>& frame_zip_list,
        const cv::Vec4i& batch_bbox);

    // (vector_416, vector_80) -> tensor of shape (15, 16, 2) for MLP input.
    // Returns row-major flat float32 array of length 15*16*2 = 480.
    std::vector<float> split_features(
        const std::vector<double>& vector_416,
        const std::vector<double>& vector_80);

    // Annotations (drawing on frame_img in place). Each matches its Python
    // counterpart line-for-line.
    void annotate_fps_on_frame(const std::vector<FrameInfo*>& frames);
    void annotate_time_on_frames(std::vector<cv::Mat>& frames, double duration);
    void annotate_bg_on_frames(std::vector<FrameInfo*>& frames);
    void annotate_bbox_on_frame2(std::vector<FrameInfo*>& frames);
    void annotate_keypoints_on_frame(FrameInfo& frame_obj);
    void annotate_final_fall_on_frames(std::vector<FrameInfo*>& frames);

    // Trim a deque to DEQUE_LEN entries from the right (newest).
    template <typename T>
    static void trim_back(std::deque<T>& d, int maxlen = DEQUE_LEN) {
        while (static_cast<int>(d.size()) > maxlen) d.pop_front();
    }
};

} // namespace fall
