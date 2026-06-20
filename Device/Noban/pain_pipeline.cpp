// =========================================================
// pain_pipeline.cpp — implementation of pain_pipeline.hpp.
// Exact port of Pain-Final/Pain Detection/ Python files into C++.
// =========================================================
#include "pain_pipeline.hpp"
#include "parity_dump.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

#include <alsa/asoundlib.h>
#include <complex>
#include <vector>
#include "pocketfft_hdronly.h"

namespace pain {

// ─── feature_stats.npz contents (regenerate after retrain).
//     Last verified against models_speech/feature_stats.npz: exact match. ────
const float FEAT_MEAN[N_CHANNELS] = {
    -46.620926f, -0.12967926f, -0.021138279f, 4.9501286f, -1.8498325f,
};
const float FEAT_STD[N_CHANNELS] = {
    17.219568f, 1.8568699f, 1.1477634f, 0.45175877f, 0.2963114f,
};

// ─── YAMNet class indices (router.py) ───────────────────────────────────────
// SCREAM group: loud / distress / pain-adjacent body sounds.
static const std::vector<int> SCREAM_CLASSES = {
    6, 7, 8, 9, 10, 11,        // Shout, Bellow, Whoop, Yell, Children-shouting, Screaming
    19, 20, 21, 22, 33,        // Crying/sobbing, Baby-cry, Whimper, Wail/moan, Groan
    23, 34, 37, 39, 40,        // Sigh, Grunt, Wheeze, Gasp, Pant
};
static const std::vector<int> SPEECH_CLASSES = {
    0, 1, 2, 3, 4, 65,         // Speech, Child-speech, Conversation, Narration, Babbling, Hubbub
};

// ─── librosa.feature.delta savgol coefficients (width=9) ────────────────────
// Centre coefficients; boundary uses the same kernel clamped to valid
// positions, because when deriv == polyorder the deriv-th derivative of the
// best-fit polynomial is constant across the window (verified empirically
// against scipy.signal.savgol_coeffs with various pos arguments).
//   delta1 (polyorder=1, deriv=1):
//     [4, 3, 2, 1, 0, -1, -2, -3, -4] / 60
static const float DELTA1_KERNEL[9] = {
     4.0f/60, 3.0f/60, 2.0f/60, 1.0f/60, 0.0f,
    -1.0f/60,-2.0f/60,-3.0f/60,-4.0f/60,
};
//   delta2 (polyorder=2, deriv=2):
//     [28, 7, -8, -17, -20, -17, -8, 7, 28] / 462
static const float DELTA2_KERNEL[9] = {
    28.0f/462, 7.0f/462, -8.0f/462, -17.0f/462, -20.0f/462,
   -17.0f/462,-8.0f/462,  7.0f/462,  28.0f/462,
};
constexpr int DELTA_HALF = 4;
constexpr int DELTA_WIDTH = 9;

// ─── Global fall variable (defined here, declared in header) ────────────────
std::atomic<int> g_fall{0};

// ─── SessionResult name ─────────────────────────────────────────────────────
const char* sessionResultName(SessionResult r) {
    switch (r) {
        case SessionResult::FINE:      return "FINE";
        case SessionResult::PAIN:      return "PAIN";
        case SessionResult::UNDECIDED: return "UNDECIDED";
        case SessionResult::ERROR:     return "ERROR";
    }
    return "?";
}

// =========================================================================
// YamnetRouter
// =========================================================================
bool YamnetRouter::init(const char* path) {
    model_ = tflite::FlatBufferModel::BuildFromFile(path);
    if (!model_) { std::cerr << "YAMNet: failed to load model: " << path << "\n"; return false; }
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder(*model_, resolver)(&interp_);
    if (!interp_ || interp_->AllocateTensors() != kTfLiteOk) {
        std::cerr << "YAMNet: AllocateTensors failed\n"; return false;
    }
    std::cout << "✅ YAMNet loaded\n";
    return true;
}

RouterScores YamnetRouter::score(const std::vector<float>& audio) {
    // Prep waveform to FIXED_LEN = 15600 (= 0.975 s); leading-pad if shorter.
    float* in = interp_->typed_input_tensor<float>(0);
    int N = static_cast<int>(audio.size());
    if (N >= YAMNET_FIXED_LEN) {
        std::memcpy(in, audio.data() + (N - YAMNET_FIXED_LEN), YAMNET_FIXED_LEN * sizeof(float));
    } else {
        std::memset(in, 0, (YAMNET_FIXED_LEN - N) * sizeof(float));
        std::memcpy(in + (YAMNET_FIXED_LEN - N), audio.data(), N * sizeof(float));
    }
    interp_->Invoke();
    const float* out = interp_->typed_output_tensor<float>(0);

    float scream = 0.0f, speech = 0.0f;
    for (int idx : SCREAM_CLASSES) scream = std::max(scream, out[idx]);
    for (int idx : SPEECH_CLASSES) speech = std::max(speech, out[idx]);
    float dump[2] = {scream, speech};
    dumpTensorF32("pain_yamnet", dump, 2);
    return {scream, speech};
}

// =========================================================================
// SpeechSpecialist
// =========================================================================
bool SpeechSpecialist::init(const char* path) {
    model_ = tflite::FlatBufferModel::BuildFromFile(path);
    if (!model_) { std::cerr << "Speech specialist: failed to load: " << path << "\n"; return false; }
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder(*model_, resolver)(&interp_);
    if (!interp_ || interp_->AllocateTensors() != kTfLiteOk) {
        std::cerr << "Speech specialist: AllocateTensors failed\n"; return false;
    }
    auto& ip = interp_->input_tensor(0)->params;
    auto& op = interp_->output_tensor(0)->params;
    input_scale_  = ip.scale;
    input_zero_   = ip.zero_point;
    output_scale_ = op.scale;
    output_zero_  = op.zero_point;
    input_int8_   = interp_->input_tensor(0)->type  == kTfLiteInt8;
    output_int8_  = interp_->output_tensor(0)->type == kTfLiteInt8;
    std::cout << "✅ Speech specialist loaded"
              << " (in=" << (input_int8_  ? "int8" : "fp32")
              << " out="<< (output_int8_ ? "int8" : "fp32") << ")\n";
    return true;
}

float SpeechSpecialist::predict(const std::vector<float>& feat_normalized) {
    const int N = static_cast<int>(feat_normalized.size());
    if (input_int8_) {
        int8_t* in = interp_->typed_input_tensor<int8_t>(0);
        for (int i = 0; i < N; ++i) {
            float q = feat_normalized[i] / input_scale_ + input_zero_;
            in[i] = static_cast<int8_t>(std::max(-128.0f, std::min(127.0f, q)));
        }
    } else {
        float* in = interp_->typed_input_tensor<float>(0);
        std::memcpy(in, feat_normalized.data(), N * sizeof(float));
    }
    interp_->Invoke();
    if (output_int8_) {
        const int8_t* out = interp_->typed_output_tensor<int8_t>(0);
        return (static_cast<float>(out[0]) - output_zero_) * output_scale_;
    }
    const float* out = interp_->typed_output_tensor<float>(0);
    return out[0];
}

// =========================================================================
// LogMel5Extractor
// =========================================================================
namespace {

// HzToMel / MelToHz: librosa default is slaney (HTK=False); for our fmin=60
// and fmax=6000 (well within the slaney logarithmic region) both formulas
// produce nearly identical filter centres, but use slaney to match librosa.
// Slaney mel scale:
//   below 1000 Hz: linear scale, 1 mel = 1.5 Hz (i.e. mel = 3*f / 200)
//   above 1000 Hz: log scale,    mel = 15 + log(f / 1000) / log(6.4) * 27
// Slaney mel scale params (std::log not constexpr in C++20 standard,
// so these are runtime const).
constexpr double SLANEY_F_MIN       = 0.0;
constexpr double SLANEY_F_SP        = 200.0 / 3.0;
constexpr double SLANEY_MIN_LOG_HZ  = 1000.0;
constexpr double SLANEY_MIN_LOG_MEL = (SLANEY_MIN_LOG_HZ - SLANEY_F_MIN) / SLANEY_F_SP;
static const double SLANEY_LOGSTEP  = std::log(6.4) / 27.0;

double hzToMelSlaney(double hz) {
    if (hz >= SLANEY_MIN_LOG_HZ)
        return SLANEY_MIN_LOG_MEL + std::log(hz / SLANEY_MIN_LOG_HZ) / SLANEY_LOGSTEP;
    return (hz - SLANEY_F_MIN) / SLANEY_F_SP;
}
double melToHzSlaney(double mels) {
    if (mels >= SLANEY_MIN_LOG_MEL)
        return SLANEY_MIN_LOG_HZ * std::exp(SLANEY_LOGSTEP * (mels - SLANEY_MIN_LOG_MEL));
    return SLANEY_F_MIN + SLANEY_F_SP * mels;
}

std::vector<std::vector<float>> buildSlaneyMelFilterbank() {
    // librosa.filters.mel(n_fft=320, sr=16000, n_mels=32, fmin=60, fmax=6000,
    //                     htk=False, norm='slaney')
    const int nFft = N_FFT / 2 + 1;
    std::vector<double> melPts(N_MELS + 2);
    double mMin = hzToMelSlaney(FMIN), mMax = hzToMelSlaney(FMAX);
    for (int i = 0; i < N_MELS + 2; ++i)
        melPts[i] = melToHzSlaney(mMin + i * (mMax - mMin) / (N_MELS + 1));
    std::vector<double> fftFreqs(nFft);
    for (int k = 0; k < nFft; ++k) fftFreqs[k] = double(k) * SR / N_FFT;
    std::vector<std::vector<float>> fb(N_MELS, std::vector<float>(nFft, 0.0f));
    for (int m = 0; m < N_MELS; ++m) {
        double lo = melPts[m], ce = melPts[m + 1], up = melPts[m + 2];
        double enorm = 2.0 / (up - lo);              // slaney area normalization
        for (int k = 0; k < nFft; ++k) {
            double f = fftFreqs[k];
            double w = 0.0;
            if (f >= lo && f <= ce)      w = (f - lo) / (ce - lo);
            else if (f > ce && f <= up)  w = (up - f) / (up - ce);
            fb[m][k] = static_cast<float>(w * enorm);
        }
    }
    return fb;
}

std::vector<float> hannWindow(int n) {
    // PERIODIC Hann (fftbins=True) matches scipy.signal.get_window('hann', N, fftbins=True),
    // which is what librosa.feature.melspectrogram calls. Denominator is n (NOT n-1).
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / n));
    return w;
}

// Apply savgol-style delta with mode='interp'.
// `out_len` is the time-axis length. Kernel is symmetric of length 9.
// For positions 0..3 we clamp to position 4 (same convolution result as the
// interp-mode boundary fit when deriv == polyorder, since the deriv'th
// derivative of the polynomial fit is constant across the window).
void applyDelta(const float* in, int out_len, const float kernel[9], float* out) {
    // Convolution semantics: scipy.signal.savgol_filter -> convolve1d flips
    // the kernel. Equivalent operation here: kernel[k] multiplies the input
    // at offset (centre + half - k), i.e. the kernel is iterated REVERSED
    // over the input window.
    for (int t = 0; t < out_len; ++t) {
        int centre = std::max(DELTA_HALF, std::min(out_len - 1 - DELTA_HALF, t));
        float acc = 0.0f;
        for (int k = 0; k < DELTA_WIDTH; ++k) {
            acc += kernel[k] * in[centre + DELTA_HALF - k];
        }
        out[t] = acc;
    }
}

// librosa.yin — port of librosa.yin (2.5 s window, but called on 1 s here).
// Returns N_FRAMES values of F0 in Hz (0 = no pitch detected, librosa uses
// half-sample-period when no trough below threshold is found; we replicate).
//
// Algorithm:
//   1. center=True (default): reflect-pad audio by frame_length/2 on each side.
//   2. For each frame, compute the squared-difference function via FFT-based
//      autocorrelation (faster than naive), then cumulative-mean-normalized.
//   3. Pick the smallest tau in [tau_min, tau_max] whose value is below
//      threshold AND a local minimum; refine with parabolic interp.
//   4. If no such tau, fall back to the global minimum in the search range
//      (which librosa also refines with parabolic interp).
//
// We use direct double-sum CMNDF rather than FFT autocorr to keep the
// implementation small. For 51 frames × 2048 samples × (tau_max - tau_min)
// = 51 × 2048 × ~280 ≈ 30 M MACs per session — runs comfortably on the Pi
// since the speech branch only fires periodically.
std::vector<float> librosaYin(const std::vector<float>& y_in) {
    // Matches librosa.yin (default win_length=frame_length/2, center=True,
    // pad_mode='reflect' (librosa's actual default), trough_threshold=0.1,
    // parabolic interp clamped).
    const int  frame_len  = F0_FRAME_LEN;          // 2048
    const int  win_length = frame_len / 2;         // 1024
    const int  hop        = HOP_LENGTH;            // 320
    const int  min_period = std::max(1, int(std::floor(double(SR) / F0_FMAX)));   // 32
    const int  max_period = std::min(int(std::ceil(double(SR) / F0_FMIN)),
                                     frame_len - win_length - 1);                  // 320
    const float thr       = YIN_TROUGH_THR;

    // center=True, pad_mode='constant' — librosa.yin's actual default is
    // 'constant' (zeros), NOT 'reflect'. Allocate buffer with leading and
    // trailing zero-pad of frame_len/2 around the signal.
    const int pad = frame_len / 2;
    const int N = static_cast<int>(y_in.size());
    std::vector<float> y(N + 2 * pad, 0.0f);
    std::memcpy(y.data() + pad, y_in.data(), N * sizeof(float));

    // Number of frames (librosa center=True): 1 + N // hop.
    // For N=16000, hop=320: 51.
    const int n_frames = 1 + N / hop;
    std::vector<float> f0(n_frames, 0.0f);

    // Preallocate hot-loop buffers. Use double precision throughout — librosa's
    // FFT-based CMNDF runs at float64 (numpy default) and small accumulator
    // differences flip which trough is global-min, which flips F0 by an octave.
    //
    // librosa.yin computes SDF via FFT-based autocorrelation rather than
    // direct sum. The two are mathematically equivalent but numerically
    // differ by ~1e-3 at small values (FFT roundoff). To match librosa
    // bit-for-bit, we use the same FFT approach.
    //
    // Algorithm (from librosa.core.pitch._cumulative_mean_normalized_difference):
    //   a   = rfft(frame, N=FRAME_LEN)
    //   b   = rfft(reverse(frame[1..WIN_LEN]), N=FRAME_LEN)     # length WIN_LEN, zero-padded
    //   acf = irfft(a * b, N=FRAME_LEN)[WIN_LEN:]               # length FRAME_LEN - WIN_LEN
    //   energy = cumsum(frame^2)[WIN_LEN:] - cumsum(frame^2)[:-WIN_LEN]
    //   SDF  = energy[0] + energy - 2*acf
    //   CMNDF = SDF[tau] / (cumsum(SDF[1..tau]) / tau + tiny)
    // pocketfft float64 real-to-complex / complex-to-real. Bit-exact to
    // numpy.fft.rfft/irfft (numpy uses pocketfft internally since 1.17).
    static thread_local std::vector<double>               buf(frame_len);
    static thread_local std::vector<std::complex<double>> A(frame_len/2 + 1);
    static thread_local std::vector<std::complex<double>> B(frame_len/2 + 1);
    static thread_local std::vector<double>               inv_out(frame_len);
    const pocketfft::shape_t  fft_shape_yin{static_cast<size_t>(frame_len)};
    const pocketfft::stride_t in_stride_d{sizeof(double)};
    const pocketfft::stride_t out_stride_d{sizeof(std::complex<double>)};
    const pocketfft::shape_t  fft_axes_yin{0};
    constexpr double TINY = 2.2250738585072014e-308;   // numpy float64 tiny
    constexpr double CLIP = 1e-6;                       // librosa zero-clip threshold

    std::vector<double> energy(frame_len - win_length, 0.0);   // energy[t]
    std::vector<double> sdf(frame_len - win_length, 0.0);       // SDF over all taus
    std::vector<float>  cmndf(max_period + 1, 1.0f);            // CMNDF d'[tau]
    cmndf[0] = 1.0f;

    for (int t = 0; t < n_frames; ++t) {
        const int start = t * hop;

        // ── FFT of the full frame ───────────────────────────────────────────
        for (int i = 0; i < frame_len; ++i) buf[i] = double(y[start + i]);
        pocketfft::r2c(fft_shape_yin, in_stride_d, out_stride_d, fft_axes_yin,
                       pocketfft::FORWARD, buf.data(), A.data(), 1.0);

        // ── FFT of REVERSED frame[1..WIN_LEN] zero-padded ──────────────────
        // librosa: frame[WIN_LEN:0:-1] = [frame[WIN_LEN], ..., frame[1]] (length WIN_LEN)
        for (int i = 0; i < win_length; ++i)
            buf[i] = double(y[start + win_length - i]);
        for (int i = win_length; i < frame_len; ++i) buf[i] = 0.0;
        pocketfft::r2c(fft_shape_yin, in_stride_d, out_stride_d, fft_axes_yin,
                       pocketfft::FORWARD, buf.data(), B.data(), 1.0);

        // ── A * B (complex element-wise) ────────────────────────────────────
        for (int k = 0; k <= frame_len/2; ++k) {
            A[k] = A[k] * B[k];
        }

        // ── IFFT and take [WIN_LEN:] ────────────────────────────────────────
        // pocketfft c2r with fct=1/N matches numpy.fft.irfft bit-exact.
        pocketfft::c2r(fft_shape_yin, out_stride_d, in_stride_d, fft_axes_yin,
                       pocketfft::BACKWARD, A.data(), inv_out.data(),
                       1.0 / double(frame_len));
        for (int tau = 0; tau < int(sdf.size()); ++tau) {
            double v = inv_out[win_length + tau];
            if (std::fabs(v) < CLIP) v = 0.0;
            sdf[tau] = v;   // will subtract from energy below
        }
        // Diagnostic: dump raw ACF before SDF combination
        {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "yin_acf_t%02d", t);
            std::vector<float> acf_slice;
            for (int tau = min_period; tau <= max_period; ++tau)
                acf_slice.push_back(float(sdf[tau]));
            dumpTensorF32(tag, acf_slice);
        }

        // ── Energy window: cumsum(y^2)[WIN_LEN:] - cumsum(y^2)[:-WIN_LEN] ──
        // librosa: e_cumsum = cumsum(frame**2); energy = e_cumsum[WIN_LEN:] - e_cumsum[:-WIN_LEN].
        // A naive sliding-window approach (running_sq += new - old) drifts by ~4e-3
        // over ~1024 iterations due to float64 catastrophic cancellation when
        // adding/subtracting similarly-sized values. The cumsum-then-difference
        // path keeps each cell's value computed from a single contiguous sum,
        // which preserves precision the same way numpy.cumsum does.
        static thread_local std::vector<double> sq_cumsum;
        if ((int)sq_cumsum.size() != frame_len) sq_cumsum.assign(frame_len, 0.0);
        double cum = 0.0;
        for (int j = 0; j < frame_len; ++j) {
            double v = double(y[start + j]);
            cum += v * v;
            sq_cumsum[j] = cum;
        }
        // librosa: energy = e_cumsum[WIN_LEN:] - e_cumsum[:-WIN_LEN]
        //   energy[tau] = sq_cumsum[tau + WIN_LEN] - sq_cumsum[tau]
        // Note this drops frame[0]^2 from the first window — librosa's indexing
        // intentionally aligns the two halves of the difference function this way.
        for (int tau = 0; tau < int(energy.size()); ++tau) {
            double e = sq_cumsum[tau + win_length] - sq_cumsum[tau];
            if (std::fabs(e) < CLIP) e = 0.0;
            energy[tau] = e;
        }

        // ── SDF = energy[0] + energy[tau] - 2*acf[tau] ──────────────────────
        const double e0 = energy[0];
        for (int tau = 0; tau < int(sdf.size()); ++tau) {
            sdf[tau] = e0 + energy[tau] - 2.0 * sdf[tau];
        }
        // Energy dump
        {
            char tag2[32];
            std::snprintf(tag2, sizeof(tag2), "yin_energy_t%02d", t);
            std::vector<float> e_slice;
            for (int tau = min_period; tau <= max_period; ++tau)
                e_slice.push_back(float(energy[tau]));
            dumpTensorF32(tag2, e_slice);
        }
        // Diagnostic SDF dump (matches librosa yin_frames pre-CMNDF slice [min..max])
        {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "yin_sdf_t%02d", t);
            std::vector<float> sdf_slice;
            sdf_slice.reserve(max_period - min_period + 1);
            for (int tau = min_period; tau <= max_period; ++tau)
                sdf_slice.push_back(float(sdf[tau]));
            dumpTensorF32(tag, sdf_slice);
        }

        // ── CMNDF over [min_period, max_period] ─────────────────────────────
        // cumulative_mean[k] = sum(SDF[1..k+1]) / (k+1); for tau, denom = cumulative_mean[tau-1].
        double cumsum_sdf = 0.0;
        cmndf[0] = 1.0f;
        for (int tau = 1; tau <= max_period; ++tau) {
            cumsum_sdf += sdf[tau];
            double mean = cumsum_sdf / double(tau);
            cmndf[tau] = float(sdf[tau] / (mean + TINY));
        }

        // Find the smallest tau in [min_period, max_period] that is a local
        // minimum AND below threshold. librosa.util.localmin asymmetry: the
        // LEFT comparison is strict (cmndf[tau] < cmndf[tau-1]) but the RIGHT
        // is <= (cmndf[tau] <= cmndf[tau+1]). This matters when the CMNDF
        // has a plateau — librosa accepts the LEFT edge of the plateau as a
        // trough; strict-< on both sides would reject it. Caught by parity
        // testing on real speech audio (frame 18 of pain_p11085).
        int best_tau = -1;
        if (cmndf[min_period] < thr && cmndf[min_period] < cmndf[min_period + 1]) {
            best_tau = min_period;
        } else {
            for (int tau = min_period + 1; tau < max_period; ++tau) {
                if (cmndf[tau] < thr
                    && cmndf[tau] <  cmndf[tau - 1]
                    && cmndf[tau] <= cmndf[tau + 1]) {
                    best_tau = tau;
                    break;
                }
            }
            // librosa.util.localmin explicitly overrides the LAST element:
            //   lmini[..., -1] = xi[..., -1] < xi[..., -2]
            // So max_period CAN be a trough if it's strictly less than its
            // left neighbor — no right-neighbor comparison.
            if (best_tau < 0
                && cmndf[max_period] < thr
                && cmndf[max_period] < cmndf[max_period - 1]) {
                best_tau = max_period;
            }
        }
        if (best_tau < 0) {
            // Global min in [min_period, max_period]
            int gmin = min_period;
            for (int tau = min_period + 1; tau <= max_period; ++tau)
                if (cmndf[tau] < cmndf[gmin]) gmin = tau;
            best_tau = gmin;
        }

        // Parabolic interpolation, matching librosa.core.pitch._pi_stencil:
        //   a = x[k+1] + x[k-1] - 2*x[k]          (curvature)
        //   b = (x[k+1] - x[k-1]) / 2             (slope)
        //   if |b| >= |a|: shift = 0              (parabola minimum outside ±1)
        //   else:          shift = -b / a
        // Derivation: parabola through (-1, v_m), (0, v0), (1, v_p) has its
        // vertex at x = (v_m - v_p) / (2*(v_m - 2*v0 + v_p)) — equivalent to
        // -b/a with the definitions above. Previous code had the wrong sign
        // in the numerator; matches frame-44 f0 ~208→~204 Hz on pain_p10555.
        float refined = static_cast<float>(best_tau);
        if (best_tau > min_period && best_tau < max_period) {
            float v_m = cmndf[best_tau - 1], v0 = cmndf[best_tau], v_p = cmndf[best_tau + 1];
            float a = v_p + v_m - 2.0f * v0;
            float b = 0.5f * (v_p - v_m);
            if (std::fabs(b) < std::fabs(a)) {
                refined += -b / a;
            }
        }
        f0[t] = float(SR) / refined;

        // Diagnostic: dump the CMNDF slice [min_period..max_period] for parity
        // inspection. One file per frame: yin_cmndf_t<NN>.bin
        char tag[32];
        std::snprintf(tag, sizeof(tag), "yin_cmndf_t%02d", t);
        std::vector<float> cmndf_slice(
            cmndf.begin() + min_period, cmndf.begin() + max_period + 1);
        dumpTensorF32(tag, cmndf_slice);
    }
    return f0;
}

}  // anon namespace

LogMel5Extractor::LogMel5Extractor()
    : filterbank_(buildSlaneyMelFilterbank()),
      hann_(hannWindow(N_FFT)) {}

std::vector<float> LogMel5Extractor::extract(const std::vector<float>& audio) {
    // Use last WINDOW_LEN samples (leading-pad with zeros if shorter).
    std::vector<float> win(WINDOW_LEN, 0.0f);
    if (int(audio.size()) >= WINDOW_LEN)
        std::memcpy(win.data(), audio.data() + (audio.size() - WINDOW_LEN),
                    WINDOW_LEN * sizeof(float));
    else
        std::memcpy(win.data() + (WINDOW_LEN - audio.size()),
                    audio.data(), audio.size() * sizeof(float));

    // ── Log-mel (channel 0) with center=False ───────────────────────────────
    const int nFft = N_FFT / 2 + 1;
    // Number of frames with center=False, n_fft=320, hop=320, win 16000:
    //   = (WINDOW_LEN - N_FFT) / HOP_LENGTH + 1  =  (16000 - 320)/320 + 1 = 50
    const int nFrames = (WINDOW_LEN - N_FFT) / HOP_LENGTH + 1;  // 50

    // pocketfft real-to-complex transform (bit-exact to numpy.fft.rfft).
    std::vector<std::complex<float>> spec(nFft);
    std::vector<float>               frame(N_FFT);
    const pocketfft::shape_t  fft_shape{static_cast<size_t>(N_FFT)};
    const pocketfft::stride_t in_stride{sizeof(float)};
    const pocketfft::stride_t out_stride{sizeof(std::complex<float>)};
    const pocketfft::shape_t  fft_axes{0};

    // Layout: [time][mel] for log_mel, then we add delta + delta2 same shape;
    // log_F0 and log_RMS are 1-D vectors broadcast over mels.
    std::vector<std::vector<float>> log_mel(nFrames, std::vector<float>(N_MELS, 0.0f));
    std::vector<float> log_rms(nFrames, 0.0f);

    for (int t = 0; t < nFrames; ++t) {
        const int start = t * HOP_LENGTH;
        // Power-spectrum frame (no windowing? librosa power_spec multiplies by Hann)
        for (int i = 0; i < N_FFT; ++i) frame[i] = win[start + i] * hann_[i];
        pocketfft::r2c(fft_shape, in_stride, out_stride, fft_axes,
                       pocketfft::FORWARD, frame.data(), spec.data(), 1.0f);
        std::vector<float> power(nFft);
        for (int k = 0; k < nFft; ++k) {
            float re = spec[k].real(), im = spec[k].imag();
            power[k] = re * re + im * im;       // power=2.0
        }
        for (int m = 0; m < N_MELS; ++m) {
            float v = 0.0f;
            for (int k = 0; k < nFft; ++k) v += filterbank_[m][k] * power[k];
            log_mel[t][m] = v;
        }
        // RMS computed on the RAW (non-windowed) frame, frame_length=320:
        float ss = 0.0f;
        for (int i = 0; i < N_FFT; ++i) ss += win[start + i] * win[start + i];
        log_rms[t] = std::sqrt(ss / N_FFT);
    }

    // power_to_db(ref=1.0, top_db=80):  10*log10(max(S, 1e-10) / 1.0)
    // then clamp at (max - top_db).
    float max_db = -1e9f;
    for (int t = 0; t < nFrames; ++t) {
        for (int m = 0; m < N_MELS; ++m) {
            float v = log_mel[t][m];
            v = 10.0f * std::log10(std::max(v, 1e-10f));
            log_mel[t][m] = v;
            if (v > max_db) max_db = v;
        }
    }
    const float floor_db = max_db - 80.0f;
    for (int t = 0; t < nFrames; ++t)
        for (int m = 0; m < N_MELS; ++m)
            if (log_mel[t][m] < floor_db) log_mel[t][m] = floor_db;

    // Parity dump: post-power_to_db, post-floor log_mel (50 x 32 row-major)
    {
        std::vector<float> flat;
        flat.reserve(nFrames * N_MELS);
        for (int t = 0; t < nFrames; ++t)
            for (int m = 0; m < N_MELS; ++m)
                flat.push_back(log_mel[t][m]);
        dumpTensorF32("pain_log_mel", flat);
    }

    // log_rms = log10(max(rms, 1e-8))
    for (int t = 0; t < nFrames; ++t) {
        float v = log_rms[t];
        log_rms[t] = std::log10(std::max(v, 1e-8f));
    }

    // ── Δ and ΔΔ along time axis (per mel band) ─────────────────────────────
    std::vector<std::vector<float>> delta1(nFrames, std::vector<float>(N_MELS, 0.0f));
    std::vector<std::vector<float>> delta2(nFrames, std::vector<float>(N_MELS, 0.0f));
    std::vector<float> col(nFrames), out(nFrames);
    for (int m = 0; m < N_MELS; ++m) {
        for (int t = 0; t < nFrames; ++t) col[t] = log_mel[t][m];
        applyDelta(col.data(), nFrames, DELTA1_KERNEL, out.data());
        for (int t = 0; t < nFrames; ++t) delta1[t][m] = out[t];
        applyDelta(col.data(), nFrames, DELTA2_KERNEL, out.data());
        for (int t = 0; t < nFrames; ++t) delta2[t][m] = out[t];
    }

    // ── Log-F0 via librosa.yin (center=True), then log1p(max(f0, 0)) ────────
    std::vector<float> f0 = librosaYin(win);
    std::vector<float> log_f0(f0.size());
    for (size_t i = 0; i < f0.size(); ++i)
        log_f0[i] = std::log1p(std::max(f0[i], 0.0f));
    // Crop / leading-pad to TARGET_WIDTH (= 50)
    std::vector<float> log_f0_50(TARGET_WIDTH, 0.0f);
    if (int(log_f0.size()) >= TARGET_WIDTH)
        std::memcpy(log_f0_50.data(), log_f0.data() + (log_f0.size() - TARGET_WIDTH),
                    TARGET_WIDTH * sizeof(float));
    else
        std::memcpy(log_f0_50.data() + (TARGET_WIDTH - log_f0.size()),
                    log_f0.data(), log_f0.size() * sizeof(float));

    // log_rms already at the right length (50 frames since hop=320, win=16000).
    // Now assemble (TARGET_WIDTH × N_MELS × N_CHANNELS) row-major.
    std::vector<float> feat(TARGET_WIDTH * N_MELS * N_CHANNELS, 0.0f);
    const int chunk_bytes = N_CHANNELS;
    for (int t = 0; t < TARGET_WIDTH; ++t) {
        // log_mel is at nFrames == TARGET_WIDTH; if different, align trailing.
        int src_t = (nFrames >= TARGET_WIDTH) ? t + (nFrames - TARGET_WIDTH) : t;
        if (src_t < 0) src_t = 0;
        for (int m = 0; m < N_MELS; ++m) {
            float* slot = &feat[(t * N_MELS + m) * N_CHANNELS];
            slot[0] = log_mel[src_t][m];
            slot[1] = delta1 [src_t][m];
            slot[2] = delta2 [src_t][m];
            slot[3] = log_f0_50[t];
            slot[4] = log_rms[src_t];
        }
    }
    // Apply per-channel normalization (mean/std from feature_stats.npz)
    for (int t = 0; t < TARGET_WIDTH; ++t) {
        for (int m = 0; m < N_MELS; ++m) {
            float* slot = &feat[(t * N_MELS + m) * N_CHANNELS];
            for (int c = 0; c < N_CHANNELS; ++c) {
                slot[c] = (slot[c] - FEAT_MEAN[c]) / FEAT_STD[c];
            }
        }
    }
    dumpTensorF32("pain_logmel5", feat);
    // Per-channel split dumps for parity isolation: each is (50, 32) row-major.
    auto dump_channel = [&](const char* tag, int c) {
        std::vector<float> ch;
        ch.reserve(TARGET_WIDTH * N_MELS);
        for (int t = 0; t < TARGET_WIDTH; ++t)
            for (int m = 0; m < N_MELS; ++m)
                ch.push_back(feat[(t * N_MELS + m) * N_CHANNELS + c]);
        dumpTensorF32(tag, ch);
    };
    dump_channel("pain_ch0_logmel",  0);
    dump_channel("pain_ch1_delta1",  1);
    dump_channel("pain_ch2_delta2",  2);
    dump_channel("pain_ch3_logf0",   3);
    dump_channel("pain_ch4_logrms",  4);
    return feat;
}

// =========================================================================
// PainPipeline
// =========================================================================
bool PainPipeline::init(const char* yamnet_path, const char* speech_path) {
    if (!router_.init(yamnet_path)) return false;
    if (!speech_.init(speech_path)) return false;
    return true;
}

PainResult PainPipeline::score(const std::vector<float>& audio) {
    PainResult r;
    auto rs = router_.score(audio);
    r.router_scream = rs.scream;
    r.router_speech = rs.speech;

    // Branch 1: YAMNet scream score alone is sufficient → PAIN
    if (rs.scream >= SCREAM_PAIN_TH) {
        r.fused_prob = rs.scream;
        r.decision   = PainResult::PAIN;
        r.branch     = PainResult::B_SCREAM;
        return r;
    }
    // Branch 2: speech-gated specialist
    if (rs.speech >= ROUTER_SPEECH_TH) {
        std::vector<float> feat = extractor_.extract(audio);
        float p = speech_.predict(feat);
        r.pain_speech = p;
        r.fused_prob  = p;
        r.decision    = (p >= PAIN_SPEECH_TH) ? PainResult::PAIN : PainResult::FINE;
        r.branch      = PainResult::B_SPEECH;
        return r;
    }
    // Neither
    r.fused_prob = std::max(rs.scream, rs.speech);
    r.decision   = PainResult::NEITHER;
    r.branch     = PainResult::B_NONE;
    return r;
}

// =========================================================================
// Audio capture (ALSA) + session loop
// =========================================================================
namespace {

void playAlarmTone() {
    const int nSamples = int(ALARM_DUR_SEC * SR);
    std::vector<int16_t> buf(nSamples);
    for (int i = 0; i < nSamples; ++i) {
        float t = float(i) / SR;
        float env = std::min(t / 0.01f, 1.0f) *
                    std::min((ALARM_DUR_SEC - t) / 0.01f, 1.0f);
        buf[i] = int16_t(env * 28000.0f * std::sin(2.0f * float(M_PI) * ALARM_FREQ_HZ * t));
    }
    snd_pcm_t* h = nullptr;
    // Use the amp directly (NOT "default": that's an asym PCM whose capture slave
    // is the held mic, which makes playback silent/contended under systemd — same
    // root cause as the pill-audio-silent bug). Matches main.cpp ALSA_PLAYBACK_DEV.
    if (snd_pcm_open(&h, "plughw:CARD=sndrpigooglevoi,DEV=0", SND_PCM_STREAM_PLAYBACK, 0) < 0) return;
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(h, params);
    snd_pcm_hw_params_set_access(h, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(h, params, 1);
    unsigned int rate = SR;
    snd_pcm_hw_params_set_rate_near(h, params, &rate, nullptr);
    snd_pcm_hw_params(h, params);
    snd_pcm_writei(h, buf.data(), nSamples);
    snd_pcm_drain(h);
    snd_pcm_close(h);
}

}  // anon

SessionResult runPainSession(PainPipeline& pipe, const PainAudioReader& read_audio) {
    std::cout << "\n────── PAIN SESSION ──────\n";

    // Audio is supplied by the caller's reader (the shared AudioRing). No ALSA
    // capture handle here — the dedicated capture thread owns the mic.

    // Rolling 1 s buffer (the feature window length).
    std::vector<float> roll(WINDOW_LEN, 0.0f);
    int filled = 0;
    auto feed = [&](const std::vector<float>& chunk) {
        const int k = int(chunk.size());
        if (k >= WINDOW_LEN) {
            std::memcpy(roll.data(), chunk.data() + (k - WINDOW_LEN),
                        WINDOW_LEN * sizeof(float));
            filled = WINDOW_LEN;
        } else {
            std::memmove(roll.data(), roll.data() + k, (WINDOW_LEN - k) * sizeof(float));
            std::memcpy(roll.data() + (WINDOW_LEN - k), chunk.data(), k * sizeof(float));
            filled = std::min(filled + k, WINDOW_LEN);
        }
    };

    const int HOP_SAMPLES = int(HOP_SEC * SR);   // 4800 = 300 ms

    // ── Warmup ──────────────────────────────────────────────────────────────
    std::cout << "[1/3] Warmup " << WARMUP_SEC << " s of ambient audio...\n";
    int warmup_chunks = int(WARMUP_SEC * SR / HOP_SAMPLES);
    std::vector<float> chunk;
    for (int i = 0; i < warmup_chunks; ++i) {
        read_audio(chunk, HOP_SAMPLES);
        feed(chunk);
    }

    // ── Alarm before scoring ────────────────────────────────────────────────
    // After a fall the elder may be disoriented — fire 3 short beeps so the
    // alert is unmistakable. The recording window stays beep-free; a single
    // closing beep fires after the loop ends.
    //
    // CRITICAL: Python (sounddevice callback) keeps the capture stream
    // running THROUGHOUT the alarm. To match that, we play the beeps on a
    // background thread and keep readChunk()ing in the foreground so the
    // ALSA capture ring stays drained and the rolling buffer's "last 1 s"
    // at post_alarm_start reflects what the mic actually heard during the
    // beeps (including any beep bleed) — same as Python.
    std::cout << "[2/3] Alarm: " << PAIN_BEEPS_BEFORE << " × "
              << int(ALARM_DUR_SEC * 1000) << " ms beeps — then speak now.\n";
    std::atomic<bool> alarm_done(false);
    std::thread alarm_th([&]{
        for (int i = 0; i < PAIN_BEEPS_BEFORE; ++i) {
            playAlarmTone();
            if (i + 1 < PAIN_BEEPS_BEFORE) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(int(PAIN_BEEP_GAP_SEC * 1000)));
            }
        }
        alarm_done.store(true);
    });
    // Foreground: keep draining the capture ring at the same hop cadence as
    // the scoring loop will use, so we don't xrun.
    while (!alarm_done.load()) {
        read_audio(chunk, HOP_SAMPLES);
        feed(chunk);
    }
    alarm_th.join();
    // Skip ~300ms of beep-tail bleed so the recorded response doesn\'t start
    // with the alarm beep (matches the wake-word recording delay).
    read_audio(chunk, (SR * 3) / 10);
    auto post_alarm_start = std::chrono::steady_clock::now();

    // ── Scoring loop ────────────────────────────────────────────────────────
    std::cout << "[3/3] Recording " << RECORD_SEC << " s, scoring every "
              << int(HOP_SEC * 1000) << " ms, EMA α=" << EMA_ALPHA << "\n";

    struct Frame { float t_post; float p; bool fully; float ema; };
    std::vector<Frame> frames;
    float ema = 0.0f;
    auto record_end = post_alarm_start + std::chrono::duration<float>(RECORD_SEC);

    while (std::chrono::steady_clock::now() < record_end) {
        read_audio(chunk, HOP_SAMPLES);
        feed(chunk);
        if (filled < WINDOW_LEN) continue;

        auto now = std::chrono::steady_clock::now();
        float t_post = std::chrono::duration<float>(now - post_alarm_start).count();

        PainResult r = pipe.score(roll);
        float p = r.fused_prob;
        bool fully = (t_post >= 1.0f);
        if (fully) ema = EMA_ALPHA * p + (1.0f - EMA_ALPHA) * ema;
        frames.push_back({t_post, p, fully, ema});

        const char* col = (ema >= PAIN_THRESHOLD)  ? "\033[91m"      // red
                        : (ema <= FINE_THRESHOLD)  ? "\033[92m"      // green
                                                   : "\033[93m";    // yellow
        const char* br = (r.branch == PainResult::B_SCREAM) ? "scream"
                       : (r.branch == PainResult::B_SPEECH) ? "speech"
                                                            : "none";
        std::printf("  %s t=%4.1fs p=%.3f ema=%s%.3f\033[0m rs=%.2f rp=%.2f br=%s\n",
                    fully ? "*" : " ", t_post, p, col, ema,
                    r.router_scream, r.router_speech, br);
    }

    // Closing beep — tells the elder the recording window has ended.
    playAlarmTone();

    // ── Sustained-EMA decision ──────────────────────────────────────────────
    int valid = 0, run = 0, longest = 0;
    float max_ema = 0.0f;
    for (auto& f : frames) {
        if (!f.fully) continue;
        valid++;
        max_ema = std::max(max_ema, f.ema);
        if (f.ema >= PAIN_THRESHOLD) { run++; longest = std::max(longest, run); }
        else                          { run = 0; }
    }

    SessionResult result;
    if (valid < SUSTAINED_N) {
        result = SessionResult::UNDECIDED;
        std::cout << "Reason: only " << valid << " valid post-alarm frames "
                  << "(need >= " << SUSTAINED_N << ").\n";
    } else if (longest >= SUSTAINED_N) {
        result = SessionResult::PAIN;
        std::cout << "Reason: longest EMA-PAIN run = " << longest
                  << " (>= " << SUSTAINED_N << "); max EMA = " << max_ema << "\n";
    } else if (max_ema <= FINE_THRESHOLD) {
        result = SessionResult::FINE;
        std::cout << "Reason: max EMA = " << max_ema << " <= " << FINE_THRESHOLD << "\n";
    } else {
        result = SessionResult::UNDECIDED;
        std::cout << "Reason: max EMA = " << max_ema << " between thresholds.\n";
    }
    std::cout << "RESULT: " << sessionResultName(result) << "\n";
    std::cout << "──────────────────────────\n";
    return result;
}

}  // namespace pain
