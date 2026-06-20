// =========================================================
// Pain Detection — exact port of Pain-Final/Pain Detection/
//   - frontends.py  : logmel5 feature extractor (50,32,5)
//   - router.py     : YAMNet TFLite -> {scream, speech}
//   - pipeline.py   : cascade decision (PAIN/FINE/NEITHER)
//   - live_inference.py : session loop (warmup -> alarm ->
//                         rolling-buffer scoring -> sustained-N decision)
//
// State machine: when fall=0 main.cpp runs WUW; when fall flips 0->1
// it calls pain::runPainSession(), which captures audio, scores it,
// and returns FINE/PAIN/UNDECIDED.
// =========================================================
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

namespace pain {

// ─── Constants (match frontends.py / pipeline.py / live_inference.py) ───────
constexpr int   SR             = 16000;
constexpr int   WINDOW_LEN     = SR;          // 1 s — feature window
constexpr int   N_FFT          = 320;
constexpr int   HOP_LENGTH     = 320;
constexpr int   N_MELS         = 32;
constexpr float FMIN           = 60.0f;
constexpr float FMAX           = 6000.0f;
constexpr int   TARGET_WIDTH   = 50;          // WINDOW_LEN / HOP_LENGTH
constexpr int   N_CHANNELS     = 5;

// F0 / YIN params (librosa.yin defaults)
constexpr int   F0_FMIN        = 50;
constexpr int   F0_FMAX        = 500;
constexpr int   F0_FRAME_LEN   = 2048;
constexpr float YIN_TROUGH_THR = 0.1f;        // librosa.yin default

// YAMNet input length
constexpr int   YAMNET_FIXED_LEN = 15600;

// Cascade thresholds (pipeline.py DEFAULTS — recall-leaning val-tuned)
constexpr float SCREAM_PAIN_TH   = 0.30f;
constexpr float ROUTER_SPEECH_TH = 0.30f;
constexpr float PAIN_SPEECH_TH   = 0.36f;

// Live-inference session params (live_inference.py)
// No warmup: pain features are pure log-mel + YAMNet (stateless per window),
// unlike WUW's PCEN whose adaptive AGC needs to converge. The 1 s feature
// window fills during the alarm beeps, so scoring is valid from the first
// post-beep frame. Keeping this 0 makes the "speak now" prompt fire right after
// the fall instead of ~5 s later.
constexpr float WARMUP_SEC     = 0.0f;
constexpr float RECORD_SEC     = 5.0f;
constexpr float HOP_SEC        = 0.3f;
constexpr float EMA_ALPHA      = 0.4f;
constexpr float PAIN_THRESHOLD = 0.30f;       // EMA fire threshold
constexpr float FINE_THRESHOLD = 0.10f;
constexpr int   SUSTAINED_N    = 3;

constexpr float ALARM_FREQ_HZ      = 900.0f;
constexpr float ALARM_DUR_SEC      = 0.30f;
// Pain pre-roll: 3 beeps separated by short gaps, recognized as an alarm
// pattern (most fall-alert devices use 2-3 short beeps for this).
constexpr int   PAIN_BEEPS_BEFORE  = 3;
constexpr float PAIN_BEEP_GAP_SEC  = 0.15f;

// feature_stats.npz contents (mean, std per channel).
// Hardcoded so we don't ship a .npz reader; regenerate from
// Pain-Final/Pain Detection/models_speech/feature_stats.npz after each retrain.
extern const float FEAT_MEAN[N_CHANNELS];
extern const float FEAT_STD[N_CHANNELS];

// ─── Types ──────────────────────────────────────────────────────────────────
struct RouterScores { float scream; float speech; };

struct PainResult {
    float router_scream = 0.0f;
    float router_speech = 0.0f;
    float pain_speech   = -1.0f;     // -1 = specialist not invoked
    float fused_prob    = 0.0f;
    enum Decision { NEITHER, FINE, PAIN } decision = NEITHER;
    enum Branch   { B_NONE, B_SCREAM, B_SPEECH }  branch   = B_NONE;
};

enum class SessionResult { FINE, PAIN, UNDECIDED, ERROR };
const char* sessionResultName(SessionResult r);

// ─── YAMNet router (TFLite, float32) ────────────────────────────────────────
class YamnetRouter {
public:
    bool init(const char* path);
    // Picks the max scream-class score and max speech-class score over the
    // 521-class YAMNet output, on the trailing YAMNET_FIXED_LEN samples of
    // `audio`. Leading-pads with zeros if shorter.
    RouterScores score(const std::vector<float>& audio);
private:
    std::unique_ptr<tflite::FlatBufferModel> model_;
    std::unique_ptr<tflite::Interpreter>     interp_;
};

// ─── Speech specialist (TFLite, int8 quant) ─────────────────────────────────
class SpeechSpecialist {
public:
    bool init(const char* path);
    // feat: row-major (TARGET_WIDTH*N_MELS*N_CHANNELS) float, already
    // normalized via FEAT_MEAN/FEAT_STD. Returns sigmoid prob in [0,1].
    float predict(const std::vector<float>& feat_normalized);
private:
    std::unique_ptr<tflite::FlatBufferModel> model_;
    std::unique_ptr<tflite::Interpreter>     interp_;
    float input_scale_  = 1.0f;
    int   input_zero_   = 0;
    float output_scale_ = 1.0f;
    int   output_zero_  = 0;
    bool  input_int8_   = false;
    bool  output_int8_  = false;
};

// ─── Feature extractor: logmel5 (50, 32, 5) ─────────────────────────────────
class LogMel5Extractor {
public:
    LogMel5Extractor();
    // Audio: float32 mono @ 16 kHz; uses last 1 s. Returns row-major
    // (TARGET_WIDTH × N_MELS × N_CHANNELS) flattened — matches Python
    // np.stack(..., axis=-1) layout (time, mel, channel).
    std::vector<float> extract(const std::vector<float>& audio);
private:
    std::vector<std::vector<float>> filterbank_;   // [N_MELS][N_FFT/2+1]
    std::vector<float>              hann_;          // [N_FFT]
};

// ─── Cascade pipeline (router + specialist) ─────────────────────────────────
class PainPipeline {
public:
    bool init(const char* yamnet_path, const char* speech_path);
    // Audio: ≥ 1 s float32 mono @ 16 kHz (longer is fine; only last 1 s
    // is used for the router, only the full buffer for the specialist
    // when invoked).
    PainResult score(const std::vector<float>& audio);
private:
    YamnetRouter      router_;
    SpeechSpecialist  speech_;
    LogMel5Extractor  extractor_;
};

// ─── Session: warmup -> alarm -> rolling-buffer scoring -> decision ─────────
// Audio comes from `read_audio(out, n)` — a caller-supplied blocking reader
// that fills `out` with the next n float samples @ SAMPLE_RATE (main wires it
// to the shared AudioRing fed by the dedicated capture thread, so pain no
// longer opens ALSA itself and there's no two-opener race on the mic). The
// alarm beeps still play directly on the ALSA playback device. Blocking call.
using PainAudioReader = std::function<void(std::vector<float>&, int)>;
SessionResult runPainSession(PainPipeline& pipe, const PainAudioReader& read_audio);

// Test hook so main.cpp can flip the fall variable from stdin etc.
extern std::atomic<int> g_fall;

}  // namespace pain
