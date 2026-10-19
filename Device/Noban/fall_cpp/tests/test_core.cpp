// Golden tests of the library-free core against the Python framework.
//   test_core <golden.txt> <python_default.yaml> <shipped config.yaml>
// golden.txt holds inputs and outputs recorded from the Python implementation (make_golden.py).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "classic_stage.h"
#include "config.h"
#include "deep_stage.h"
#include "detector.h"
#include "json_util.h"
#include "representation.h"
#include "resampler.h"
#include "result_writer.h"
#include "track_fusion.h"
#include "vision_utils.h"

using namespace fd;

namespace {

int g_checks = 0, g_failed = 0;
std::string g_json_out;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::cout << "  FAIL: " << what << "\n"; }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

struct Golden {
    std::map<std::string, std::vector<double>> num;
    std::map<std::string, std::string> str;
    const std::vector<double>& get(const std::string& tag) const {
        auto it = num.find(tag);
        if (it == num.end()) throw std::runtime_error("golden tag missing: " + tag);
        return it->second;
    }
};

Golden load_golden(const std::string& path) {
    Golden g;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag.size() > 6 && tag.substr(tag.size() - 6) == ".value") { std::string v; ss >> v; g.str[tag] = v; continue; }
        size_t n;
        ss >> n;
        std::vector<double> values(n);
        for (size_t i = 0; i < n; ++i) ss >> values[i];
        g.num[tag] = values;
    }
    return g;
}

void compare_vec(const std::vector<double>& got, const std::vector<double>& want, double tol, const std::string& what) {
    if (got.size() != want.size()) { check(false, what + ": size " + std::to_string(got.size()) + " vs " + std::to_string(want.size())); return; }
    double worst = 0;
    size_t where = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const double d = std::fabs(got[i] - want[i]) / std::max(1.0, std::fabs(want[i]));
        if (d > worst) { worst = d; where = i; }
    }
    check(worst <= tol, what + ": max relative diff " + std::to_string(worst) + " at " + std::to_string(where));
}

std::vector<double> to_d(const std::vector<float>& v) { return std::vector<double>(v.begin(), v.end()); }

// ---- settings taken from the Python final config (tests/golden/python_default.yaml)
struct Settings {
    FeatureSpec spec;
    ClassicConfig classic;
    DeepConfig deep;
    DetectorConfig detector;
    FusionConfig fusion;
    double yolo_conf, yolo_iou;
};

Settings read_settings(const std::string& path) {
    const YamlNode y = load_yaml_file(path);
    Settings s;
    s.spec.point_order = y.at("representation.point_order").as_string();
    s.spec.channels = y.at("representation.channels").as_int();
    s.spec.batch_bbox_margin = y.at("representation.batch_bbox_margin").as_double();
    s.spec.smooth_half_window = y.at("representation.pose_smoothing.half_window").as_int();
    s.spec.smooth_sigma = y.at("representation.pose_smoothing.sigma").as_double();
    s.classic.svm_threshold = y.at("classic.svm_threshold").as_double();
    s.classic.use_model_threshold = y.at("classic.use_model_threshold").as_bool();
    s.classic.suppress_vr_below = y.at("classic.high_sensitive.suppress_vr_below").as_double();
    s.classic.suppress_vc_below = y.at("classic.high_sensitive.suppress_vc_below").as_double();
    s.classic.force_vc_above = y.at("classic.high_sensitive.force_vc_above").as_double();
    s.classic.force_vr_above = y.at("classic.high_sensitive.force_vr_above").as_double();
    s.classic.post1_min_count = y.at("classic.post1.min_count").as_int();
    s.classic.post2_min_count = y.at("classic.post2.min_count").as_int();
    s.deep.cnn_threshold = y.at("deep.cnn_threshold").as_double();
    s.deep.use_model_threshold = y.at("deep.use_model_threshold").as_bool();
    s.deep.window_stride = y.at("deep.window_stride").as_int();
    s.deep.cooldown_batches = y.at("deep.cooldown_batches").as_int();
    s.deep.min_mean_confidence = y.at("deep.min_mean_confidence").as_double();
    s.deep.min_frame_confidence = y.at("deep.min_frame_confidence").as_double();
    s.detector.window_stride = s.deep.window_stride;
    s.detector.cooldown_batches = s.deep.cooldown_batches;
    s.detector.multi_person_suspend_batches = y.at("bbox.multi_person.suspend_batches").as_int();
    s.fusion.weight_yolo = y.at("bbox.fusion.weight_yolo").as_double();
    s.fusion.weight_bg = y.at("bbox.fusion.weight_bg").as_double();
    s.fusion.weight_interp = y.at("bbox.fusion.weight_interp").as_double();
    s.fusion.bg_score = y.at("bbox.fusion.bg_score").as_double();
    s.fusion.max_gap_frames = y.at("bbox.fusion.max_gap_frames").as_int();
    s.fusion.half_window = y.at("bbox.smoothing.half_window").as_int();
    s.fusion.sigma = y.at("bbox.smoothing.sigma").as_double();
    s.fusion.every_n_frames = y.at("bbox.yolo.every_n_frames").as_int();
    s.yolo_conf = y.at("bbox.yolo.conf").as_double();
    s.yolo_iou = y.at("bbox.yolo.iou_nms").as_double();
    return s;
}

// ---- stubs (the same functions as in make_golden.py)
struct SigmoidSvm : ISvm {
    std::vector<double> predict_proba(const std::vector<float>& f, int n) override {
        std::vector<double> p(n);
        for (int i = 0; i < n; ++i) p[i] = 1.0 / (1.0 + std::exp(-10.0 * (static_cast<double>(f[i * 80 + 40]) - 0.5)));
        return p;
    }
    std::optional<double> threshold() const override { return std::nullopt; }
};
struct OneSvm : ISvm {
    std::vector<double> predict_proba(const std::vector<float>&, int n) override { return std::vector<double>(n, 1.0); }
    std::optional<double> threshold() const override { return std::nullopt; }
};
struct AlwaysCnn : ICnn {
    std::vector<double> predict_proba(const std::vector<float>&, int n) override { return std::vector<double>(n, 0.99); }
    std::optional<double> threshold() const override { return std::nullopt; }
};
struct SlopeCnn : ICnn {
    int channels = 3, T = 16;
    std::vector<double> predict_proba(const std::vector<float>& x, int n) override {
        const size_t row = static_cast<size_t>(T) * 15 * channels;
        std::vector<double> p(n);
        for (int i = 0; i < n; ++i) {
            const float last = x[i * row + (static_cast<size_t>(T - 1) * 15 + 0) * channels + 1];
            const float first = x[i * row + (0 * 15 + 0) * channels + 1];
            p[i] = last > first ? 0.99 : 0.1;
        }
        return p;
    }
    std::optional<double> threshold() const override { return std::nullopt; }
};
struct LinPose : IPose {
    std::vector<PoseResult> estimate(const std::vector<ImagePtr>&, const std::vector<Box>& boxes) override {
        std::vector<PoseResult> out;
        for (const Box& b : boxes) {
            PoseResult r;
            const double sx = (b.x2 - b.x1) / 16.0, sy = (b.y2 - b.y1) / 16.0;      // numpy.linspace(start, stop, 17)
            for (int i = 0; i < 17; ++i) {
                r.kp[i][0] = i == 16 ? b.x2 : b.x1 + i * sx;
                r.kp[i][1] = i == 16 ? b.y2 : b.y1 + i * sy;
                r.score[i] = 0.8;
            }
            out.push_back(r);
        }
        return out;
    }
};
struct ScriptedBBox : IBBoxModule {
    std::vector<Box> boxes;
    std::vector<int> missing;
    int n = 0, next = 0, lookahead = 18;
    std::vector<FrameBox> process_batch(const std::vector<Frame>& frames) override {
        n = frames.back().index + 1;
        std::vector<FrameBox> out;
        while (next <= n - 1 - lookahead) {
            FrameBox fb;
            fb.index = next;
            const bool miss = next < static_cast<int>(missing.size()) && missing[next];
            if (!miss) fb.box = boxes[std::min<size_t>(next, boxes.size() - 1)];
            fb.source = "yolo";
            fb.score = 0.9;
            out.push_back(fb);
            ++next;
        }
        return out;
    }
    int lookahead_frames() const override { return lookahead; }
    bool multi_person_confirmed() const override { return false; }
};

std::vector<Box> boxes_from(const std::vector<double>& v) {
    std::vector<Box> out;
    for (size_t i = 0; i + 3 < v.size(); i += 4) out.push_back({v[i], v[i + 1], v[i + 2], v[i + 3]});
    return out;
}

// ------------------------------------------------------------------ sections
void test_signatures(const Golden& g) {
    std::cout << "signatures\n";
    for (int i = 0;; ++i) {
        const std::string tag = "sig" + std::to_string(i);
        if (!g.num.count(tag + ".spec")) break;
        const auto& v = g.get(tag + ".spec");
        FeatureSpec s;
        s.point_order = v[0] == 0 ? "legacy" : "chain";
        s.channels = static_cast<int>(v[1]);
        s.batch_bbox_margin = v[2];
        s.smooth_half_window = static_cast<int>(v[3]);
        s.smooth_sigma = v[4];
        check(s.signature() == g.str.at(tag + ".value"), tag + ": " + s.signature() + " vs " + g.str.at(tag + ".value"));
    }
}

void test_representation(const Golden& g) {
    std::cout << "representation (features80, CNN tensor, confidences)\n";
    for (int i = 0; g.num.count("sig" + std::to_string(i) + ".spec"); ++i) {
        const auto& sv = g.get("sig" + std::to_string(i) + ".spec");
        FeatureSpec spec;
        spec.point_order = sv[0] == 0 ? "legacy" : "chain";
        spec.channels = static_cast<int>(sv[1]);
        spec.batch_bbox_margin = sv[2];
        spec.smooth_half_window = static_cast<int>(sv[3]);
        spec.smooth_sigma = sv[4];
        const std::string tag = "rep" + std::to_string(i);
        const std::vector<Box> boxes = boxes_from(g.get(tag + ".boxes"));
        const auto& kp = g.get(tag + ".kp");
        const auto& sc = g.get(tag + ".sc");
        std::vector<PoseResult> poses(16);
        for (int t = 0; t < 16; ++t)
            for (int j = 0; j < 17; ++j) {
                poses[t].kp[j][0] = kp[(t * 17 + j) * 2];
                poses[t].kp[j][1] = kp[(t * 17 + j) * 2 + 1];
                poses[t].score[j] = sc[t * 17 + j];
            }
        const WindowFeatures w = build_window(boxes, poses, spec);
        compare_vec(to_d(w.features80), g.get(tag + ".f80"), 2e-6, tag + " features80");
        compare_vec(to_d(w.cnn_tensor), g.get(tag + ".tensor"), 2e-6, tag + " tensor (" + spec.point_order + ", C=" + std::to_string(spec.channels) + ")");
        compare_vec({w.mean_confidence, w.min_frame_confidence}, g.get(tag + ".conf"), 1e-9, tag + " confidences");
    }
}

void test_classic(const Golden& g, const Settings& s) {
    std::cout << "classic stage (SVM -> high sensitivity -> post-1 -> post-2)\n";
    const std::vector<Box> boxes = boxes_from(g.get("classic.boxes"));
    const auto& missing = g.get("classic.missing");
    SigmoidSvm svm;
    ClassicStage stage(s.classic, s.spec, svm);
    const int N = static_cast<int>(boxes.size());
    std::vector<double> prob(N, -1), s1(N, -1), s2(N, -1), s3(N, -1);
    for (int c = 0; c < N; c += 16) {
        std::vector<FrameBox> fbs;
        for (int i = c; i < c + 16; ++i) {
            FrameBox fb;
            fb.index = i;
            if (!missing[i]) fb.box = boxes[i];
            fb.source = "yolo";
            fb.score = 0.9;
            fbs.push_back(fb);
        }
        const ClassicUpdate u = stage.advance(fbs);
        for (auto& kv : u.svm_prob) prob[kv.first] = kv.second;
        for (auto& kv : u.s1) s1[kv.first] = kv.second;
        for (auto& kv : u.s2) s2[kv.first] = kv.second;
        for (auto& kv : u.s3) s3[kv.first] = kv.second;
    }
    compare_vec(prob, g.get("classic.svm_prob"), 1e-6, "svm_prob");
    compare_vec(s1, g.get("classic.s1"), 0, "s1");
    compare_vec(s2, g.get("classic.s2"), 0, "s2");
    compare_vec(s3, g.get("classic.s3"), 0, "s3");
    int pos = 0;
    for (double v : s3) pos += v == 1;
    check(pos > 0 && pos < N, "scenario exercises both s3 values");
}

void run_detector_scenario(const Golden& g, const Settings& s, const std::string& name, ISvm& svm, ICnn& cnn,
                           const std::string& json_out = "") {
    std::cout << "detector scenario '" << name << "'\n";
    const std::string p = "det." + name;
    ScriptedBBox bbox;
    bbox.boxes = boxes_from(g.get(p + ".boxes"));
    for (double m : g.get(p + ".missing")) bbox.missing.push_back(static_cast<int>(m));
    LinPose pose;
    ClassicStage classic(s.classic, s.spec, svm);
    DeepStage deep(s.deep, s.spec, pose, cnn);
    FallDetector det(s.detector, bbox, classic, deep, true);
    const int n_batches = static_cast<int>(g.get(p + ".n_batches")[0]);
    std::vector<BatchResult> results;
    for (int b = 0; b < n_batches; ++b) {
        std::vector<Frame> frames(16);
        for (int i = 0; i < 16; ++i) {
            frames[i].index = 16 * b + i;
            frames[i].time_s = frames[i].index / 16.0;
            frames[i].src_index = frames[i].index;
            frames[i].width = 64; frames[i].height = 48;
        }
        results.push_back(det.push_batch(frames));
    }
    for (const BatchResult& r : det.flush()) results.push_back(r);

    const auto& la = g.get(p + ".lookahead");
    check(det.lookahead_frames() == la[0] && det.latency_batches() == la[1], "lookahead / latency batches");
    std::vector<double> aligned;
    for (const BatchResult& r : results) { aligned.push_back(r.valid); aligned.push_back(r.target_batch.value_or(-1)); aligned.push_back(r.fall_label); }
    compare_vec(aligned, g.get(p + ".aligned"), 0, "aligned fall_label sequence");

    std::vector<double> events;
    for (const FallEvent& e : det.events())
        for (double v : {double(e.event_id), double(e.window_start), double(e.window_end), double(e.event_frame), e.probability,
                         double(e.decided_at_frame), double(e.latency_frames), double(e.involves_padding)})
            events.push_back(v);
    if (events.empty()) events.push_back(0);
    compare_vec(events, g.get(p + ".events"), 1e-9, "events");
    check(!det.events().empty(), "scenario produces at least one event");

    std::vector<double> windows;
    for (const DeepWindowResult& w : det.window_log()) {
        const int status = w.status == "ok" ? 0 : (w.status == "low_pose_confidence" ? 1 : 2);
        for (double v : {double(w.start), double(w.end), double(w.positive), double(status), w.probability.value_or(-1.0)}) windows.push_back(v);
    }
    if (windows.empty()) windows.push_back(0);
    compare_vec(windows, g.get(p + ".windows"), 1e-9, "deep window log");

    std::vector<double> s3, cnn_prob;
    for (int i = 0; i < 16 * n_batches; ++i) {
        auto it = det.history().find(i);
        s3.push_back(it == det.history().end() || !it->second.s3 ? -1 : *it->second.s3);
        cnn_prob.push_back(it == det.history().end() || !it->second.cnn_prob ? -1 : *it->second.cnn_prob);
    }
    compare_vec(s3, g.get(p + ".s3"), 0, "per-frame s3");
    compare_vec(cnn_prob, g.get(p + ".cnn_prob"), 1e-9, "per-frame cnn probability");

    if (!json_out.empty()) {                                  // exercised by tools/compare_with_python.py
        VideoRun run;
        run.video_id = "scenario_motion.avi";
        run.path = "/other/machine/scenario/Videos/scenario_motion.avi";
        run.n_frames = 16 * n_batches;
        for (int i = 0; i < run.n_frames; ++i) run.src_index.push_back(i);
        run.batches = results;
        write_result_json(json_out, run, det);
    }
}

void test_detector(const Golden& g, const Settings& s) {
    OneSvm one;
    AlwaysCnn always;
    run_detector_scenario(g, s, "steady", one, always);
    SigmoidSvm sig;
    SlopeCnn slope;
    slope.channels = s.spec.channels;
    run_detector_scenario(g, s, "motion", sig, slope, g_json_out);
}

void test_fusion(const Golden& g, const Settings& s) {
    std::cout << "track fusion + smoothing\n";
    const auto& flat = g.get("fusion.obs");
    TrackFusion fz(s.fusion);
    check(fz.lookahead() == g.get("fusion.lookahead")[0], "fusion look-ahead");
    std::map<int, Obs> all;
    for (size_t i = 0; i + 6 < flat.size(); i += 7) {
        Obs o;
        o.box = {flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4]};
        o.source = flat[i + 5] == 1 ? "yolo" : "bg";
        o.score = flat[i + 6];
        all[static_cast<int>(flat[i])] = o;
    }
    for (auto& kv : all) if (kv.first < 60) fz.obs[kv.first] = kv.second;
    fz.n_received = 60;
    std::vector<FrameBox> out = fz.finalize(60 - 1 - fz.lookahead());
    for (auto& kv : all) if (kv.first >= 60) fz.obs[kv.first] = kv.second;
    fz.n_received = 100;
    for (const FrameBox& fb : fz.finalize(99)) out.push_back(fb);
    std::vector<double> got;
    for (const FrameBox& f : out) {
        got.push_back(f.index);
        got.push_back(f.box ? 1 : 0);
        for (double v : f.box ? std::vector<double>{f.box->x1, f.box->y1, f.box->x2, f.box->y2} : std::vector<double>(4, 0.0)) got.push_back(v);
        got.push_back(f.source == "yolo" ? 1 : f.source == "bg" ? 2 : f.source == "interp" ? 3 : 0);
        got.push_back(f.score);
    }
    compare_vec(got, g.get("fusion.out"), 1e-9, "final boxes");
    int none = 0, interp = 0;
    for (const FrameBox& f : out) { none += !f.box; interp += f.source == "interp"; }
    check(none > 0 && interp > 0, "scenario has missing frames and interpolated frames");
}

void test_vision(const Golden& g, const Settings& s) {
    std::cout << "YOLO decode, letterbox and SimCC decode\n";
    for (int ci = 0; g.num.count("yolo" + std::to_string(ci) + ".meta"); ++ci) {
        const std::string p = "yolo" + std::to_string(ci);
        const auto& meta = g.get(p + ".meta");
        const int w = static_cast<int>(meta[0]), h = static_cast<int>(meta[1]);
        const Letterbox lb = letterbox_params(w, h, 320);
        check(near(lb.scale, meta[2], 1e-12) && lb.pad_x == meta[3] && lb.pad_y == meta[4], p + " letterbox geometry");
        std::vector<float> pred(84 * 2100, 0.0f);
        const auto& cand = g.get(p + ".cand");
        for (size_t i = 0; i + 5 < cand.size(); i += 6) {
            const int a = static_cast<int>(cand[i]);
            for (int c = 0; c < 5; ++c) pred[c * 2100 + a] = static_cast<float>(cand[i + 1 + c]);
        }
        const auto dets = decode_yolo(pred.data(), 84, 2100, 0, static_cast<float>(s.yolo_conf), static_cast<float>(s.yolo_iou), lb, w, h);
        std::vector<double> got;
        for (const Detection& d : dets) for (double v : {d.box.x1, d.box.y1, d.box.x2, d.box.y2, d.score}) got.push_back(v);
        if (got.empty()) got.push_back(0);
        compare_vec(got, g.get(p + ".out"), 1e-6, p + " detections");
    }
    for (int ci = 0; g.num.count("simcc" + std::to_string(ci) + ".box"); ++ci) {
        const std::string p = "simcc" + std::to_string(ci);
        const auto& b = g.get(p + ".box");
        const CropGeometry crop = pose_crop_geometry({b[0], b[1], b[2], b[3]}, 192.0 / 256.0, 1.25);
        std::vector<float> sx, sy;
        for (double v : g.get(p + ".x")) sx.push_back(static_cast<float>(v));
        for (double v : g.get(p + ".y")) sy.push_back(static_cast<float>(v));
        const PoseResult r = decode_simcc(sx.data(), sy.data(), 384, 512, 2.0, 192, 256, crop);
        std::vector<double> got;
        for (int k = 0; k < 17; ++k) { got.push_back(r.kp[k][0]); got.push_back(r.kp[k][1]); }
        for (int k = 0; k < 17; ++k) got.push_back(r.score[k]);
        compare_vec(got, g.get(p + ".out"), 1e-9, p + " keypoints and scores");
    }
}

void test_resampler(const Golden& g) {
    std::cout << "16 FPS resampler\n";
    for (int ci = 0; g.num.count("resample" + std::to_string(ci) + ".meta"); ++ci) {
        const std::string p = "resample" + std::to_string(ci);
        const auto& meta = g.get(p + ".meta");
        const double fps = meta[0];
        const int n = static_cast<int>(meta[1]);
        Resampler r(16.0, 1.0 / fps);
        std::vector<double> got;
        for (int j = 0; j < n; ++j)
            for (const Selection& s : r.push(j, j / fps)) { got.push_back(s.k); got.push_back(s.src_index); }
        for (const Selection& s : r.finish()) { got.push_back(s.k); got.push_back(s.src_index); }
        compare_vec(got, g.get(p + ".out"), 0, p + " (" + std::to_string(fps) + " fps)");
    }
}

void test_config(const std::string& python_yaml, const std::string& shipped) {
    std::cout << "config files\n";
    const YamlNode y = load_yaml_file(python_yaml);
    check(y.at("bbox.coarse.max_area_ratio").as_double() == 0.5, "multi-line flow map parsed (coarse)");
    check(y.at("pose.input_size").list.size() == 2 && y.at("pose.input_size").list[1].as_int() == 256, "flow list");
    check(y.at("dataset.include_groups").list.at(0).as_string() == "*", "quoted scalar in list with trailing comment");
    check(y.at("deep.cnn_threshold").as_double() == 0.94 && !y.at("deep.use_model_threshold").as_bool(), "scalars and booleans");
    check(y.at("input.max_side").as_int() == 960, "int");
    const Config c = load_config(shipped);
    check(c.mode == "camera" || c.mode == "infer", "shipped config mode");
    check(c.batch_size == 16 && c.bbox.yolo.every_n_frames == 8 && c.deep.window_stride == 1 && c.deep.cooldown_batches == 6, "values copied from the Python config");
    check(c.spec.channels == 3 && c.spec.point_order == "legacy", "representation");
    check(c.yolo_threads >= 1 && c.pose_threads >= 1 && c.classifier_threads >= 1, "thread settings present");
    const Settings s = read_settings(python_yaml);
    check(s.spec.signature() == c.spec.signature(), "shipped config has the same feature signature as the Python config");
    check(std::fabs(c.bbox.fusion.weight_bg - s.fusion.weight_bg) < 1e-12 && c.classic.post1_min_count == s.classic.post1_min_count &&
              c.deep.cnn_threshold == s.deep.cnn_threshold && c.bbox.yolo.conf == s.yolo_conf, "shipped config matches the Python config");
}

void test_json_helpers() {
    std::cout << "meta json helpers\n";
    const std::string meta = "{\n \"feature_signature\": \"d5e0695d308fdf55\",\n \"threshold\": 0.37,\n \"classes\": [0, 1],\n"
                             " \"val_metrics\": {\"threshold\": 0.99, \"tp\": 3}\n}";
    check(json_string(meta, "feature_signature") == std::optional<std::string>("d5e0695d308fdf55"), "string");
    check(json_number(meta, "threshold") == std::optional<double>(0.37), "first (top-level) threshold wins");
    check(json_int_list(meta, "classes") == std::optional<std::vector<int>>(std::vector<int>{0, 1}), "int list");
    check(!json_number(meta, "missing"), "missing key");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::cerr << "usage: test_core golden.txt python_default.yaml config.yaml\n"; return 2; }
    try {
        const Golden g = load_golden(argv[1]);
        const Settings s = read_settings(argv[2]);
        if (argc > 4) g_json_out = argv[4];
        test_signatures(g);
        test_representation(g);
        test_classic(g, s);
        test_detector(g, s);
        test_fusion(g, s);
        test_vision(g, s);
        test_resampler(g);
        test_config(argv[2], argv[3]);
        test_json_helpers();
    } catch (const std::exception& e) {
        std::cout << "ERROR: " << e.what() << "\n";
        return 1;
    }
    std::cout << (g_failed ? "FAILED" : "OK") << ": " << g_checks - g_failed << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
