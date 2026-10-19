#include "representation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace fd {
namespace {

// Canonical order of the 15 points and the two supported tensor orders (see constants.py).
const char* const kCanonical[15] = {"bbox_tl", "head", "left_shoulder", "right_shoulder", "left_elbow",
                                    "right_elbow", "left_wrist", "right_wrist", "left_hip", "right_hip",
                                    "left_knee", "right_knee", "left_ankle", "right_ankle", "bbox_br"};
// Canonical slot 0 = top-left, 1 = head, 2..13 = COCO keypoints 5..16 in COCO order, 14 = bottom-right.
const char* const kLegacy[15] = {"bbox_tl", "head", "left_shoulder", "right_shoulder", "left_elbow", "right_elbow",
                                 "left_wrist", "right_wrist", "left_hip", "right_hip", "left_knee", "right_knee",
                                 "left_ankle", "right_ankle", "bbox_br"};
const char* const kChain[15] = {"bbox_tl", "left_wrist", "left_elbow", "left_shoulder", "head", "right_shoulder",
                                "right_elbow", "right_wrist", "right_hip", "right_knee", "right_ankle",
                                "left_ankle", "left_knee", "left_hip", "bbox_br"};

std::vector<int> order_index(const std::string& point_order) {
    const char* const* names;
    if (point_order == "legacy") names = kLegacy;
    else if (point_order == "chain") names = kChain;
    else throw std::runtime_error("unknown point_order: " + point_order);
    std::vector<int> idx;
    for (int i = 0; i < 15; ++i) {
        int found = -1;
        for (int j = 0; j < 15; ++j)
            if (std::strcmp(names[i], kCanonical[j]) == 0) found = j;
        idx.push_back(found);
    }
    return idx;
}

}  // namespace

// ------------------------------------------------------------------ signature (SHA-1 + python repr)
std::string sha1_hex(const std::string& data) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string msg = data;
    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bit_len >> (8 * i)) & 0xFF));
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(msg[off + 4 * i])) << 24) | (uint32_t(uint8_t(msg[off + 4 * i + 1])) << 16) |
                   (uint32_t(uint8_t(msg[off + 4 * i + 2])) << 8) | uint32_t(uint8_t(msg[off + 4 * i + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    char out[41];
    std::snprintf(out, sizeof(out), "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return out;
}

std::string python_float_repr(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);   // shortest round-trip representation
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string FeatureSpec::signature() const {
    // Mirrors json.dumps({"v": 1, **asdict(spec)}, sort_keys=True) of the Python FeatureSpec.
    std::string json = "{\"batch_bbox_margin\": " + python_float_repr(batch_bbox_margin) +
                       ", \"channels\": " + std::to_string(channels) +
                       ", \"point_order\": \"" + point_order + "\"" +
                       ", \"smooth_half_window\": " + std::to_string(smooth_half_window) +
                       ", \"smooth_sigma\": " + python_float_repr(smooth_sigma) +
                       ", \"target_fps\": " + python_float_repr(target_fps) +
                       ", \"v\": 1, \"window\": " + std::to_string(window) + "}";
    return sha1_hex(json).substr(0, 16);
}

// ------------------------------------------------------------------ features
BatchRef batch_reference(const std::vector<Box>& boxes, double margin) {
    double x1 = boxes[0].x1, y1 = boxes[0].y1, x2 = boxes[0].x2, y2 = boxes[0].y2;
    for (const Box& b : boxes) {
        x1 = std::min(x1, b.x1); y1 = std::min(y1, b.y1);
        x2 = std::max(x2, b.x2); y2 = std::max(y2, b.y2);
    }
    const double side = std::max({x2 - x1, y2 - y1, 1.0}) * margin;
    return {(x1 + x2) / 2.0 - side / 2.0, (y1 + y2) / 2.0 - side / 2.0, side};
}

std::vector<float> window_features80(const std::vector<Box>& boxes, const BatchRef& ref) {
    std::vector<float> out;
    out.reserve(boxes.size() * 5);
    for (const Box& b : boxes) {
        out.push_back(static_cast<float>((b.y1 - ref.y0) / ref.side));
        out.push_back(static_cast<float>((b.y2 - ref.y0) / ref.side));
        out.push_back(static_cast<float>((b.x1 - ref.x0) / ref.side));
        out.push_back(static_cast<float>((b.x2 - ref.x0) / ref.side));
        out.push_back(static_cast<float>((b.x2 - b.x1) / std::max(b.y2 - b.y1, 1e-6)));
    }
    return out;
}

WindowFeatures build_window(const std::vector<Box>& boxes, const std::vector<PoseResult>& poses, const FeatureSpec& spec) {
    const int T = static_cast<int>(boxes.size());
    const BatchRef ref = batch_reference(boxes, spec.batch_bbox_margin);

    // 17 -> 13 points: head = confidence-weighted mean of the face keypoints 0..4.
    std::vector<double> kp(T * 13 * 2), sc(T * 13);
    for (int t = 0; t < T; ++t) {
        const PoseResult& p = poses[t];
        double wsum = 0, hx = 0, hy = 0, face_score = 0;
        for (int i = 0; i < 5; ++i) {
            const double w = std::max(p.score[i], 1e-3);
            wsum += w; hx += p.kp[i][0] * w; hy += p.kp[i][1] * w; face_score += p.score[i];
        }
        kp[(t * 13) * 2] = hx / wsum;
        kp[(t * 13) * 2 + 1] = hy / wsum;
        sc[t * 13] = std::min(std::max(face_score / 5.0, 0.0), 1.0);
        for (int j = 1; j < 13; ++j) {
            kp[(t * 13 + j) * 2] = p.kp[4 + j][0];
            kp[(t * 13 + j) * 2 + 1] = p.kp[4 + j][1];
            sc[t * 13 + j] = std::min(std::max(p.score[4 + j], 0.0), 1.0);
        }
    }

    // Confidence-aware Gaussian smoothing over time (weight = gauss(dt) * confidence(source frame)).
    std::vector<double> smooth(kp.size());
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < 13; ++j) {
            double denom = 0, nx = 0, ny = 0;
            for (int s = 0; s < T; ++s) {
                const int off = t - s;
                if (std::abs(off) > spec.smooth_half_window) continue;
                const double g = std::exp(-0.5 * std::pow(off / spec.smooth_sigma, 2));
                const double w = g * sc[s * 13 + j];
                denom += w; nx += w * kp[(s * 13 + j) * 2]; ny += w * kp[(s * 13 + j) * 2 + 1];
            }
            if (denom > 1e-6) {
                const double d = std::max(denom, 1e-9);
                smooth[(t * 13 + j) * 2] = nx / d;
                smooth[(t * 13 + j) * 2 + 1] = ny / d;
            } else {
                smooth[(t * 13 + j) * 2] = kp[(t * 13 + j) * 2];
                smooth[(t * 13 + j) * 2 + 1] = kp[(t * 13 + j) * 2 + 1];
            }
        }
    }

    // (T, 15, C) tensor in canonical slot order, then permuted to the configured point order.
    const int C = spec.channels;
    std::vector<double> canon(static_cast<size_t>(T) * 15 * C);
    for (int t = 0; t < T; ++t) {
        auto put = [&](int slot, double x, double y, double conf) {
            double* dst = &canon[(static_cast<size_t>(t) * 15 + slot) * C];
            dst[0] = (x - ref.x0) / ref.side;
            dst[1] = (y - ref.y0) / ref.side;
            if (C == 3) dst[2] = conf;
        };
        put(0, boxes[t].x1, boxes[t].y1, 1.0);
        for (int j = 0; j < 13; ++j) put(1 + j, smooth[(t * 13 + j) * 2], smooth[(t * 13 + j) * 2 + 1], sc[t * 13 + j]);
        put(14, boxes[t].x2, boxes[t].y2, 1.0);
    }
    WindowFeatures out;
    out.features80 = window_features80(boxes, ref);
    const std::vector<int> order = order_index(spec.point_order);
    out.cnn_tensor.resize(canon.size());
    for (int t = 0; t < T; ++t)
        for (int p = 0; p < 15; ++p)
            for (int c = 0; c < C; ++c)
                out.cnn_tensor[(static_cast<size_t>(t) * 15 + p) * C + c] =
                    static_cast<float>(canon[(static_cast<size_t>(t) * 15 + order[p]) * C + c]);

    double sum = 0, worst = 1e300;
    for (int t = 0; t < T; ++t) {
        double m = 0;
        for (int j = 0; j < 13; ++j) m += sc[t * 13 + j];
        m /= 13.0;
        sum += m;
        worst = std::min(worst, m);
    }
    out.mean_confidence = sum / T;
    out.min_frame_confidence = worst;
    return out;
}

}  // namespace fd
