// Configuration: a small YAML-subset reader (maps, flow maps/lists that may span lines, block lists of
// scalars, comments, quoted strings) plus the typed Config struct used by the pipeline.
// The key names are the same as in the Python framework's default.yaml.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "classic_stage.h"
#include "deep_stage.h"
#include "detector.h"
#include "representation.h"
#include "track_fusion.h"

namespace fd {

class YamlNode {
public:
    enum class Type { Null, Scalar, Map, List };
    Type type = Type::Null;
    std::string scalar;
    std::vector<std::pair<std::string, YamlNode>> map;
    std::vector<YamlNode> list;

    const YamlNode* find(const std::string& key) const;
    // Dotted path lookup, e.g. at("bbox.fusion.weight_yolo"); throws with the full path if missing.
    const YamlNode& at(const std::string& dotted) const;
    bool has(const std::string& dotted) const;
    double as_double() const;
    int as_int() const;
    bool as_bool() const;
    const std::string& as_string() const { return scalar; }
};

YamlNode parse_yaml(const std::string& text);
YamlNode load_yaml_file(const std::string& path);

struct YoloConfig {
    bool enabled = true;
    std::string param, bin;
    std::string input_name = "in0", output_name = "out0";
    int input_size = 320;
    int every_n_frames = 8;
    double conf = 0.2, iou_nms = 0.5;
    int full_frame_every_n_runs = 8;
};

struct BBoxConfig {
    YoloConfig yolo;
    int bg_grid = 16;
    double bg_update_margin = 0.02;
    int coarse_width = 80, coarse_threshold = 45, coarse_kernel = 7, coarse_close_iterations = 2, coarse_diff_step = 4;
    double coarse_min_area_ratio = 0.0005, coarse_max_area_ratio = 0.5;
    int fine_size = 70, fine_threshold = 40, fine_temporal_threshold = 35, fine_kernel = 5, fine_close_iterations = 3;
    double fine_min_area_ratio = 0.0005, fine_pad_ratio = 0.01;
    double roi_margin_with_yolo = 1.4, roi_margin_without_yolo = 2.0, roi_min_side_frac = 0.33;
    FusionConfig fusion;
    double multi_conf = 0.4;
    int multi_confirm_runs = 3;
    int multi_suspend_batches = 3;
};

struct PoseConfig {
    std::string model;
    int input_w = 192, input_h = 256;
    double bbox_padding = 1.25, simcc_split_ratio = 2.0;
    bool rgb_input = true;
};

struct Config {
    std::string mode;                                  // "camera" | "infer"
    std::string models_dir;
    int yolo_threads = 4, pose_threads = 4, classifier_threads = 1;
    double target_fps = 16.0, fallback_fps = 25.0;
    int batch_size = 16;
    int max_side = 960;                                // 0 = no downscaling
    bool strict_signature = true;
    FeatureSpec spec;
    BBoxConfig bbox;
    PoseConfig pose;
    ClassicConfig classic;
    DeepConfig deep;
    DetectorConfig detector;
    std::string svm_model, cnn_model;
    // camera mode
    std::string camera_source = "/dev/video9";
    int camera_width = 640, camera_height = 480;
    bool trust_camera_rate = true;
    int max_backlog_batches = 4;
    bool log_timing = false;
    // infer mode
    std::vector<std::string> videos;
    std::string video_list_file, output_dir = "results_cpp";
};

Config load_config(const std::string& path);

}  // namespace fd
