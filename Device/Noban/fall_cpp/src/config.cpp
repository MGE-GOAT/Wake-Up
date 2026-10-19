#include "config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fd {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Remove a trailing "# comment" that is not inside quotes.
std::string strip_comment(const std::string& line) {
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquote(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size() + 0 && (s[i + 1] == '\\' || s[i + 1] == '"')) ++i;
            out.push_back(s[i]);
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') return s.substr(1, s.size() - 2);
    return s;
}

// Index of the ':' that separates a key from its value ("key: value" or "key:"), or npos.
size_t key_colon(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == ':' && (i + 1 == s.size() || std::isspace(static_cast<unsigned char>(s[i + 1])))) return i;
    }
    return std::string::npos;
}

std::vector<std::string> split_top_level(const std::string& s) {   // split on commas outside brackets/quotes
    std::vector<std::string> parts;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (char c : s) {
        if (quote) { cur.push_back(c); if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '{' || c == '[') ++depth;
        if (c == '}' || c == ']') --depth;
        if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    if (!trim(cur).empty()) parts.push_back(cur);
    return parts;
}

YamlNode parse_value(const std::string& raw);

YamlNode parse_flow(const std::string& text) {
    const std::string s = trim(text);
    YamlNode node;
    const std::string inner = s.substr(1, s.size() - 2);
    if (s.front() == '{') {
        node.type = YamlNode::Type::Map;
        for (const std::string& part : split_top_level(inner)) {
            const size_t colon = key_colon(part);
            if (colon == std::string::npos) throw std::runtime_error("bad flow map entry: " + part);
            node.map.emplace_back(unquote(part.substr(0, colon)), parse_value(part.substr(colon + 1)));
        }
    } else {
        node.type = YamlNode::Type::List;
        for (const std::string& part : split_top_level(inner)) node.list.push_back(parse_value(part));
    }
    return node;
}

YamlNode parse_value(const std::string& raw) {
    const std::string s = trim(raw);
    YamlNode node;
    if (s.empty() || s == "~" || s == "null") return node;
    if (s.front() == '{' || s.front() == '[') return parse_flow(s);
    node.type = YamlNode::Type::Scalar;
    node.scalar = unquote(s);
    return node;
}

struct Line { int indent; std::string text; };

bool brackets_balanced(const std::string& s) {
    int depth = 0;
    char quote = 0;
    for (char c : s) {
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return depth <= 0;
}

YamlNode parse_block(const std::vector<Line>& lines, size_t& pos, int indent) {
    YamlNode node;
    if (pos < lines.size() && lines[pos].text.rfind("- ", 0) == 0 && lines[pos].indent == indent) {
        node.type = YamlNode::Type::List;
        while (pos < lines.size() && lines[pos].indent == indent && lines[pos].text.rfind("- ", 0) == 0)
            node.list.push_back(parse_value(lines[pos++].text.substr(2)));
        return node;
    }
    node.type = YamlNode::Type::Map;
    while (pos < lines.size() && lines[pos].indent == indent) {
        const std::string text = lines[pos].text;
        const size_t colon = key_colon(text);
        if (colon == std::string::npos) throw std::runtime_error("cannot parse config line: " + text);
        const std::string key = unquote(text.substr(0, colon));
        std::string value = trim(text.substr(colon + 1));
        ++pos;
        if (value.empty()) {                               // nested block (map or list) or empty value
            if (pos < lines.size() && lines[pos].indent > indent) {
                node.map.emplace_back(key, parse_block(lines, pos, lines[pos].indent));
            } else if (pos < lines.size() && lines[pos].indent == indent && lines[pos].text.rfind("- ", 0) == 0) {
                node.map.emplace_back(key, parse_block(lines, pos, indent));
            } else {
                node.map.emplace_back(key, YamlNode());
            }
        } else {
            while (!brackets_balanced(value) && pos < lines.size()) value += " " + lines[pos++].text;  // multi-line flow
            node.map.emplace_back(key, parse_value(value));
        }
    }
    return node;
}

}  // namespace

const YamlNode* YamlNode::find(const std::string& key) const {
    for (const auto& kv : map)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

const YamlNode& YamlNode::at(const std::string& dotted) const {
    const YamlNode* node = this;
    size_t start = 0;
    while (true) {
        const size_t dot = dotted.find('.', start);
        const std::string key = dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        node = node->find(key);
        if (!node) throw std::runtime_error("missing config key: " + dotted);
        if (dot == std::string::npos) return *node;
        start = dot + 1;
    }
}

bool YamlNode::has(const std::string& dotted) const {
    try { at(dotted); return true; } catch (const std::exception&) { return false; }
}

double YamlNode::as_double() const {
    char* end = nullptr;
    const double v = std::strtod(scalar.c_str(), &end);
    if (scalar.empty() || *end != '\0') throw std::runtime_error("not a number: '" + scalar + "'");
    return v;
}
int YamlNode::as_int() const { return static_cast<int>(as_double()); }
bool YamlNode::as_bool() const {
    if (scalar == "true" || scalar == "True") return true;
    if (scalar == "false" || scalar == "False") return false;
    throw std::runtime_error("not a boolean: '" + scalar + "'");
}

YamlNode parse_yaml(const std::string& text) {
    std::vector<Line> lines;
    std::istringstream stream(text);
    std::string raw;
    while (std::getline(stream, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string body = strip_comment(raw);
        if (trim(body).empty()) continue;
        int indent = 0;
        while (indent < static_cast<int>(body.size()) && body[indent] == ' ') ++indent;
        lines.push_back({indent, trim(body)});
    }
    size_t pos = 0;
    if (lines.empty()) return YamlNode();
    return parse_block(lines, pos, lines.front().indent);
}

YamlNode load_yaml_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return parse_yaml(buffer.str());
}

Config load_config(const std::string& path) {
    const YamlNode y = load_yaml_file(path);
    Config c;
    c.mode = y.at("mode").as_string();
    if (c.mode != "camera" && c.mode != "infer") throw std::runtime_error("mode must be 'camera' or 'infer'");
    c.models_dir = y.at("paths.models_dir").as_string();

    c.yolo_threads = y.at("runtime.yolo_threads").as_int();
    c.pose_threads = y.at("runtime.pose_threads").as_int();
    c.classifier_threads = y.at("runtime.classifier_threads").as_int();

    c.target_fps = y.at("temporal.target_fps").as_double();
    c.batch_size = y.at("temporal.batch_size").as_int();
    const int window = y.at("temporal.window_size").as_int();
    c.fallback_fps = y.at("temporal.fallback_fps").as_double();
    if (c.batch_size != window) throw std::runtime_error("temporal.batch_size and temporal.window_size must match");
    if (c.target_fps <= 0) throw std::runtime_error("temporal.target_fps must be positive");
    const YamlNode& ms = y.at("input.max_side");
    c.max_side = ms.type == YamlNode::Type::Null ? 0 : ms.as_int();
    c.strict_signature = y.at("models.strict_signature").as_bool();

    c.spec.target_fps = c.target_fps;
    c.spec.window = window;
    c.spec.point_order = y.at("representation.point_order").as_string();
    c.spec.channels = y.at("representation.channels").as_int();
    c.spec.batch_bbox_margin = y.at("representation.batch_bbox_margin").as_double();
    c.spec.smooth_half_window = y.at("representation.pose_smoothing.half_window").as_int();
    c.spec.smooth_sigma = y.at("representation.pose_smoothing.sigma").as_double();
    if (c.spec.point_order != "legacy" && c.spec.point_order != "chain") throw std::runtime_error("representation.point_order must be legacy or chain");
    if (c.spec.channels != 2 && c.spec.channels != 3) throw std::runtime_error("representation.channels must be 2 or 3");

    BBoxConfig& b = c.bbox;
    b.yolo.enabled = y.at("bbox.yolo.enabled").as_bool();
    b.yolo.param = y.at("bbox.yolo.param").as_string();
    b.yolo.bin = y.at("bbox.yolo.bin").as_string();
    b.yolo.input_name = y.at("bbox.yolo.input_name").as_string();
    b.yolo.output_name = y.at("bbox.yolo.output_name").as_string();
    b.yolo.input_size = y.at("bbox.yolo.input_size").as_int();
    b.yolo.every_n_frames = y.at("bbox.yolo.every_n_frames").as_int();
    b.yolo.conf = y.at("bbox.yolo.conf").as_double();
    b.yolo.iou_nms = y.at("bbox.yolo.iou_nms").as_double();
    b.yolo.full_frame_every_n_runs = y.at("bbox.yolo.full_frame_every_n_runs").as_int();
    if (b.yolo.every_n_frames < 1 || c.batch_size % b.yolo.every_n_frames != 0)
        throw std::runtime_error("bbox.yolo.every_n_frames must divide temporal.batch_size");
    b.bg_grid = y.at("bbox.background.grid").as_int();
    b.bg_update_margin = y.at("bbox.background.update_margin").as_double();
    b.coarse_width = y.at("bbox.coarse.width").as_int();
    b.coarse_threshold = y.at("bbox.coarse.threshold").as_int();
    b.coarse_kernel = y.at("bbox.coarse.kernel").as_int();
    b.coarse_close_iterations = y.at("bbox.coarse.close_iterations").as_int();
    b.coarse_min_area_ratio = y.at("bbox.coarse.min_area_ratio").as_double();
    b.coarse_max_area_ratio = y.at("bbox.coarse.max_area_ratio").as_double();
    b.coarse_diff_step = y.at("bbox.coarse.diff_step").as_int();
    b.fine_size = y.at("bbox.fine.size").as_int();
    b.fine_threshold = y.at("bbox.fine.threshold").as_int();
    b.fine_temporal_threshold = y.at("bbox.fine.temporal_threshold").as_int();
    b.fine_kernel = y.at("bbox.fine.kernel").as_int();
    b.fine_close_iterations = y.at("bbox.fine.close_iterations").as_int();
    b.fine_min_area_ratio = y.at("bbox.fine.min_area_ratio").as_double();
    b.fine_pad_ratio = y.at("bbox.fine.pad_ratio").as_double();
    b.roi_margin_with_yolo = y.at("bbox.roi.margin_with_yolo").as_double();
    b.roi_margin_without_yolo = y.at("bbox.roi.margin_without_yolo").as_double();
    b.roi_min_side_frac = y.at("bbox.roi.min_side_frac").as_double();
    b.fusion.weight_yolo = y.at("bbox.fusion.weight_yolo").as_double();
    b.fusion.weight_bg = y.at("bbox.fusion.weight_bg").as_double();
    b.fusion.weight_interp = y.at("bbox.fusion.weight_interp").as_double();
    b.fusion.bg_score = y.at("bbox.fusion.bg_score").as_double();
    b.fusion.bg_gate_iou = y.at("bbox.fusion.bg_gate_iou").as_double();
    b.fusion.max_gap_frames = y.at("bbox.fusion.max_gap_frames").as_int();
    b.fusion.half_window = y.at("bbox.smoothing.half_window").as_int();
    b.fusion.sigma = y.at("bbox.smoothing.sigma").as_double();
    b.fusion.every_n_frames = b.yolo.every_n_frames;
    b.multi_conf = y.at("bbox.multi_person.conf").as_double();
    b.multi_confirm_runs = y.at("bbox.multi_person.confirm_runs").as_int();
    b.multi_suspend_batches = y.at("bbox.multi_person.suspend_batches").as_int();

    c.pose.model = y.at("pose.model").as_string();
    c.pose.input_w = y.at("pose.input_size").list.at(0).as_int();
    c.pose.input_h = y.at("pose.input_size").list.at(1).as_int();
    c.pose.bbox_padding = y.at("pose.bbox_padding").as_double();
    c.pose.simcc_split_ratio = y.at("pose.simcc_split_ratio").as_double();
    c.pose.rgb_input = y.at("pose.rgb_input").as_bool();

    c.svm_model = y.at("classic.svm_model").as_string();
    c.classic.svm_threshold = y.at("classic.svm_threshold").as_double();
    c.classic.use_model_threshold = y.at("classic.use_model_threshold").as_bool();
    c.classic.suppress_vr_below = y.at("classic.high_sensitive.suppress_vr_below").as_double();
    c.classic.suppress_vc_below = y.at("classic.high_sensitive.suppress_vc_below").as_double();
    c.classic.force_vc_above = y.at("classic.high_sensitive.force_vc_above").as_double();
    c.classic.force_vr_above = y.at("classic.high_sensitive.force_vr_above").as_double();
    c.classic.post1_min_count = y.at("classic.post1.min_count").as_int();
    c.classic.post2_min_count = y.at("classic.post2.min_count").as_int();

    c.cnn_model = y.at("deep.cnn_model").as_string();
    c.deep.cnn_threshold = y.at("deep.cnn_threshold").as_double();
    c.deep.use_model_threshold = y.at("deep.use_model_threshold").as_bool();
    c.deep.window_stride = y.at("deep.window_stride").as_int();
    c.deep.cooldown_batches = y.at("deep.cooldown_batches").as_int();
    c.deep.min_mean_confidence = y.at("deep.min_mean_confidence").as_double();
    c.deep.min_frame_confidence = y.at("deep.min_frame_confidence").as_double();
    if (c.deep.window_stride < 1) throw std::runtime_error("deep.window_stride must be >= 1");

    c.detector.batch_size = c.batch_size;
    c.detector.fps = c.target_fps;
    c.detector.window_stride = c.deep.window_stride;
    c.detector.cooldown_batches = c.deep.cooldown_batches;
    c.detector.multi_person_suspend_batches = b.multi_suspend_batches;

    c.camera_source = y.at("camera.source").as_string();
    c.camera_width = y.at("camera.width").as_int();
    c.camera_height = y.at("camera.height").as_int();
    c.trust_camera_rate = y.at("camera.trust_camera_rate").as_bool();
    c.max_backlog_batches = y.at("camera.max_backlog_batches").as_int();
    c.log_timing = y.at("camera.log_timing").as_bool();

    if (y.has("infer.videos") && y.at("infer.videos").type == YamlNode::Type::List)
        for (const YamlNode& v : y.at("infer.videos").list) c.videos.push_back(v.as_string());
    if (y.has("infer.video_list_file")) c.video_list_file = y.at("infer.video_list_file").as_string();
    c.output_dir = y.at("infer.output_dir").as_string();
    return c;
}

}  // namespace fd
