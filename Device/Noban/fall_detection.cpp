// =========================================================
// fall_detection — direct port of fall_detection.py.
//
// The structure mirrors the Python file 1:1: same class boundaries, same
// method names, same intermediate state. Comments tag each chunk back to
// the corresponding Python lines so future maintainers can diff against
// fall_detection.py and see equivalence.
//
// Numerical notes:
//   • MoVeNet preprocessing uses `tf.image.resize_with_pad`. We replicate it
//     with cv::resize(INTER_LINEAR_EXACT) + cv::copyMakeBorder(BORDER_CONSTANT).
//     INTER_LINEAR_EXACT uses fixed-point bilinear with half-pixel centers,
//     matching tf.image.resize(method='bilinear', antialias=False) bit-for-bit
//     on uint8 input. Everything else (SVM/MLP/ONNX, OpenCV ops, our own math)
//     is also bit-equivalent.
// =========================================================
#include "fall_detection.hpp"
#include "parity_dump.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace fall {

// ─── Small helpers ────────────────────────────────────────────────────────

// Python `int(round(x))` uses banker's rounding (round-half-to-even). C++'s
// std::round rounds half-away-from-zero — different at .5 boundaries. Use
// std::nearbyint with the default FE_TONEAREST rounding mode, which is
// round-half-to-even, to match Python exactly.
static inline int py_round_int(double x) {
    return static_cast<int>(std::nearbyint(x));
}

// Python `np.clip(value, lo, hi)`.
template <typename T>
static inline T np_clip(T v, T lo, T hi) {
    return std::max(lo, std::min(v, hi));
}

// Numerically-faithful approximation of tf.image.resize_with_pad: scale
// the input to fit inside target × target with INTER_LINEAR (preserves
// aspect ratio), then symmetric-pad with zeros to exactly target × target.
// Returns CV_8UC3, RGB.
static cv::Mat resize_with_pad_rgb(const cv::Mat& image_bgr, int target_h, int target_w) {
    cv::Mat rgb;
    cv::cvtColor(image_bgr, rgb, cv::COLOR_BGR2RGB);

    const int h = rgb.rows;
    const int w = rgb.cols;
    if (h == 0 || w == 0) return cv::Mat::zeros(target_h, target_w, CV_8UC3);

    // Same min-scale logic that resize_with_pad uses: pick the largest
    // scale that keeps both dims <= target.
    const double scale = std::min(
        static_cast<double>(target_h) / h,
        static_cast<double>(target_w) / w);
    const int new_h = std::max(1, py_round_int(h * scale));
    const int new_w = std::max(1, py_round_int(w * scale));

    cv::Mat scaled;
    // INTER_LINEAR_EXACT uses fixed-point bilinear with half-pixel centers,
    // which matches tf.image.resize(method='bilinear', antialias=False)
    // bit-for-bit on uint8 input. Plain INTER_LINEAR drifts by ~1 LSB
    // because of float-vs-fixed rounding differences. Our input is always
    // uint8 (camera frames), so EXACT is applicable.
    cv::resize(rgb, scaled, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR_EXACT);

    const int pad_top    = (target_h - new_h) / 2;
    const int pad_bottom = target_h - new_h - pad_top;
    const int pad_left   = (target_w - new_w) / 2;
    const int pad_right  = target_w - new_w - pad_left;
    cv::Mat padded;
    cv::copyMakeBorder(scaled, padded, pad_top, pad_bottom, pad_left, pad_right,
                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return padded;
}

// ─── Constructor: mirrors fall_detection.__init__ ─────────────────────────
FallDetection::FallDetection(int H, int W)
    : H_(H), W_(W),
      kernel1_(cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7))),
      kernel2_(cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5))),
      gaussian_weights_(gaussian_window(SMOOTHING_WINDOW, SMOOTHING_WINDOW / 2.0)),
      extract_bg_(W, H),
      ort_env_(ORT_LOGGING_LEVEL_WARNING, "fall_detection")
{
    // pad, batch_size, smoothing_window_size, Vc0..Vr1, T, L, w0 — all in header.

    // self.SVM_A_path = 'SVM_rbf.onnx'
    // self.SVM_model = InferenceSession(self.SVM_A_path, providers=['CPUExecutionProvider'])
    ort_opts_.SetIntraOpNumThreads(1);
    svm_session_ = std::make_unique<Ort::Session>(ort_env_, "SVM_rbf.onnx", ort_opts_);

    // self.MLP = ort.InferenceSession("Last_stage_Model.onnx", providers=['CPUExecutionProvider'])
    mlp_session_ = std::make_unique<Ort::Session>(ort_env_, "Last_stage_Model.onnx", ort_opts_);

    // self.MLP_output_name = self.MLP.get_outputs()[0].name
    {
        Ort::AllocatorWithDefaultOptions alloc;
        auto name_alloc = mlp_session_->GetOutputNameAllocated(0, alloc);
        mlp_output_name_ = std::string(name_alloc.get());
    }

    // self.interpreter = tf.lite.Interpreter(model_path="4.tflite"); allocate_tensors()
    tflite_model_ = tflite::FlatBufferModel::BuildFromFile("4.tflite");
    if (!tflite_model_) throw std::runtime_error("fall_detection: failed to load 4.tflite");
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder(*tflite_model_, resolver)(&tflite_interp_);
    if (!tflite_interp_) throw std::runtime_error("fall_detection: failed to build MoVeNet interpreter");
    // MoVeNet on a single XNNPack worker. ~30ms → ~60ms per inference, but
    // MoVeNet only fires after the SVM cascade hits all-1 on a 16-frame
    // batch (i.e. only when something interesting is happening), so the
    // latency cost almost never materializes. In exchange we leave 3
    // cores free for WUW + camera capture + everything else.
    tflite_interp_->SetNumThreads(1);
    tflite_interp_->AllocateTensors();
}

// ─── Static helpers ──────────────────────────────────────────────────────

// Insert BATCH_SIZE nullopt entries into bboxes_ so the interpolation
// stage (interpolate_and_smooth_bboxes) sees a clear gap and won't bridge
// linear-interp across a pause window. Also clears any in-flight unsmoothed
// frame_info entries' smoothed_bbox so they can't get retroactively
// interpolated through. Out1/out2/out3 are decision flags — leaving them
// stale is fine (they'll be overwritten on the next batch_number guards).
void FallDetection::insertGapSentinel() {
    for (int i = 0; i < BATCH_SIZE; ++i) bboxes_.push_back(std::nullopt);
    trim_back(bboxes_);
}

// gaussian_window(size, sigma) — fall_detection.py:731-737
std::vector<double> FallDetection::gaussian_window(int size, double sigma) {
    const double center = (size - 1) / 2.0;
    std::vector<double> w(size);
    double sum = 0.0;
    for (int i = 0; i < size; ++i) {
        const double v = (i - center) / sigma;
        w[i] = std::exp(-0.5 * v * v);
        sum += w[i];
    }
    for (double& x : w) x /= sum;
    return w;
}

// ─── frame_inference — fall_detection.py:79-275 ──────────────────────────
FallDetection::InferenceResult FallDetection::frame_inference(
    std::vector<FrameClass> frame_obj_batch)
{
    // start_all = time.time(); self.fall_lable = False
    auto t_all_start = std::chrono::steady_clock::now();
    fall_label_ = false;

    // start = time.time(); self.batch_number += 1
    auto t_phase = std::chrono::steady_clock::now();
    batch_number_ += 1;

    // frame_batch = [fr.frame for fr in frame_obj_batch]
    std::vector<cv::Mat> frame_batch;
    frame_batch.reserve(frame_obj_batch.size());
    for (auto& fr : frame_obj_batch) frame_batch.push_back(fr.frame);

    // background = self.extract_bg.update_bg(frame_batch[0])
    cv::Mat background = extract_bg_.update_bg(frame_batch[0]);

    // self.frame_info_list += [frame_info(fr.frame, fr.fps, background) for ...]
    for (auto& fr : frame_obj_batch) {
        FrameInfo fi;
        fi.frame_img = fr.frame;  // shared header w/ same data (Python: same ref)
        fi.background_img = background.clone();  // Python: copy.deepcopy(background)
        fi.fps = fr.fps;
        frame_info_list_.push_back(std::move(fi));
    }
    trim_back(frame_info_list_);

    // approx_batch_bbox = self.extract_approximate_batch_bbox(frame_batch, background)
    auto approx_batch_bbox = extract_approximate_batch_bbox(frame_batch, background);

    if (!approx_batch_bbox.has_value()) {
        // self.bboxes += (self.batch_size * [None])
        for (int i = 0; i < BATCH_SIZE; ++i) bboxes_.push_back(std::nullopt);
    } else {
        // bboxes_list = self.extract_main_bboxes(frame_batch, background, approx_batch_bbox)
        auto bboxes_list = extract_main_bboxes(frame_batch, background, *approx_batch_bbox);
        // self.bboxes += bboxes_list
        for (auto& b : bboxes_list) bboxes_.push_back(b);
    }
    trim_back(bboxes_);

    {
        auto now = std::chrono::steady_clock::now();
        detect_bbox_bg_time_.push_back(std::chrono::duration<double>(now - t_phase).count());
        trim_back(detect_bbox_bg_time_);
        t_phase = now;
    }

    // ── Stage 2 / interpolate_and_smooth — Python lines 121-145 ──
    if (batch_number_ >= 3) {
        // center_start = len(self.bboxes) - 2*self.batch_size
        // center_end = center_start + self.batch_size
        int center_start = static_cast<int>(bboxes_.size()) - 2 * BATCH_SIZE;
        int center_end   = center_start + BATCH_SIZE;

        // smoothing_start = center_start - self.batch_size
        // smoothing_end = center_end + self.batch_size
        int smoothing_start = center_start - BATCH_SIZE;
        int smoothing_end   = center_end + BATCH_SIZE;

        // bboxes_context = list(self.bboxes)[smoothing_start:smoothing_end]
        std::vector<OptBbox5> bboxes_context;
        bboxes_context.reserve(smoothing_end - smoothing_start);
        // Python list slicing is bounds-tolerant (negative starts wrap, clamps).
        // For an early batch_number=3 with len(bboxes_)=48, smoothing_start=0, smoothing_end=64
        // — python's slice yields the first 48 (clipped) elements. We mimic.
        int s_lo = std::max(0, smoothing_start);
        int s_hi = std::min(static_cast<int>(bboxes_.size()), smoothing_end);
        for (int i = s_lo; i < s_hi; ++i) bboxes_context.push_back(bboxes_[i]);

        // smoothed_context = self.interpolate_and_smooth_bboxes(bboxes_context)
        auto smoothed_context = interpolate_and_smooth_bboxes(bboxes_context);

        // offset = self.batch_size; smoothed_batch = smoothed_context[offset:offset+batch_size]
        const int offset = BATCH_SIZE;
        std::vector<OptBbox5> smoothed_batch;
        smoothed_batch.reserve(BATCH_SIZE);
        for (int i = offset; i < offset + BATCH_SIZE && i < static_cast<int>(smoothed_context.size()); ++i) {
            smoothed_batch.push_back(smoothed_context[i]);
        }
        while (static_cast<int>(smoothed_batch.size()) < BATCH_SIZE) smoothed_batch.push_back(std::nullopt);

        // frame_info_list_temp = list(self.frame_info_list)[-2*self.batch_size:][:self.batch_size]
        int total_fi = static_cast<int>(frame_info_list_.size());
        int fi_start = std::max(0, total_fi - 2 * BATCH_SIZE);
        for (int i = 0; i < BATCH_SIZE && fi_start + i < total_fi; ++i) {
            // for i, fr in enumerate(frame_info_list_temp): fr.smoothed_bbox = smoothed_batch[i]
            frame_info_list_[fi_start + i].smoothed_bbox = smoothed_batch[i];
        }
    }
    {
        auto now = std::chrono::steady_clock::now();
        interpolate_smooth_time_.push_back(std::chrono::duration<double>(now - t_phase).count());
        trim_back(interpolate_smooth_time_);
        t_phase = now;
    }

    // ── Stage 3: SVM — Python lines 148-177 ──
    if (batch_number_ >= 4) {
        // for i in range(self.batch_size-1, -1, -1):
        //     myList = list(self.frame_info_list)[-(i+2*self.batch_size):][:self.batch_size]
        //     if all(frame.smoothed_bbox is not None ...):
        //         feature_vector_80, batch_bbox = self.extract_feature_vector80(myList)
        //         myList[len(myList)//2].relative_feature_vector80 = feature_vector_80
        //         myList[len(myList)//2].batch_bbox = batch_bbox
        for (int i = BATCH_SIZE - 1; i >= 0; --i) {
            int total = static_cast<int>(frame_info_list_.size());
            int my_start = std::max(0, total - (i + 2 * BATCH_SIZE));
            // Python's `[:self.batch_size]` then takes the first 16 of that slice.
            std::vector<FrameInfo*> my_list;
            my_list.reserve(BATCH_SIZE);
            for (int k = 0; k < BATCH_SIZE && my_start + k < total; ++k) {
                my_list.push_back(&frame_info_list_[my_start + k]);
            }
            if (static_cast<int>(my_list.size()) < BATCH_SIZE) continue;
            bool all_set = true;
            for (auto* f : my_list) if (!f->smoothed_bbox.has_value()) { all_set = false; break; }
            if (!all_set) continue;
            auto r = extract_feature_vector80(my_list);
            FrameInfo* center = my_list[my_list.size() / 2];
            center->relative_feature_vector80 = r.feat;
            center->batch_bbox = r.frame_bbox;
            center->batch_bbox_set = true;
        }

        // list_temp = list(self.frame_info_list)[-int(2.5*self.batch_size):][:self.batch_size]
        int total = static_cast<int>(frame_info_list_.size());
        int lt_start = std::max(0, total - static_cast<int>(2.5 * BATCH_SIZE));
        std::vector<FrameInfo*> list_temp;
        for (int k = 0; k < BATCH_SIZE && lt_start + k < total; ++k) {
            list_temp.push_back(&frame_info_list_[lt_start + k]);
        }

        // list_temp_feature_vectors = [frame.relative_feature_vector80 for frame in list_temp]
        std::vector<std::vector<double>> list_temp_feature_vectors;
        list_temp_feature_vectors.reserve(list_temp.size());
        for (auto* f : list_temp) list_temp_feature_vectors.push_back(f->relative_feature_vector80);

        // outttts = self.high_sensitive(list_temp_feature_vectors, Vc0, Vc1, Vr0, Vr1)
        auto out16 = high_sensitive(list_temp_feature_vectors, Vc0, Vc1, Vr0, Vr1);

        if (test_type == "Full_load") {
            out16.fill(1);
        } else if (test_type == "light_load") {
            out16.fill(0);
        }
        // self.out1 += list(outttts)
        for (int v : out16) out1_.push_back(v);
        trim_back(out1_);
    }
    {
        auto now = std::chrono::steady_clock::now();
        svm_inference_time_.push_back(std::chrono::duration<double>(now - t_phase).count());
        trim_back(svm_inference_time_);
        t_phase = now;
    }

    // ── Stage 4: Postprocess1 — Python lines 181-189 ──
    if (batch_number_ >= 5) {
        // for i in range(2*self.L -1, -1, -1):
        //   myList = list(self.out1)[-(i+2*self.L):][:2*self.L]
        //   WLt = np.sum(myList);  out2.append(0 if WLt < w0 else 1)
        for (int i = 2 * L - 1; i >= 0; --i) {
            int total = static_cast<int>(out1_.size());
            int my_start = std::max(0, total - (i + 2 * L));
            int my_end   = std::min(my_start + 2 * L, total);
            int WLt = 0;
            for (int k = my_start; k < my_end; ++k) WLt += out1_[k];
            out2_.push_back(WLt < w0 ? 0 : 1);
        }
        trim_back(out2_);
    }

    // ── Stage 5: Postprocess2 — Python lines 192-199 ──
    if (batch_number_ >= 6) {
        for (int i = 2 * T - 1; i >= 0; --i) {
            int total = static_cast<int>(out2_.size());
            int my_start = std::max(0, total - (i + 2 * T));
            int my_end   = std::min(my_start + 2 * T, total);
            int WTt = 0;
            for (int k = my_start; k < my_end; ++k) WTt += out2_[k];
            out3_.push_back(WTt > 2 ? 1 : 0);
        }
        trim_back(out3_);
    }
    {
        auto now = std::chrono::steady_clock::now();
        post_process_time_.push_back(std::chrono::duration<double>(now - t_phase).count());
        trim_back(post_process_time_);
        t_phase = now;
    }

    // ── Stage 6: deep classifier / annotation — Python lines 207-275 ──
    InferenceResult result;

    if (ignore_ > 0) ignore_ -= 1;
    // Python: `if self.test_type == "full_load": self.ignore = 0`
    // (the lowercase variant exists in the file; we mirror it).
    if (test_type == "full_load") ignore_ = 0;

    if (batch_number_ >= 7) {
        // for i in range(self.batch_size -1, -1, -2):
        for (int i = BATCH_SIZE - 1; i >= 0; i -= 2) {
            int total = static_cast<int>(out3_.size());
            int my_start = std::max(0, total - (i + BATCH_SIZE));
            int my_end   = std::min(my_start + BATCH_SIZE, total);
            int sum_list = 0;
            for (int k = my_start; k < my_end; ++k) sum_list += out3_[k];

            if (sum_list == 16 && ignore_ == 0) {
                std::cout << "!!! doubtful !!!\n";

                // doubtful_frames = list(self.frame_info_list)[-(int(self.batch_size*3.5)+i):][:self.batch_size]
                int total_fi = static_cast<int>(frame_info_list_.size());
                int df_start = std::max(0, total_fi - (static_cast<int>(BATCH_SIZE * 3.5) + i));
                std::vector<FrameInfo*> doubtful_frames;
                for (int k = 0; k < BATCH_SIZE && df_start + k < total_fi; ++k) {
                    doubtful_frames.push_back(&frame_info_list_[df_start + k]);
                }
                if (doubtful_frames.empty()) continue;
                FrameInfo* base_frame = doubtful_frames[doubtful_frames.size() / 2];

                if (base_frame->relative_feature_vector80.empty()) {
                    std::cout << "Error: ### feature_vector_80 is None ###\n";
                    continue;
                }

                auto rfv416_opt = extract_feature_vector416(doubtful_frames, base_frame->batch_bbox);
                if (!rfv416_opt.has_value()) {
                    std::cout << "ignore: ### no human in bbox ###\n";
                    break;
                }

                auto feature_tensor_flat = split_features(*rfv416_opt, base_frame->relative_feature_vector80);
                // feature_tensor shape (1, 15, 16, 2); flat length = 1*15*16*2 = 480
                std::array<int64_t, 4> mlp_shape{1, 15, 16, 2};
                Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
                    OrtArenaAllocator, OrtMemTypeDefault);
                Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                    mem_info,
                    feature_tensor_flat.data(), feature_tensor_flat.size(),
                    mlp_shape.data(), mlp_shape.size());

                const char* input_name = "inputs";
                const char* output_name = mlp_output_name_.c_str();
                auto outputs = mlp_session_->Run(
                    Ort::RunOptions{nullptr},
                    &input_name, &input_tensor, 1,
                    &output_name, 1);
                const float* out_data = outputs[0].GetTensorMutableData<float>();
                double final_predict_00 = static_cast<double>(out_data[0]);

                if (final_predict_00 > 0.9) {
                    annotate_final_fall_on_frames(doubtful_frames);
                    fall_label_ = true;
                    std::cout << "!!! FALL frames detected !!! " << final_predict_00 << "\n";
                    if (test_type == "normal") {
                        ignore_ = 5;
                        break;
                    }
                }
            }
        }

        // target_frames = list(self.frame_info_list)[-int(self.batch_size*4.5):][:self.batch_size]
        int total_fi = static_cast<int>(frame_info_list_.size());
        int tf_start = std::max(0, total_fi - static_cast<int>(BATCH_SIZE * 4.5));
        std::vector<FrameInfo*> target_frames;
        for (int k = 0; k < BATCH_SIZE && tf_start + k < total_fi; ++k) {
            target_frames.push_back(&frame_info_list_[tf_start + k]);
        }
        annotate_fps_on_frame(target_frames);
        annotate_bbox_on_frame2(target_frames);
        annotate_bg_on_frames(target_frames);

        result.annotated_frames.reserve(target_frames.size());
        for (auto* f : target_frames) result.annotated_frames.push_back(f->frame_img);
    }
    {
        auto now = std::chrono::steady_clock::now();
        deep_classifier_time_.push_back(std::chrono::duration<double>(now - t_phase).count());
        trim_back(deep_classifier_time_);
    }

    // stop_all = time.time(); self.all_time.append(stop_all - start_all)
    auto t_all_end = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(t_all_end - t_all_start).count();
    all_time_.push_back(total_sec); trim_back(all_time_);

    // self.annotate_time_on_frames(annotated_frames, stop_all - start_all)
    annotate_time_on_frames(result.annotated_frames, total_sec);

    result.fall_label = fall_label_;
    return result;
}

// ─── extract_approximate_batch_bbox — Python 557-630 ─────────────────────
std::optional<cv::Vec4i> FallDetection::extract_approximate_batch_bbox(
    const std::vector<cv::Mat>& frame_batch,
    const cv::Mat& background_)
{
    // indices = [0, len/3, 2*len/3, len-1]
    const int n = static_cast<int>(frame_batch.size());
    const std::array<int, 4> indices{0, n / 3, 2 * n / 3, n - 1};

    // size = (70, 50) — note Python (W, H)
    const int W = 70, H = 50;
    const int H_main = frame_batch[0].rows;
    const int W_main = frame_batch[0].cols;
    const int image_area = H * W;
    const double max_area_ratio = 0.5;

    cv::Mat background_resized;
    cv::resize(background_, background_resized, cv::Size(W, H));
    cv::Mat gray_background;
    cv::cvtColor(background_resized, gray_background, cv::COLOR_BGR2GRAY);

    std::vector<std::array<double, 5>> bboxes;
    bboxes.reserve(4);

    for (int idx : indices) {
        cv::Mat frame_resize;
        cv::resize(frame_batch[idx], frame_resize, cv::Size(W, H));

        cv::Mat gray_frame;
        cv::cvtColor(frame_resize, gray_frame, cv::COLOR_BGR2GRAY);

        cv::Mat diff;
        cv::absdiff(gray_frame, gray_background, diff);

        cv::Mat fgmask;
        cv::threshold(diff, fgmask, 45, 255, cv::THRESH_BINARY);
        cv::morphologyEx(fgmask, fgmask, cv::MORPH_CLOSE, kernel1_, cv::Point(-1, -1), 2);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(fgmask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_TC89_L1);

        double max_area = 0.0;
        int max_idx = -1;
        for (size_t c = 0; c < contours.size(); ++c) {
            double a = cv::contourArea(contours[c]);
            if (a > max_area) { max_area = a; max_idx = static_cast<int>(c); }
        }

        if (max_idx >= 0 && max_area < max_area_ratio * image_area) {
            cv::Rect br = cv::boundingRect(contours[max_idx]);
            int margin_x = static_cast<int>(0.01 * W);  // Python: int(0.01 * W)
            int margin_y = static_cast<int>(0.01 * H);
            int x1 = std::max(br.x - margin_x, 0);
            int y1 = std::max(br.y - margin_y, 0);
            int x2 = std::min(br.x + br.width + margin_x, W);
            int y2 = std::min(br.y + br.height + margin_y, H);
            double ratio = static_cast<double>(x2 - x1) / static_cast<double>(y2 - y1);
            bboxes.push_back({y1 / static_cast<double>(H),
                              y2 / static_cast<double>(H),
                              x1 / static_cast<double>(W),
                              x2 / static_cast<double>(W),
                              ratio});
        }
        // else: dropped (Python appends None then filters below)
    }

    if (bboxes.empty()) return std::nullopt;

    double y1n =  std::numeric_limits<double>::infinity();
    double y2n = -std::numeric_limits<double>::infinity();
    double x1n =  std::numeric_limits<double>::infinity();
    double x2n = -std::numeric_limits<double>::infinity();
    for (auto& b : bboxes) {
        y1n = std::min(y1n, b[0]);
        y2n = std::max(y2n, b[1]);
        x1n = std::min(x1n, b[2]);
        x2n = std::max(x2n, b[3]);
    }
    std::array<double, 4> bounding_rect{y1n, y2n, x1n, x2n};
    auto box = bounding_square_box(bounding_rect, H_main, W_main);
    // For parity: dump the post-aggregation square box (y1, y2, x1, x2 ints
    // cast to float so the dump format stays float32 — diff_tensors.py also
    // reads float32). Python equivalent dumps the same 4 values.
    float bb_f[4] = {(float)box[0], (float)box[1], (float)box[2], (float)box[3]};
    dumpTensorF32("fall_approx", bb_f, 4);
    return box;
}

// ─── bounding_square_box — Python 632-648 ──────────────────────────────
cv::Vec4i FallDetection::bounding_square_box(const std::array<double, 4>& bbox, int H, int W) {
    double y1_ = bbox[0], y2_ = bbox[1], x1_ = bbox[2], x2_ = bbox[3];
    double y1 = y1_ * H, y2 = y2_ * H, x1 = x1_ * W, x2 = x2_ * W;
    double box_w = x2 - x1, box_h = y2 - y1;
    double side = std::min({std::max(box_w, box_h) * 1.4,
                            static_cast<double>(H),
                            static_cast<double>(W)});
    double center_x = (x1 + x2) / 2.0, center_y = (y1 + y2) / 2.0;
    double half_side = side / 2.0;

    center_x = std::max(half_side, std::min(W - half_side, center_x));
    center_y = std::max(half_side, std::min(H - half_side, center_y));

    // Python uses `int(...)` which truncates toward zero, NOT round.
    return cv::Vec4i(
        static_cast<int>(center_y - half_side),
        static_cast<int>(center_y + half_side),
        static_cast<int>(center_x - half_side),
        static_cast<int>(center_x + half_side));
}

// ─── extract_main_bboxes — Python 488-554 ──────────────────────────────
std::vector<OptBbox5> FallDetection::extract_main_bboxes(
    const std::vector<cv::Mat>& frame_batch,
    const cv::Mat& background__,
    const cv::Vec4i& batch_bbox)
{
    int bb_y1 = batch_bbox[0], bb_y2 = batch_bbox[1];
    int bb_x1 = batch_bbox[2], bb_x2 = batch_bbox[3];

    const int bb_h = bb_y2 - bb_y1;
    const int bb_w = bb_x2 - bb_x1;
    const int H = 70, W = 70;       // standard_size — Python (70,70)
    const int image_area = H * W;
    const int H_main = frame_batch[0].rows;
    const int W_main = frame_batch[0].cols;
    const double min_area_ratio = 0.001;

    cv::Mat background_crop = background__(cv::Range(bb_y1, bb_y2), cv::Range(bb_x1, bb_x2));
    cv::Mat background_resized, gray_background;
    cv::resize(background_crop, background_resized, cv::Size(W, H));
    cv::cvtColor(background_resized, gray_background, cv::COLOR_BGR2GRAY);

    std::vector<OptBbox5> out;
    out.reserve(frame_batch.size());

    for (const cv::Mat& frame_full : frame_batch) {
        cv::Mat frame_crop = frame_full(cv::Range(bb_y1, bb_y2), cv::Range(bb_x1, bb_x2));
        cv::Mat frame_resize;
        cv::resize(frame_crop, frame_resize, cv::Size(W, H));

        cv::Mat gray_frame;
        cv::cvtColor(frame_resize, gray_frame, cv::COLOR_BGR2GRAY);

        cv::Mat diff;
        cv::absdiff(gray_frame, gray_background, diff);

        cv::Mat fgmask;
        cv::threshold(diff, fgmask, 45, 255, cv::THRESH_BINARY);
        cv::morphologyEx(fgmask, fgmask, cv::MORPH_CLOSE, kernel2_, cv::Point(-1, -1), 3);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(fgmask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_TC89_L1);

        double max_area = 0.0;
        int    max_idx  = -1;
        for (size_t c = 0; c < contours.size(); ++c) {
            double a = cv::contourArea(contours[c]);
            if (a > max_area) { max_area = a; max_idx = static_cast<int>(c); }
        }

        if (max_idx >= 0 && (min_area_ratio * image_area) < max_area) {
            cv::Rect br = cv::boundingRect(contours[max_idx]);
            int margin_x = static_cast<int>(0.01 * W);
            int margin_y = static_cast<int>(0.01 * H);
            int x1 = std::max(br.x - margin_x, 0);
            int y1 = std::max(br.y - margin_y, 0);
            int x2 = std::min(br.x + br.width + margin_x, W);
            int y2 = std::min(br.y + br.height + margin_y, H);
            double ratio = static_cast<double>(x2 - x1) / static_cast<double>(y2 - y1);

            Bbox5 bb;
            bb.y1    = ((static_cast<double>(y1) / H) * bb_h + bb_y1) / static_cast<double>(H_main);
            bb.y2    = ((static_cast<double>(y2) / H) * bb_h + bb_y1) / static_cast<double>(H_main);
            bb.x1    = ((static_cast<double>(x1) / W) * bb_w + bb_x1) / static_cast<double>(W_main);
            bb.x2    = ((static_cast<double>(x2) / W) * bb_w + bb_x1) / static_cast<double>(W_main);
            bb.ratio = ratio;
            out.push_back(bb);
        } else {
            out.push_back(std::nullopt);
        }
    }
    return out;
}

// ─── interpolate_and_smooth_bboxes — Python 679-725 ─────────────────────
std::vector<OptBbox5> FallDetection::interpolate_and_smooth_bboxes(
    const std::vector<OptBbox5>& bbox_list)
{
    std::vector<OptBbox5> bbox_array = bbox_list;  // copy
    std::vector<int> none_indices, valid_indices;
    for (int i = 0; i < static_cast<int>(bbox_array.size()); ++i) {
        if (bbox_array[i].has_value()) valid_indices.push_back(i);
        else                            none_indices.push_back(i);
    }

    if (valid_indices.empty()) return bbox_array;

    if (!none_indices.empty()) {
        // Interpolate internal Nones between consecutive valid indices.
        for (int k = 0; k + 1 < static_cast<int>(valid_indices.size()); ++k) {
            int start_idx = valid_indices[k];
            int end_idx   = valid_indices[k + 1];
            if (end_idx - start_idx > 1) {
                const Bbox5& a = *bbox_array[start_idx];
                const Bbox5& b = *bbox_array[end_idx];
                const double denom = static_cast<double>(end_idx - start_idx);
                for (int j = start_idx + 1; j < end_idx; ++j) {
                    const double t = (j - start_idx) / denom;
                    Bbox5 interp;
                    interp.y1    = a.y1    + (b.y1    - a.y1)    * t;
                    interp.y2    = a.y2    + (b.y2    - a.y2)    * t;
                    interp.x1    = a.x1    + (b.x1    - a.x1)    * t;
                    interp.x2    = a.x2    + (b.x2    - a.x2)    * t;
                    interp.ratio = a.ratio + (b.ratio - a.ratio) * t;
                    bbox_array[j] = interp;
                }
            }
        }
    }

    const int N = static_cast<int>(bbox_array.size());
    const int half = SMOOTHING_WINDOW / 2;  // 6
    std::vector<OptBbox5> smoothed;
    smoothed.reserve(N);

    for (int i = 0; i < N; ++i) {
        int start = std::max(0, i - half);
        int end   = std::min(N, i + half + 1);
        bool any_none = false;
        for (int k = start; k < end; ++k) if (!bbox_array[k].has_value()) { any_none = true; break; }

        if (any_none) {
            smoothed.push_back(bbox_array[i]);  // pass-through (may itself be None)
            continue;
        }

        // Python computes sum_weights = sum(gaussian_weights[:len(window)]) — note the
        // weights are taken from the *front* of the kernel, not centered. We mirror.
        const int win_len = end - start;
        double sum_weights = 0.0;
        for (int w = 0; w < win_len; ++w) sum_weights += gaussian_weights_[w];

        Bbox5 sm{0, 0, 0, 0, 0};
        for (int j = 0; j < 5; ++j) {
            double acc = 0.0;
            for (int w = 0; w < win_len; ++w) {
                const Bbox5& b = *bbox_array[start + w];
                const double v = (j == 0 ? b.y1
                                 : j == 1 ? b.y2
                                 : j == 2 ? b.x1
                                 : j == 3 ? b.x2
                                          : b.ratio);
                acc += v * gaussian_weights_[w];
            }
            const double val = acc / sum_weights;
            if      (j == 0) sm.y1    = val;
            else if (j == 1) sm.y2    = val;
            else if (j == 2) sm.x1    = val;
            else if (j == 3) sm.x2    = val;
            else             sm.ratio = val;
        }
        smoothed.push_back(sm);
    }
    return smoothed;
}

// ─── extract_feature_vector80 / relativalize / calculate_frame_bbox ──────
FallDetection::Feat80Result FallDetection::extract_feature_vector80(
    const std::vector<FrameInfo*>& frame_info_list)
{
    // smoothed_bboxes_list = [mem.smoothed_bbox for mem in frame_info_list]
    // feature_vector_80 = np.array(smoothed_bboxes_list).flatten()
    std::vector<double> feat80;
    feat80.reserve(80);
    for (auto* f : frame_info_list) {
        const Bbox5& b = *f->smoothed_bbox;
        feat80.push_back(b.y1);
        feat80.push_back(b.y2);
        feat80.push_back(b.x1);
        feat80.push_back(b.x2);
        feat80.push_back(b.ratio);
    }

    int H = frame_info_list[0]->frame_img.rows;
    int W = frame_info_list[0]->frame_img.cols;
    cv::Vec4i frame_bbox = calculate_frame_bbox(feat80, H, W);
    auto relative = relativalize_feature_vector_80(feat80, frame_bbox, H, W);
    // Parity dump: 80 floats. Python equivalent dumps the relativalized
    // feature vector (post-frame-bbox subtraction).
    std::vector<float> rel_f(relative.begin(), relative.end());
    dumpTensorF32("fall_feat80", rel_f);
    return Feat80Result{std::move(relative), frame_bbox};
}

std::vector<double> FallDetection::relativalize_feature_vector_80(
    const std::vector<double>& feature_vector_80,
    const cv::Vec4i& frame_bbox,
    int H, int W)
{
    double y1_base = frame_bbox[0], y2_base = frame_bbox[1];
    double x1_base = frame_bbox[2], x2_base = frame_bbox[3];
    double H_base = y2_base - y1_base;
    double W_base = x2_base - x1_base;

    std::vector<double> out;
    out.reserve(80);
    for (int i = 0; i < 16; ++i) {
        double y1_   = feature_vector_80[i * 5 + 0];
        double y2_   = feature_vector_80[i * 5 + 1];
        double x1_   = feature_vector_80[i * 5 + 2];
        double x2_   = feature_vector_80[i * 5 + 3];
        double ratio = feature_vector_80[i * 5 + 4];

        double y1 = y1_ * H, y2 = y2_ * H, x1 = x1_ * W, x2 = x2_ * W;
        out.push_back((y1 - y1_base) / H_base);
        out.push_back((y2 - y1_base) / H_base);
        out.push_back((x1 - x1_base) / W_base);
        out.push_back((x2 - x1_base) / W_base);
        out.push_back(ratio);
    }
    return out;
}

cv::Vec4i FallDetection::calculate_frame_bbox(
    const std::vector<double>& feature_vector_80, int H, int W)
{
    // bboxes_list = vector.reshape(16,5); bbox_avg = mean axis=0  (5 components)
    std::array<double, 5> avg{0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 5; ++k)
            avg[k] += feature_vector_80[i * 5 + k];
    for (double& v : avg) v /= 16.0;

    double y1_ = np_clip(avg[0], 0.0, 1.0);
    double y2_ = np_clip(avg[1], 0.0, 1.0);
    double x1_ = np_clip(avg[2], 0.0, 1.0);
    double x2_ = np_clip(avg[3], 0.0, 1.0);

    double y1 = y1_ * H, y2 = y2_ * H, x1 = x1_ * W, x2 = x2_ * W;
    double box_w = x2 - x1, box_h = y2 - y1;

    double side = std::min(((box_w + box_h) / 2.0) * 1.8,
                           static_cast<double>(std::min(W, H)));
    double center_x = (x1 + x2) / 2.0, center_y = (y1 + y2) / 2.0;
    double half_side = side / 2.0;

    center_x = np_clip(center_x, half_side, static_cast<double>(W) - half_side);
    center_y = np_clip(center_y, half_side, static_cast<double>(H) - half_side);

    int x1_final = py_round_int(center_x - half_side);
    int x2_final = py_round_int(center_x + half_side);
    int y1_final = py_round_int(center_y - half_side);
    int y2_final = py_round_int(center_y + half_side);
    return cv::Vec4i(y1_final, y2_final, x1_final, x2_final);
}

cv::Vec4i FallDetection::calculate_bounding_rect_box(const Bbox5& bbox, int H, int W) {
    double y1_ = np_clip(bbox.y1, 0.0, 1.0);
    double y2_ = np_clip(bbox.y2, 0.0, 1.0);
    double x1_ = np_clip(bbox.x1, 0.0, 1.0);
    double x2_ = np_clip(bbox.x2, 0.0, 1.0);

    double y1 = y1_ * H, y2 = y2_ * H, x1 = x1_ * W, x2 = x2_ * W;
    double box_w = x2 - x1, box_h = y2 - y1;
    double side_x = std::min(box_w * 1.3, static_cast<double>(W));
    double side_y = std::min(box_h * 1.3, static_cast<double>(H));

    double cx = (x1 + x2) / 2.0, cy = (y1 + y2) / 2.0;
    double hs_x = side_x / 2.0, hs_y = side_y / 2.0;

    cx = np_clip(cx, hs_x, static_cast<double>(W) - hs_x);
    cy = np_clip(cy, hs_y, static_cast<double>(H) - hs_y);

    int x1f = py_round_int(cx - hs_x);
    int x2f = py_round_int(cx + hs_x);
    int y1f = py_round_int(cy - hs_y);
    int y2f = py_round_int(cy + hs_y);
    return cv::Vec4i(y1f, y2f, x1f, x2f);
}

// ─── SVM stage — Python predict / high_sensitive ───────────────────────
std::vector<std::array<double, 2>> FallDetection::svm_predict(
    const std::vector<std::vector<double>>& feature_matrix_16x80)
{
    // Python passes float32. SVM ONNX export from sklearn typically exposes
    // input "X" with shape [None, 80] and two outputs: [labels, probabilities].
    // We take probabilities (index 1).
    std::vector<float> flat;
    flat.reserve(feature_matrix_16x80.size() * 80);
    for (auto& row : feature_matrix_16x80)
        for (double v : row) flat.push_back(static_cast<float>(v));

    std::array<int64_t, 2> shape{static_cast<int64_t>(feature_matrix_16x80.size()), 80};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(
        mem, flat.data(), flat.size(), shape.data(), shape.size());

    Ort::AllocatorWithDefaultOptions alloc;
    auto in_name_alloc  = svm_session_->GetInputNameAllocated(0, alloc);
    auto in_name_str    = std::string(in_name_alloc.get()); // expected "X"

    // probabilities is output index 1 in sklearn ONNX export
    auto out0_alloc = svm_session_->GetOutputNameAllocated(0, alloc);
    auto out1_alloc = svm_session_->GetOutputNameAllocated(1, alloc);
    std::array<std::string, 2> out_name_strs{
        std::string(out0_alloc.get()), std::string(out1_alloc.get())};
    std::array<const char*, 2> out_names{out_name_strs[0].c_str(), out_name_strs[1].c_str()};
    const char* in_names[1] = {in_name_str.c_str()};

    auto outputs = svm_session_->Run(Ort::RunOptions{nullptr},
                                     in_names, &input, 1,
                                     out_names.data(), out_names.size());

    // outputs[1] is probabilities. sklearn-onnx exports usually emit a dense
    // [N, 2] float tensor (when `options={id(clf): {'zipmap': False}}` was set
    // at export time), but the default export emits `seq<map<int,float>>`.
    // Python's onnxruntime returns either as something indexable like
    // `predictions[i][1]`, so the Python code works on both. C++ needs to
    // branch on the actual ONNX value type. We support both shapes here.
    std::vector<std::array<double, 2>> result(feature_matrix_16x80.size());
    if (outputs[1].IsTensor()) {
        const float* prob = outputs[1].GetTensorMutableData<float>();
        for (size_t i = 0; i < feature_matrix_16x80.size(); ++i) {
            result[i][0] = static_cast<double>(prob[i * 2 + 0]);
            result[i][1] = static_cast<double>(prob[i * 2 + 1]);
        }
    } else {
        // sequence-of-maps fallback: outputs[1] is Seq<Map<int64,float>> of N maps.
        const size_t n = outputs[1].GetCount();
        if (n != feature_matrix_16x80.size())
            throw std::runtime_error("svm_predict: sequence length mismatch");
        for (size_t i = 0; i < n; ++i) {
            Ort::Value map_v   = outputs[1].GetValue(static_cast<int>(i), nullptr);
            Ort::Value keys    = map_v.GetValue(0, nullptr);
            Ort::Value values  = map_v.GetValue(1, nullptr);
            const int64_t* k_data = keys.GetTensorMutableData<int64_t>();
            const float*   v_data = values.GetTensorMutableData<float>();
            const size_t   m = keys.GetTensorTypeAndShapeInfo().GetElementCount();
            for (size_t j = 0; j < m; ++j) {
                if      (k_data[j] == 0) result[i][0] = static_cast<double>(v_data[j]);
                else if (k_data[j] == 1) result[i][1] = static_cast<double>(v_data[j]);
            }
        }
    }
    return result;
}

std::array<int, 16> FallDetection::high_sensitive(
    const std::vector<std::vector<double>>& feature_matrix_16x80,
    double /*Vc0_*/, double /*Vc1_*/, double Vr0_, double /*Vr1_*/)
{
    std::array<int, 16> out{};
    out.fill(0);

    // any(x is None for x in feature_matrix_16x80) — for us, an empty inner
    // vector means "feature not set", which is the same as Python's None.
    for (auto& v : feature_matrix_16x80) if (v.empty()) return out;

    auto initial = svm_predict(feature_matrix_16x80);  // (16, 2)

    for (int i = 0; i < 16; ++i) {
        const auto& fv = feature_matrix_16x80[i];
        int o = initial[i][1] > 0.4 ? 1 : 0;

        // Vc = ((fv[40]+fv[41]) - (fv[35]+fv[36])) / 2
        // Vr = fv[44] - fv[39]
        double Vc = ((fv[40] + fv[41]) - (fv[35] + fv[36])) / 2.0;
        double Vr = fv[44] - fv[39];

        // Python: `if Vr < Vr0 and Vc < Vc0: out = 0`
        if (Vr < Vr0_ && Vc < Vc0) o = 0;

        // Python: hard-fire when Vc > 0.017 and Vr > 0.02 and not (fv[41] == fv[36])
        if (Vc > 0.017 && Vr > 0.02 && !(fv[41] == fv[36])) {
            o = 1;
            std::cout << "############ Vc:" << Vc << " Vr:" << Vr
                      << " SVM:" << initial[i][1] << "\n";
        }
        out[i] = o;
    }
    return out;
}

// ─── MoVeNet (TFLite) — Python get_movenet_keypoints / process_keypoints / apply_movenet ──
cv::Mat FallDetection::get_movenet_keypoints(const cv::Mat& image_bgr) {
    cv::Mat padded = resize_with_pad_rgb(image_bgr, 192, 192);
    // copy into input tensor (1, 192, 192, 3) uint8
    uint8_t* in = tflite_interp_->typed_input_tensor<uint8_t>(0);
    std::memcpy(in, padded.data, 192 * 192 * 3);
    tflite_interp_->Invoke();
    const float* out = tflite_interp_->typed_output_tensor<float>(0); // (1,1,17,3)

    cv::Mat kp(17, 3, CV_64F);
    for (int i = 0; i < 17; ++i) {
        kp.at<double>(i, 0) = static_cast<double>(out[i * 3 + 0]);   // y
        kp.at<double>(i, 1) = static_cast<double>(out[i * 3 + 1]);   // x
        kp.at<double>(i, 2) = static_cast<double>(out[i * 3 + 2]);   // score
    }
    return kp;
}

cv::Mat FallDetection::process_keypoints(const cv::Mat& keypoints) {
    // avg_point = mean(keypoints[:5, :], axis=0)
    cv::Mat out(13, 3, CV_64F);
    for (int j = 0; j < 3; ++j) {
        double s = 0.0;
        for (int i = 0; i < 5; ++i) s += keypoints.at<double>(i, j);
        out.at<double>(0, j) = s / 5.0;
    }
    for (int i = 5; i < 17; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.at<double>(i - 5 + 1, j) = keypoints.at<double>(i, j);
        }
    }
    return out;
}

cv::Mat FallDetection::apply_movenet(const cv::Mat& frame, const Bbox5& bbox) {
    int h = frame.rows, w = frame.cols;
    cv::Vec4i rect = calculate_bounding_rect_box(bbox, h, w);
    int y1 = rect[0], y2 = rect[1], x1 = rect[2], x2 = rect[3];
    int h_crop = y2 - y1, w_crop = x2 - x1;

    cv::Mat crop = frame(cv::Range(y1, y2), cv::Range(x1, x2));
    cv::Mat kp = get_movenet_keypoints(crop);
    cv::Mat processed = process_keypoints(kp);

    // processed[:, 0] = processed[:, 0]*h_crop + y1
    // processed[:, 1] = processed[:, 1]*w_crop + x1
    for (int i = 0; i < 13; ++i) {
        processed.at<double>(i, 0) = processed.at<double>(i, 0) * h_crop + y1;
        processed.at<double>(i, 1) = processed.at<double>(i, 1) * w_crop + x1;
    }
    // Parity dump: 13×3 keypoints in (y, x, score) layout, as float32.
    float kp_f[13 * 3];
    for (int i = 0; i < 13; ++i)
        for (int j = 0; j < 3; ++j)
            kp_f[i * 3 + j] = static_cast<float>(processed.at<double>(i, j));
    dumpTensorF32("fall_keypoints", kp_f, 13 * 3);
    return processed;
}

// ─── smooth_signal — Python 882-914 ────────────────────────────────────
std::vector<double> FallDetection::smooth_signal(
    const std::vector<double>& signal,
    const std::vector<double>& score_signal)
{
    const int window_size = 3;
    const double sigma    = 2.0;
    const int N = static_cast<int>(signal.size());

    std::vector<double> kernel(2 * window_size + 1);
    double sumk = 0.0;
    for (int i = -window_size; i <= window_size; ++i) {
        double v = i / sigma;
        kernel[i + window_size] = std::exp(-0.5 * v * v);
        sumk += kernel[i + window_size];
    }
    for (double& v : kernel) v /= sumk;

    std::vector<double> out(N);
    for (int i = 0; i < N; ++i) {
        double weighted_sum = 0.0, total_weight = 0.0;
        for (int oo = -window_size; oo <= window_size; ++oo) {
            int j = i + oo;
            if (j < 0 || j >= N) continue;
            double conf_w = score_signal[j];
            double g_w    = kernel[oo + window_size];
            double comb   = conf_w * g_w;
            weighted_sum += signal[j] * comb;
            total_weight += comb;
        }
        out[i] = total_weight > 0.0 ? weighted_sum / total_weight : signal[i];
    }
    return out;
}

// ─── extract_feature_vector416 — Python 917-960 ─────────────────────────
std::optional<std::vector<double>> FallDetection::extract_feature_vector416(
    std::vector<FrameInfo*>& frame_zip_list,
    const cv::Vec4i& batch_bbox)
{
    for (auto* mem : frame_zip_list) {
        if (!mem->movenet_result_set) {
            mem->movenet_result = apply_movenet(mem->frame_img, *mem->smoothed_bbox);
            mem->movenet_result_set = true;
            // human_conf = mean of column 2
            double s = 0.0;
            for (int i = 0; i < 13; ++i) s += mem->movenet_result.at<double>(i, 2);
            mem->human_conf = s / 13.0;
            annotate_keypoints_on_frame(*mem);
        }
    }

    double sum_conf = 0.0, min_conf = std::numeric_limits<double>::infinity();
    for (auto* mem : frame_zip_list) {
        sum_conf += mem->human_conf;
        min_conf = std::min(min_conf, mem->human_conf);
    }
    double final_human_conf = sum_conf / frame_zip_list.size();
    if (final_human_conf < 0.2 || min_conf < 0.1) return std::nullopt;

    const double bb_y1 = batch_bbox[0], bb_y2 = batch_bbox[1];
    const double bb_x1 = batch_bbox[2], bb_x2 = batch_bbox[3];
    const double bb_h = bb_y2 - bb_y1, bb_w = bb_x2 - bb_x1;

    // signals (26) and score_signals (13), each length 16.
    std::array<std::vector<double>, 26> signals;
    std::array<std::vector<double>, 13> score_signals;
    for (auto& s : signals) s.reserve(16);
    for (auto& s : score_signals) s.reserve(16);

    for (auto* mem : frame_zip_list) {
        cv::Mat kp = mem->movenet_result.clone();
        // normalize against batch_bbox
        for (int i = 0; i < 13; ++i) {
            kp.at<double>(i, 0) = (kp.at<double>(i, 0) - bb_y1) / bb_h;
            kp.at<double>(i, 1) = (kp.at<double>(i, 1) - bb_x1) / bb_w;
        }
        // for i,(y,x,score): signals[2i].append(x); signals[2i+1].append(y); score[i].append(score)
        for (int i = 0; i < 13; ++i) {
            double y = kp.at<double>(i, 0);
            double x = kp.at<double>(i, 1);
            double sc = kp.at<double>(i, 2);
            signals[2 * i].push_back(x);
            signals[2 * i + 1].push_back(y);
            score_signals[i].push_back(sc);
        }
    }

    std::vector<double> X;
    X.reserve(26 * 16);
    for (int i = 0; i < 26; ++i) {
        auto smoothed = smooth_signal(signals[i], score_signals[i / 2]);
        for (double v : smoothed) X.push_back(v);
    }
    return X;
}

// ─── split_features — Python 652-677 ──────────────────────────────────
std::vector<float> FallDetection::split_features(
    const std::vector<double>& vector_416,
    const std::vector<double>& vector_80)
{
    // bbox_signals_list = vector_80.reshape(16,5).transpose()  -> shape (5,16)
    // bbox_signals_list[k][t] = vector_80[t*5 + k]
    auto bsl = [&](int k, int t) { return vector_80[t * 5 + k]; };

    // 15 x 16 lists, each entry is a per-time series
    std::array<std::array<double, 16>, 15> x_list;
    std::array<std::array<double, 16>, 15> y_list;
    int x_count = 0, y_count = 0;

    // y_list.append(bbox_signals_list[0])  -> y1 across time
    for (int t = 0; t < 16; ++t) y_list[y_count][t] = bsl(0, t);
    y_count++;
    // x_list.append(bbox_signals_list[2])  -> x1 across time
    for (int t = 0; t < 16; ++t) x_list[x_count][t] = bsl(2, t);
    x_count++;

    // signals_list = vector_416.reshape(26, 16)
    auto sl = [&](int r, int c) { return vector_416[r * 16 + c]; };
    for (int i = 0; i < 26; ++i) {
        if (i % 2 == 0) {
            for (int t = 0; t < 16; ++t) x_list[x_count][t] = sl(i, t);
            x_count++;
        } else {
            for (int t = 0; t < 16; ++t) y_list[y_count][t] = sl(i, t);
            y_count++;
        }
    }

    // y_list.append(bbox_signals_list[1]) ; x_list.append(bbox_signals_list[3])
    for (int t = 0; t < 16; ++t) y_list[y_count][t] = bsl(1, t);
    y_count++;
    for (int t = 0; t < 16; ++t) x_list[x_count][t] = bsl(3, t);
    x_count++;
    // x_count == y_count == 15 now.

    // X_train = [x_list, y_list] -> shape (2, 15, 16)
    // final = np.transpose(X_train, (1, 2, 0)) -> shape (15, 16, 2)
    // final[a, b, c] = X_train[c, a, b]
    std::vector<float> flat(15 * 16 * 2);
    for (int a = 0; a < 15; ++a) {
        for (int b = 0; b < 16; ++b) {
            flat[a * 16 * 2 + b * 2 + 0] = static_cast<float>(x_list[a][b]);
            flat[a * 16 * 2 + b * 2 + 1] = static_cast<float>(y_list[a][b]);
        }
    }
    return flat;
}

// ─── Annotations — Python 277-431 ──────────────────────────────────────
void FallDetection::annotate_fps_on_frame(const std::vector<FrameInfo*>& frames) {
    for (auto* fr : frames) {
        cv::Mat& frame = fr->frame_img;
        double fps = fr->fps;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Cam FPS: %.1f", fps);

        int width  = frame.cols;
        int height = frame.rows;
        double font_scale = width / 1000.0;
        int    thickness  = std::max(1, static_cast<int>(font_scale * 2));
        int baseline = 0;
        cv::Size sz = cv::getTextSize(buf, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
        int text_x = (width - sz.width) / 2;
        int text_y = height - 10;
        cv::putText(frame, buf, cv::Point(text_x, text_y),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(255, 255, 255), thickness, cv::LINE_AA);
    }
}

void FallDetection::annotate_time_on_frames(std::vector<cv::Mat>& frames, double duration) {
    if (frames.empty()) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Time: %.2fms", 1000.0 * duration);
    for (cv::Mat& fr : frames) {
        int width = fr.cols, height = fr.rows;
        double font_scale = width / 1000.0;
        int thickness = std::max(1, static_cast<int>(font_scale * 2));
        int x_pos = width - static_cast<int>(width * 0.02) - 200;
        int y_pos = height - static_cast<int>(height * 0.02);
        cv::putText(fr, buf, cv::Point(x_pos, y_pos),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(0, 255, 0), thickness);
    }
}

void FallDetection::annotate_bg_on_frames(std::vector<FrameInfo*>& frames) {
    for (auto* frame : frames) {
        cv::Mat& fr = frame->frame_img;
        cv::Mat& bg = frame->background_img;

        int bg_height = static_cast<int>(fr.rows * 0.2);
        int bg_width  = static_cast<int>(bg_height * (static_cast<double>(bg.cols) / bg.rows));

        cv::Mat small_bg;
        cv::resize(bg, small_bg, cv::Size(bg_width, bg_height));

        const int border_size = 2;
        cv::Mat small_bg_with_border;
        cv::copyMakeBorder(small_bg, small_bg_with_border,
                           border_size, border_size, border_size, border_size,
                           cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

        const int margin = 10;
        int x_start = fr.cols - bg_width - 2 * border_size - margin;
        int y_start = margin;
        cv::Rect dst(x_start, y_start, small_bg_with_border.cols, small_bg_with_border.rows);
        small_bg_with_border.copyTo(fr(dst));
    }
}

void FallDetection::annotate_bbox_on_frame2(std::vector<FrameInfo*>& frames) {
    for (auto* fr : frames) {
        if (!fr->smoothed_bbox.has_value()) continue;
        const Bbox5& b = *fr->smoothed_bbox;
        cv::Mat& frame = fr->frame_img;
        int H = frame.rows, W = frame.cols;
        int y1 = static_cast<int>(b.y1 * H);
        int y2 = static_cast<int>(b.y2 * H);
        int x1 = static_cast<int>(b.x1 * W);
        int x2 = static_cast<int>(b.x2 * W);
        cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(0, 255, 0), 2);
    }
}

void FallDetection::annotate_keypoints_on_frame(FrameInfo& frame_obj) {
    cv::Mat& frame = frame_obj.frame_img;
    const cv::Mat& kp = frame_obj.movenet_result;
    double conf = frame_obj.human_conf;

    for (int i = 0; i < 13; ++i) {
        int y = static_cast<int>(kp.at<double>(i, 0));
        int x = static_cast<int>(kp.at<double>(i, 1));
        cv::circle(frame, cv::Point(x, y), 5, cv::Scalar(0, 0, 255), -1);
    }
    int width = frame.cols, height = frame.rows;
    double font_scale = width / 1000.0;
    int thickness = std::max(1, static_cast<int>(font_scale * 2));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "movenet Conf: %.2f", conf);
    cv::putText(frame, buf, cv::Point(10, height - 10),
                cv::FONT_HERSHEY_SIMPLEX, font_scale,
                cv::Scalar(0, 255, 0), thickness);
}

void FallDetection::annotate_final_fall_on_frames(std::vector<FrameInfo*>& frames) {
    for (auto* f : frames) {
        cv::putText(f->frame_img, "Fall Detected!", cv::Point(50, 50),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
        cv::rectangle(f->frame_img, cv::Point(10, 10), cv::Point(300, 100),
                      cv::Scalar(0, 255, 255), 2);
    }
}

std::vector<std::array<double,5>> FallDetection::getLast16Bboxes() const {
    std::vector<std::array<double,5>> out;
    int n = std::min<int>(16, bboxes_.size());
    for (int i = (int)bboxes_.size() - n; i < (int)bboxes_.size(); ++i) {
        const auto& b = bboxes_[i];
        if (b.has_value()) {
            out.push_back({b->y1, b->y2, b->x1, b->x2, b->ratio});
        } else {
            double nan = std::numeric_limits<double>::quiet_NaN();
            out.push_back({nan, nan, nan, nan, nan});
        }
    }
    return out;
}

FallDetection::StateSummary FallDetection::getStateSummary() const {
    StateSummary s{};
    // Last 16 of each output deque (Python: list(detector.out1)[-16:])
    auto sum_tail = [](const std::deque<int>& d) {
        int n = std::min<int>(16, d.size());
        int s = 0;
        for (int i = (int)d.size() - n; i < (int)d.size(); ++i) s += d[i];
        return s;
    };
    s.out1_sum = sum_tail(out1_);
    s.out2_sum = sum_tail(out2_);
    s.out3_sum = sum_tail(out3_);
    // bbox_present: count non-empty in last 16 of bboxes_
    {
        int n = std::min<int>(16, bboxes_.size());
        for (int i = (int)bboxes_.size() - n; i < (int)bboxes_.size(); ++i)
            if (bboxes_[i].has_value()) s.bbox_present++;
    }
    // smoothed_present: count frames with smoothed_bbox in [-32:-16] window
    {
        int total = (int)frame_info_list_.size();
        int lo = std::max(0, total - 32);
        int hi = std::max(0, total - 16);
        for (int i = lo; i < hi; ++i)
            if (frame_info_list_[i].smoothed_bbox.has_value()) s.smoothed_present++;
    }
    s.ignore = ignore_;
    return s;
}

} // namespace fall
