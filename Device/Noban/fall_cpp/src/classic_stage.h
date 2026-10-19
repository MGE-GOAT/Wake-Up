// Classic (cheap) cascade stage: SVM per frame, then two temporal post-processing filters.
//
// All operators are centred sliding windows of T frames covering [c - T/2, c + T - 1 - T/2]
// (8 frames before and 7 after the centre for T=16), so each stage adds 7 frames of delay:
//
//   final box --(+7)--> SVM flag s1 --(+7)--> post-1 s2 --(+7)--> post-2 s3
//
//  s1: SVM probability >= threshold on the 80-d vector, then the "high-sensitivity" motion rule;
//      frames whose window lacks a person box are not suspicious.
//  s2: window sum of s1 >= post1_min   (removes isolated false alarms)
//  s3: window sum of s2 >= post2_min   (raises sensitivity next to suspicious frames)
#pragma once

#include <map>
#include <vector>

#include "representation.h"
#include "types.h"

namespace fd {

struct ClassicConfig {
    double svm_threshold = 0.4;
    bool use_model_threshold = true;
    double suppress_vr_below = 0.001, suppress_vc_below = -0.001;
    double force_vc_above = 0.017, force_vr_above = 0.02;
    int post1_min_count = 4;
    int post2_min_count = 3;
};

struct ClassicUpdate {  // values that became final during one advance() call, by frame index
    std::map<int, double> svm_prob;
    std::map<int, int> s1, s2, s3;
};

int apply_high_sensitive(const std::vector<float>& vec80, int base, const ClassicConfig& cfg, int window);

class ClassicStage {
public:
    ClassicStage(const ClassicConfig& cfg, const FeatureSpec& spec, ISvm& svm);

    int lookahead_frames() const { return 3 * after_; }
    ClassicUpdate advance(const std::vector<FrameBox>& new_boxes);
    const BoxMap& boxes() const { return boxes_; }

private:
    void trim();

    ClassicConfig cfg_;
    FeatureSpec spec_;
    ISvm& svm_;
    int T_, before_, after_;
    double threshold_;
    BoxMap boxes_;
    int n_boxes_ = 0, next_center_ = 0, next_s2_ = 0, next_s3_ = 0;
    std::map<int, int> s1_, s2_;
};

}  // namespace fd
