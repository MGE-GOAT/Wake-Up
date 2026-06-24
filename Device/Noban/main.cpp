// =========================================================
// Farsi Wake Word + Command Pipeline — Kali Linux
// All models pre-loaded at startup — no reload per detection
// =========================================================
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <functional>
#include <alsa/asoundlib.h>
#include "rnnoise.h"
#include "onnxruntime_cxx_api.h"
#include <complex>
#include "pocketfft_hdronly.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include <curl/curl.h>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>   // stat() used by _led_export_pin
#include <unistd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include "pain_pipeline.hpp"
#include "fall_detection.hpp"
#include "parity_dump.hpp"
#include "mqtt_client.hpp"
#include "media_crypto.hpp"   // Phase 3.1 — E2E encrypt fall/snapshot media
#include "gpio_io.hpp"        // libgpiod v2 — button + RGB LED (Trixie)

// =========================================================
// CONFIG
// =========================================================
const int    SAMPLE_RATE    = 16000;
// AudioRing capacity. Must outlast pre-roll (5s) + a full pain session
// (~5s warmup + ~1.35s beeps + 5s record ≈ 11.4s) so the fall-clip slice is
// still resident when we cut it after the session. 20s @ 16kHz = 1.3 MB.
const int    RING_SECONDS   = 20;
const int    RECORD_SECONDS = 5;        // 5 s of post-wake audio is plenty for
                                        // two-word commands ("قرص", "تماس") and
                                        // also acts as the buffer for the
                                        // fallback voice-message path.
// RPi 4 has 4 cores. Budget when both WUW + fall_detection are inferring:
//   2 cores for WUW DS-CNN-QAT (this constant)
//   2 cores for fall MoVeNet TFLite (FallDetection ctor SetNumThreads(2))
//   1 core for OS / camera capture / mic capture (overlap; all I/O bound)
// Was 3 before fall_detection landed — drop to 2 to avoid oversubscription.
const int    N_THREADS      = 2;        // RPi 4: 2 of 4 cores for WUW
// ── ALSA device names — for I2S MEMS mic (INMP441) and I2S amp (MAX98357A)
// on the Pi, configure /etc/asound.conf so "default" routes to your cards,
// OR change these constants to e.g. "plughw:CARD=sndrpisimplecar,DEV=0".
// Open the I2S devices directly (not "default") — the `type asym` default
// failed to resolve its capture slave under systemd ("capture slave is not
// defined"), while these named devices open cleanly via arecord/aplay.
// micleft = INMP441 LEFT channel→mono (see /etc/asound.conf); playback = amp.
const char*  ALSA_CAPTURE_DEV  = "micleft";
const char*  ALSA_PLAYBACK_DEV = "plughw:CARD=sndrpigooglevoi,DEV=0";
// ── INMP441 exposes 32-bit signed samples (S32_LE) via the standard
// rpi-i2s overlay. Define USE_S32_CAPTURE=1 at compile time to switch the
// capture format from S16_LE to S32_LE (samples are scaled by 1/2^31).
#ifndef USE_S32_CAPTURE
#define USE_S32_CAPTURE 0
#endif
const int    RNNOISE_FRAMES = 480;
const int    VAD_WINDOW     = 512;
const float  VAD_THRESHOLD  = 0.5f;
// All values match the trained Python pipeline (Ds-CNN-QAT.ipynb, cell 18).
const int    STRIDE_SAMPLES = 4800;     // 300 ms — must match training
const int    WINDOW_SAMPLES = 16000;    // 1 s — used for PCEN warmup buffers only
// Decision-layer settings. 0.3/0.60 is the high-recall config; 0.4/0.70 is
// high-precision. Both work for adult wake-word detection; the additional
// gates (margin, N-consecutive, Schmitt re-arm) close the FAR gap from kid
// speech and bursty negatives without sacrificing recall.
const float  ALPHA          = 0.3f;     // EMA smoothing on per-class probs
const float  THRESHOLD_UP   = 0.60f;    // EMA wake-prob fire threshold (normal mode)
const float  GUEST_NOBAN_UP = 0.40f;    // lower "Noban" EMA bar in sleep (the "Hey" gate adds the precision)
const float  NOBAN_GATE_RAW = 0.50f;    // raw "Noban" peak fast-path in sleep (fluid "Hey Noban"; Hey-gated so safe)
const float  THRESHOLD_DOWN = 0.30f;    // Schmitt re-arm: EMA must dip below this to refire
const float  MARGIN_MIN     = 0.20f;    // ema_wake - max(ema_other, ema_noise)
const int    COOLDOWN_FRAMES= 7;        // hard backstop (~2.1 s)
const int    N_MELS         = 32;
const int    N_FFT          = 512;
const int    HOP_LENGTH     = 320;      // hop < n_fft → 192-sample overlap (37.5%)
const int    LOOKBACK       = N_FFT - HOP_LENGTH;  // 192 — carried between chunks
const int    TARGET_WIDTH   = 50;       // 50 frames × 320 samples = 1 s ring buffer
const float  FMIN           = 60.0f;
const float  FMAX           = 6000.0f;
const float  PCEN_ALPHA     = 0.90f;    // calmer AGC (was 0.98)
const float  PCEN_DELTA     = 2.0f;
const float  PCEN_R         = 0.5f;
const float  PCEN_TIME_C    = 0.10f;    // faster smoothing — matches training
const float  PCEN_EPS       = 1e-3f;    // higher floor — kills silence amplification
const char*  TFLITE_MODEL   = "./models/noban_int8.tflite";   // retrained main "Noban" wake model
const char*  HEY_MODEL      = "./models/hey_int8.tflite";     // "Hey" pre-gate model (guest double-gate)
const char*  SILERO_MODEL   = "./models/silero_vad.onnx";
const char*  YAMNET_MODEL   = "./models/yamnet.tflite";
const char*  PAIN_MODEL     = "./models/pain_logmel5.tflite";
// All command interpretation (STT + LLM intent match + routing) lives on the
// server. The device simply POSTs the 5-second post-wake clip as audio/wav
// to /Wake_Command and forgets about it.
//
// Server-side responsibilities:
//   – Run STT (faster-whisper)
//   – Match intent against known commands
//   – If "قرص"  → mark the active pill reminder as acknowledged
//                 (the app picks that up and silences the alarm)
//   – If "تماس" → push an incoming-call FCM notification to subscribers
//   – Else       → save the clip to the device's voice-message inbox
//                 (app shows it under the device's inbox tab)
//
// The device doesn't care what the server decided; it only checks HTTP 200
// to log success/failure.
const char*  SERVER_BASE_URL    = "http://192.168.1.104:5000";
const char*  WAKE_CMD_ENDPOINT  = "/Wake_Command";
const char*  LIVE_ENDPOINT      = "/Live_Device";     // heartbeat + image_req
const char*  MSG_ENDPOINT       = "/Message_Device";  // image or image+video
const long   HTTP_TIMEOUT_S     = 30;
// Keep a resolved host in libcurl's DNS cache for 5 min so every handle isn't
// forced to re-resolve noban.local (mDNS resolution is slow/flaky and was a
// real source of per-call latency). Applied to all curl handles below.
const long   DNS_CACHE_TIMEOUT_S = 300;

// ── Device identity ─────────────────────────────────────────────────────────
// Read from id.txt at startup. Two lines:
//   line 1: device_id (matches `devices.device_id` on server)
//   line 2: 32-char shared password (matches sha256 in `devices.key_hash`)
//
// Lookup order (first existing wins):
//   1. $ELDERLY_ID_FILE (override for testing)
//   2. /var/lib/elderly-care/id.txt   (Pi production location)
//   3. ~/.config/elderly-care/id.txt  (laptop dev location)
//
// If none exist, the device is unprovisioned: loadDeviceIdentity() falls back
// to FACTORY_ID (no credentials) and the main() guard runs BLE provisioning
// only, never the server-facing pipeline.
//
// FACTORY_ID is what the device boots with on a fresh SD card before any
// onboarding — Phase 1 BLE sets the real id.txt and the next start picks
// it up. While DEVICE_ID == FACTORY_ID the device intentionally refuses to
// hit any server endpoints; only the BLE provisioning sidecar runs.
constexpr const char* FACTORY_ID = "Noban";
static std::string g_device_id;
static std::string g_password;
static std::string g_key_hash;        // sha256_hex(g_password) — sent in Key-Hash header
static std::string g_mac;             // stable wlan MAC — sent in Device-Mac header

// Guest / privacy "sleep" mode. Toggled by a 3-rapid-tap button gesture (woken
// by a single press). While set, only FALL detection + the CAMERA are paused
// (privacy/false-trigger control for when the room is crowded / guests are
// over): no fall events, no snapshots, camera released. Everything else keeps
// working — WUW still runs (so the elder can still get help), pill playback,
// caregiver voice notes, and incoming calls all still fire. To suppress false
// WUW fires from chatter, the wake is DOUBLE-GATED in guest: a lenient raw
// "Hey" must open a window, then the full strict-EMA "Noban" must fire within
// it. The button (to wake) and heartbeat (server/app liveness) keep running.
// RAM-only: a reboot comes back ACTIVE so a power blip can't silently leave
// the elder unmonitored.
std::atomic<bool> g_guest_mode{false};

// True only when stdout is an interactive terminal. Used to suppress the live
// per-stride '\r' WUW meter under systemd (where it would flood journald).
static const bool g_stdout_is_tty = isatty(fileno(stdout));

// Read the device's stable hardware MAC (wlan0 preferred, then eth0/end0). The
// server uses it to recognize a re-provisioned device (same MAC, new id) and
// purge the old identity's data. Returns "" if no interface is found.
static std::string readDeviceMac() {
    for (const char* iface : {"wlan0", "eth0", "end0"}) {
        std::ifstream f(std::string("/sys/class/net/") + iface + "/address");
        std::string mac;
        if (f && std::getline(f, mac) && !mac.empty() && mac != "00:00:00:00:00:00")
            return mac;
    }
    return "";
}

#include <openssl/sha.h>

static std::string sha256_hex(const std::string& s) {
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h);
    static const char* hex = "0123456789abcdef";
    std::string out(SHA256_DIGEST_LENGTH * 2, '0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out[2*i]     = hex[(h[i] >> 4) & 0xf];
        out[2*i + 1] = hex[h[i] & 0xf];
    }
    return out;
}

// Read id.txt. Returns false if file missing or malformed.
static bool readIdTxt(const std::string& path, std::string& dev_id, std::string& pw) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    if (!std::getline(f, dev_id)) return false;
    if (!std::getline(f, pw))     return false;
    // Strip trailing \r (handle CRLF line endings).
    if (!dev_id.empty() && dev_id.back() == '\r') dev_id.pop_back();
    if (!pw.empty()     && pw.back()     == '\r') pw.pop_back();
    return !dev_id.empty() && !pw.empty();
}

// Initialize g_device_id / g_password / g_key_hash from disk. Call once at
// startup before any HTTP request goes out.
static void loadDeviceIdentity() {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("ELDERLY_ID_FILE")) candidates.push_back(env);
    candidates.push_back("/var/lib/elderly-care/id.txt");
    if (const char* home = std::getenv("HOME"))
        candidates.push_back(std::string(home) + "/.config/elderly-care/id.txt");

    for (const auto& p : candidates) {
        if (readIdTxt(p, g_device_id, g_password)) {
            g_key_hash = sha256_hex(g_password);
            std::cout << "[id] loaded from " << p
                      << " (device_id=" << g_device_id << ")\n";
            return;
        }
    }

    // No id.txt anywhere → device is unprovisioned (fresh SD card or a
    // post-factory-reset wipe). Boot as FACTORY_ID with no credentials; the
    // main() guard then refuses to touch the server and runs only the BLE
    // provisioning sidecar until the companion app writes a real id.txt.
    g_device_id = FACTORY_ID;
    g_password.clear();
    g_key_hash.clear();
    std::cerr << "[id] WARNING: no id.txt found in any of:\n";
    for (const auto& p : candidates) std::cerr << "        " << p << "\n";
    std::cerr << "[id] booting UNPROVISIONED as device_id=" << g_device_id
              << " — server disabled until BLE setup writes real creds.\n";
}

// Back-compat aliases — keep existing callsites compiling while we migrate.
// These are pointers into the std::string storage so the underlying char*
// stays valid for the process lifetime (g_device_id is static).
#define DEVICE_ID (g_device_id.c_str())
#define KEY_HASH  (g_key_hash.c_str())

// The device knows about NO commands — the server interprets the audio
// and routes it. Command text strings live only in the server config so
// adding/editing commands doesn't require a Pi re-flash.

// =========================================================
// MIC CAPTURE — single ALSA handle owned by main. WUW inference runs in the
// same thread (merged producer+consumer): the 300ms blocking read paces the
// loop, the ~20ms per-stride inference fits comfortably in the kernel ring
// buffer's headroom (xrun threshold is hundreds of ms).
// =========================================================
#if USE_S32_CAPTURE
// INMP441 is a 24-bit mic delivered in S32_LE frames, but the i2s shifting
// adds ~3 bits of headroom above the nominal 2^23 full-scale. Empirically
// calibrated 2026-05-26 with this exact mic + googlevoicehat overlay:
// conversational speech at 1/2^26 lands rms≈0.086 peak≈1.09 — matches the
// training-data range (librosa-loaded int16 audio, rms≈0.05-0.10).
// Earlier guesses of 1/2^31 (256× too quiet, model deaf) and 1/2^23 (7×
// too hot, model saturated → false fires on noise) failed because they
// were based on the datasheet alone, not the actual mic's headroom.
// 2026-06-03 recalibrated to 1/2^27 for the NEW INMP441 captured via the
// `micleft` ALSA device (pure LEFT channel). The original 1/2^26 was set for
// the old capture path (mono downmix of the 2ch card = half level); pure-LEFT
// is ~2x hotter, so 1/2^26 gave rms≈0.175 peak≈1.96 w/ clipping → mel saturated
// → wake word went deaf (w=0.04). Halving to 1/2^27 lands rms≈0.087 peak≈0.98,
// matching the training range. Re-run the empirical check if the capture path
// or mic changes again.
// Runtime-tunable so we can sweep the scale empirically (the C++ PCEN is
// level-sensitive: too hot saturates the mel → deaf, too quiet → flat). Set
// env MIC_SCALE_POW=<n> to use 1/2^n; default 27. Lets us find the firing
// scale with --wuw-wav without recompiling per value.
inline float initMicScale() {
    if (const char* e = getenv("MIC_SCALE_POW")) {
        float p = std::atof(e);
        if (p > 0.0f) return 1.0f / std::pow(2.0f, p);
    }
    return 1.0f / 134217728.0f;   // 1/2^27 default
}
float MIC_SCALE = initMicScale();
#else
constexpr float MIC_SCALE = 1.0f / 32768.0f;
#endif

// Returns a configured capture handle, or nullptr on failure.
snd_pcm_t* openMicHandle() {
    snd_pcm_t* handle = nullptr;
    snd_pcm_hw_params_t* params = nullptr;
    if (snd_pcm_open(&handle, ALSA_CAPTURE_DEV, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "ALSA: failed to open mic (" << ALSA_CAPTURE_DEV << ")\n";
        return nullptr;
    }
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
#if USE_S32_CAPTURE
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S32_LE);
#else
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
#endif
    snd_pcm_hw_params_set_channels(handle, params, 1);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(handle, params, &rate, nullptr);
    // Commit. If ALSA rejects the params the handle is left misconfigured and
    // reads spin-fail in a tight loop — close + return nullptr so the caller
    // backs off and retries cleanly.
    if (snd_pcm_hw_params(handle, params) < 0) {
        std::cerr << "[mic] hw_params commit failed — will retry\n";
        snd_pcm_close(handle);
        return nullptr;
    }
    // Sanity line (once per mic open): the micleft plug must resample to
    // SAMPLE_RATE. A mismatch (e.g. 48000) means time/pitch-distorted features
    // and the model never matches — worth keeping visible.
    if (rate != (unsigned)SAMPLE_RATE)
        std::cerr << "[mic] WARNING: capture rate=" << rate
                  << " != wanted " << SAMPLE_RATE << " (features will be distorted)\n";
    return handle;
}

// Read one STRIDE_SAMPLES chunk; returns it as float in [-1, 1]. On xrun the
// caller should snd_pcm_prepare(handle) and try again; on hard failure
// returns an empty vector.
std::vector<float> readMicStride(snd_pcm_t* handle) {
#if USE_S32_CAPTURE
    std::vector<int32_t> chunk(STRIDE_SAMPLES);
#else
    std::vector<int16_t> chunk(STRIDE_SAMPLES);
#endif
    int rc = snd_pcm_readi(handle, chunk.data(), STRIDE_SAMPLES);
    if (rc < 0) { snd_pcm_prepare(handle); return {}; }
    std::vector<float> fchunk(rc);
    for (int i = 0; i < rc; ++i) fchunk[i] = float(chunk[i]) * MIC_SCALE;
    return fchunk;
}

// ── Shared mic ring buffer ─────────────────────────────────────────────────
// One ALSA reader (audioCaptureThread) is the sole producer; consumers are the
// WUW loop and recordCommand (streaming reads via a cursor) and the fall
// handler (snapshot of the most-recent N seconds for the clip pre-roll). This
// decouples ALSA blocking from WUW inference (gap-free capture) and lets the
// fall event carry audio. Holds RING_SECONDS of float samples @ SAMPLE_RATE.
class AudioRing {
public:
    explicit AudioRing(size_t capacity) : buf_(capacity, 0.0f), cap_(capacity) {}

    // Producer: append n samples.
    void write(const float* d, size_t n) {
        {
            std::lock_guard<std::mutex> lk(m_);
            for (size_t i = 0; i < n; ++i) buf_[(written_ + i) % cap_] = d[i];
            written_ += n;
        }
        cv_.notify_all();
    }

    // Absolute count of samples ever written — also the "live now" cursor a
    // consumer should jump to after a pause so it skips stale buffered audio.
    uint64_t now() { std::lock_guard<std::mutex> lk(m_); return written_; }

    // Streaming read: wait (up to 500 ms) until `want` samples past `from`
    // exist, then copy them. Returns the advanced cursor. On run==false or a
    // timeout (producer idle, e.g. mic released for a call) returns `from` with
    // out cleared, so the caller can re-check state. If the consumer fell
    // behind by more than the capacity, fast-forwards to the freshest window so
    // it never reads samples already overwritten.
    uint64_t read(uint64_t from, size_t want, std::vector<float>& out,
                  const std::atomic<bool>& run) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(500),
                     [&] { return written_ >= from + want || !run.load(); });
        if (!run.load() || written_ < from + want) { out.clear(); return from; }
        if (written_ - from > cap_) from = written_ - cap_;   // resync if behind
        out.resize(want);
        for (size_t i = 0; i < want; ++i) out[i] = buf_[(from + i) % cap_];
        return from + want;
    }

    // Copy the absolute sample range [from, from+want) if still resident
    // (clamped to what hasn't been overwritten). For cutting the fall-clip
    // pre-roll slice — an exact window ending at the fall instant.
    void slice(uint64_t from, size_t want, std::vector<float>& out) {
        std::lock_guard<std::mutex> lk(m_);
        uint64_t oldest = written_ > cap_ ? written_ - cap_ : 0;
        if (from < oldest) from = oldest;
        uint64_t end = std::min(from + want, written_);
        out.clear();
        if (end <= from) return;
        out.resize(static_cast<size_t>(end - from));
        for (size_t i = 0; i < out.size(); ++i) out[i] = buf_[(from + i) % cap_];
    }

    // Best-effort copy of the most-recent `n` samples (fall clip pre-roll).
    void snapshot_recent(size_t n, std::vector<float>& out) {
        std::lock_guard<std::mutex> lk(m_);
        size_t avail = (size_t)std::min<uint64_t>(n, written_);
        avail = std::min(avail, cap_);
        out.resize(avail);
        uint64_t start = written_ - avail;
        for (size_t i = 0; i < avail; ++i) out[i] = buf_[(start + i) % cap_];
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    std::vector<float>      buf_;
    size_t                  cap_;
    uint64_t                written_ = 0;
};

// =========================================================
// FEATURE EXTRACTION: MEL + PCEN  (KFR)
// =========================================================
std::vector<std::vector<float>> buildMelFilterbank() {
    int nFft = N_FFT/2+1; double sr = SAMPLE_RATE;
    // Slaney mel scale (librosa default htk=False): linear below 1 kHz,
    // logarithmic above. Audit found training uses Slaney + Slaney area
    // normalization; the HTK 2595*log10 formula we had before produces a
    // ~75x scale and bin-center mismatch vs librosa.feature.melspectrogram.
    auto hzToMel = [](double hz) {
        const double F_MIN = 0.0, F_SP = 200.0/3.0;        // linear region
        const double MIN_LOG_HZ = 1000.0;
        const double MIN_LOG_MEL = (MIN_LOG_HZ - F_MIN) / F_SP;
        const double LOG_STEP = std::log(6.4) / 27.0;
        if (hz >= MIN_LOG_HZ) return MIN_LOG_MEL + std::log(hz / MIN_LOG_HZ) / LOG_STEP;
        return (hz - F_MIN) / F_SP;
    };
    auto melToHz = [](double mel) {
        const double F_MIN = 0.0, F_SP = 200.0/3.0;
        const double MIN_LOG_HZ = 1000.0;
        const double MIN_LOG_MEL = (MIN_LOG_HZ - F_MIN) / F_SP;
        const double LOG_STEP = std::log(6.4) / 27.0;
        if (mel >= MIN_LOG_MEL) return MIN_LOG_HZ * std::exp(LOG_STEP * (mel - MIN_LOG_MEL));
        return F_MIN + F_SP * mel;
    };
    double melMin = hzToMel(FMIN), melMax = hzToMel(FMAX);
    std::vector<double> melPoints(N_MELS+2);
    for (int i=0;i<N_MELS+2;i++) melPoints[i]=melToHz(melMin+i*(melMax-melMin)/(N_MELS+1));
    std::vector<double> fftFreqs(nFft);
    for (int i=0;i<nFft;i++) fftFreqs[i]=i*sr/N_FFT;
    std::vector<std::vector<float>> f(N_MELS, std::vector<float>(nFft,0.0f));
    for (int m=0;m<N_MELS;m++) {
        double lo=melPoints[m],ce=melPoints[m+1],up=melPoints[m+2];
        // Slaney area normalization: scale each triangle by 2/(up-lo).
        double enorm = 2.0 / (up - lo);
        for (int k=0;k<nFft;k++) {
            double freq=fftFreqs[k];
            double v = 0.0;
            if (freq>=lo&&freq<=ce) v=(freq-lo)/(ce-lo);
            else if (freq>ce&&freq<=up) v=(up-freq)/(up-ce);
            f[m][k]=static_cast<float>(v * enorm);
        }
    }
    return f;
}
std::vector<float> hannWindow(int size) {
    // PERIODIC Hann (fftbins=True), matches scipy.signal.get_window('hann', N, fftbins=True)
    // which is what librosa.feature.melspectrogram uses by default. Denominator is N
    // (NOT N-1, which would be the symmetric variant).
    std::vector<float> w(size);
    for (int i=0;i<size;i++) w[i]=0.5f*(1.0f-std::cos(2.0f*M_PI*i/size));
    return w;
}
// PCEN smoothing state + 50-frame mel ring buffer.
// Mirrors Python's StreamingPCEN class:
//   - M: per-bin IIR smoothing state, carried across calls
//   - mel_buffer: rolling window of the latest 50 PCEN frames (= 1 s)
struct PCENState {
    std::vector<float>              M;            // [N_MELS]
    std::vector<std::vector<float>> mel_buffer;   // [TARGET_WIDTH][N_MELS]
    std::vector<float>              lookback;     // [LOOKBACK] — last LOOKBACK
                                                  // samples of prev chunk, prepended
                                                  // to next chunk so frames spanning
                                                  // the boundary get computed
    bool                            initialized = false;
};

// Single-pass streaming feature extraction.
// Each call processes ONLY the new audio chunk (typically STRIDE_SAMPLES =
// 4800 samples = 15 mel frames), updates the carried PCEN state, appends the
// new frames to the ring buffer (oldest frames roll out), and returns the
// flattened buffer. Matches Python's extract_features in cell 18.
std::vector<float> extractFeatures(const std::vector<float>& chunk, PCENState& state) {
    static auto filterbank = buildMelFilterbank();
    static auto hann = hannWindow(N_FFT);
    // One-shot dump of the mel filterbank for parity inspection
    static bool _fb_dumped = false;
    if (!_fb_dumped) {
        std::vector<float> flat;
        flat.reserve(filterbank.size() * filterbank[0].size());
        for (auto& row : filterbank)
            for (float v : row) flat.push_back(v);
        // Override the fixture-prefix so this dumps regardless of which
        // parity fixture is being processed (we only need it once).
        std::string saved = g_parity_fixture_stem;
        g_parity_fixture_stem = "_global";
        dumpTensorF32("wuw_filterbank", flat);
        g_parity_fixture_stem = saved;
        _fb_dumped = true;
    }
    int nFft = N_FFT/2+1;

    // Build the FFT input as [lookback, chunk] so frames spanning the chunk
    // boundary get computed. Matches Python (cells 2, 17, 18).
    std::vector<float> input;
    input.reserve(state.lookback.size() + chunk.size());
    input.insert(input.end(), state.lookback.begin(), state.lookback.end());
    input.insert(input.end(), chunk.begin(), chunk.end());

    int nFrames = 0;
    for (int s=0; s+N_FFT<=(int)input.size(); s+=HOP_LENGTH) nFrames++;

    std::vector<std::vector<float>> mel(nFrames, std::vector<float>(N_MELS, 0.0f));
    // pocketfft real-to-complex transform — same backend numpy/scipy use.
    // Bit-exact output vs np.fft.rfft (verified by parity test).
    std::vector<float> temp(N_FFT);
    std::vector<std::complex<float>> spectrum(nFft);
    const pocketfft::shape_t  fft_shape{static_cast<size_t>(N_FFT)};
    const pocketfft::stride_t in_stride{sizeof(float)};
    const pocketfft::stride_t out_stride{sizeof(std::complex<float>)};
    const pocketfft::shape_t  fft_axes{0};

    for (int f=0; f<nFrames; f++) {
        int start = f*HOP_LENGTH;
        for (int i=0; i<N_FFT; i++) {
            int idx = start+i;
            temp[i] = (idx<(int)input.size()) ? input[idx]*hann[i] : 0.0f;
        }
        // Dump frame 0's windowed input ONCE per process for parity
        static bool _frame0_dumped = false;
        if (!_frame0_dumped && f == 0 && !g_parity_fixture_stem.empty()) {
            std::vector<float> w0(temp.data(), temp.data() + N_FFT);
            dumpTensorF32("wuw_frame0_input", w0);
            _frame0_dumped = true;
        }
        pocketfft::r2c(fft_shape, in_stride, out_stride, fft_axes,
                       pocketfft::FORWARD, temp.data(), spectrum.data(), 1.0f);
        std::vector<float> mag(nFft);
        // MAGNITUDE (|STFT|) — librosa.feature.melspectrogram(power=1.0) which
        // is what Ds-CNN-QAT training/inference uses with PCEN. Squared input
        // (power=2.0) would shift the mel scale by ~|X| and miscalibrate PCEN.
        for (int k=0; k<nFft; k++) {
            float re = spectrum[k].real();
            float im = spectrum[k].imag();
            mag[k] = std::sqrt(re*re + im*im);
        }
        static bool _power0_dumped = false;
        if (!_power0_dumped && f == 0 && !g_parity_fixture_stem.empty()) {
            dumpTensorF32("wuw_frame0_power", mag);
            _power0_dumped = true;
        }
        for (int m=0; m<N_MELS; m++) {
            float v = 0;
            for (int k=0; k<nFft; k++) v += filterbank[m][k]*mag[k];
            mel[f][m] = v;
        }
    }

    // Scale mel to match Python: librosa.pcen(S=mel*(2**31))
    // PCEN constants (eps, delta) are calibrated for integer-range spectrograms.
    const float MEL_SCALE = 2147483648.0f; // 2^31
    for (int f=0; f<nFrames; f++)
        for (int m=0; m<N_MELS; m++)
            mel[f][m] *= MEL_SCALE;

    // Diagnostic dump: raw mel spectrogram (post-scale, pre-PCEN). Only the
    // last TARGET_WIDTH=50 frames go to PCEN's ring buffer, so dump those.
    if (nFrames >= TARGET_WIDTH) {
        std::vector<float> mel_flat;
        mel_flat.reserve(TARGET_WIDTH * N_MELS);
        for (int f = nFrames - TARGET_WIDTH; f < nFrames; ++f)
            for (int m = 0; m < N_MELS; ++m)
                mel_flat.push_back(mel[f][m]);
        dumpTensorF32("wuw_mel_raw", mel_flat);
    }

    // Apply PCEN frame-by-frame using carried state.M (= librosa's zi).
    // Smoothing coefficient: librosa derives b from the time-constant via
    //   T = time_constant * sr / hop
    //   b = (sqrt(1 + 4*T*T) - 1) / (2*T*T)
    // which is the exact pole of the discrete IIR smoother. Our previous
    // `1 - exp(-1/T)` differed by ~0.15% — small but pointlessly off.
    const double T_pcen = SAMPLE_RATE * (double)PCEN_TIME_C / HOP_LENGTH;
    const float s = static_cast<float>((std::sqrt(1.0 + 4.0 * T_pcen * T_pcen) - 1.0)
                                       / (2.0 * T_pcen * T_pcen));
    float deltaR = std::pow(PCEN_DELTA, PCEN_R);
    std::vector<std::vector<float>> pcen(nFrames, std::vector<float>(N_MELS, 0.0f));
    for (int f=0; f<nFrames; f++) {
        for (int m=0; m<N_MELS; m++) {
            state.M[m] = (1.0f-s)*state.M[m] + s*mel[f][m];
            float sm = std::pow(PCEN_EPS + state.M[m], PCEN_ALPHA);
            pcen[f][m] = std::pow(mel[f][m]/sm + PCEN_DELTA, PCEN_R) - deltaR;
        }
    }

    // Save the last LOOKBACK samples of this chunk for next call's prefix.
    // Matches Python (cells 2, 17, 18): lookback = chunk[-LOOKBACK:].
    if ((int)chunk.size() >= LOOKBACK) {
        state.lookback.assign(chunk.end() - LOOKBACK, chunk.end());
    } else {
        // Partial chunk shorter than LOOKBACK: shift old lookback left, append chunk.
        std::vector<float> combined;
        combined.reserve(state.lookback.size() + chunk.size());
        combined.insert(combined.end(), state.lookback.begin(), state.lookback.end());
        combined.insert(combined.end(), chunk.begin(), chunk.end());
        state.lookback.assign(combined.end() - LOOKBACK, combined.end());
    }

    // Slide ring buffer left by nFrames, append new pcen frames at end.
    // Matches Python: np.roll(buffer, -n_new); buffer[-n_new:] = new_frames
    if (nFrames >= TARGET_WIDTH) {
        for (int f=0; f<TARGET_WIDTH; f++)
            state.mel_buffer[f] = pcen[nFrames - TARGET_WIDTH + f];
    } else if (nFrames > 0) {
        for (int f=0; f<TARGET_WIDTH-nFrames; f++)
            state.mel_buffer[f] = state.mel_buffer[f+nFrames];
        for (int f=0; f<nFrames; f++)
            state.mel_buffer[TARGET_WIDTH-nFrames+f] = pcen[f];
    }

    // Flatten ring buffer to (TARGET_WIDTH × N_MELS) row-major output.
    std::vector<float> output(TARGET_WIDTH * N_MELS, 0.0f);
    for (int f=0; f<TARGET_WIDTH; f++)
        for (int m=0; m<N_MELS; m++)
            output[f*N_MELS + m] = state.mel_buffer[f][m];
    return output;
}

// =========================================================
// WAKE WORD DETECTOR — loaded once at startup
// =========================================================
// Per-class probabilities from the 3-class softmax (matches training order:
// wake=0, other=1, noise=2). The original code only read [0]; we now read
// all three so the decision layer can compute a margin.
struct WakeProbs { float wake; float other; float noise; };

struct WakeWordDetector {
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter>     interp;
    float input_scale=1.f, output_scale=1.f;
    int   input_zero=0,    output_zero=0;
    PCENState pcenState;

    bool init(const char* path) {
        model = tflite::FlatBufferModel::BuildFromFile(path);
        if (!model) { std::cerr << "TFLite: failed to load\n"; return false; }
        tflite::ops::builtin::BuiltinOpResolver resolver;
        tflite::InterpreterBuilder(*model, resolver)(&interp);
        interp->AllocateTensors();
        auto& ip=interp->input_tensor(0)->params; auto& op=interp->output_tensor(0)->params;
        input_scale=ip.scale; input_zero=ip.zero_point;
        output_scale=op.scale; output_zero=op.zero_point;
        std::cout << "✅ Wake word model loaded\n"; return true;
    }

    // Initialize PCEN state: M and mel_buffer to zeros.
    // Matches Python's StreamingPCEN.__init__:
    //   self.zi         = silence-warmed (= zeros for our config, see comment)
    //   self.mel_buffer = np.zeros((TARGET_WIDTH, N_MELS))
    //
    // Why "silence-warmed = zeros" here: Python warms zi by running PCEN over
    // 1 s of silence audio 5 times. With the mel * 2^31 scaling, silence input
    // yields E=0, so M decays/stays at 0 throughout — zi ends up at literal
    // zeros. The runtime warmup is a no-op for silence input, so we just
    // zero-initialize directly.
    void warmUp() {
        std::cout << "⏳ Warming up PCEN...\n";
        pcenState.M.assign(N_MELS, 0.0f);
        pcenState.mel_buffer.assign(TARGET_WIDTH, std::vector<float>(N_MELS, 0.0f));
        pcenState.lookback.assign(LOOKBACK, 0.0f);
        pcenState.initialized = true;
        std::cout << "✅ PCEN ready.\n";
    }

    // Reset = re-initialize to silence-warmed state.
    // Equivalent to Python's StreamingPCEN.reset().
    void reset() {
        warmUp();
    }

    // Always call this every stride — keeps PCEN M state current even during cooldown.
    std::vector<float> computeFeatures(const std::vector<float>& audioWindow) {
        auto feats = extractFeatures(audioWindow, pcenState);
        dumpTensorF32("wuw_pcen", feats);
        return feats;
    }
    // Runs TFLite on already-extracted features and returns all 3 class probs.
    // Inference now runs every stride (no cooldown skip) so EMAs stay continuous.
    WakeProbs inferFromFeatures(const std::vector<float>& features) {
        auto* input = interp->typed_input_tensor<int8_t>(0);
        for (size_t i=0;i<features.size();i++) {
            float q=features[i]/input_scale+input_zero;
            input[i]=(int8_t)std::max(-128.f,std::min(127.f,q));
        }
        interp->Invoke();
        int8_t* raw = interp->typed_output_tensor<int8_t>(0);
        WakeProbs p{
            (raw[0] - output_zero) * output_scale,
            (raw[1] - output_zero) * output_scale,
            (raw[2] - output_zero) * output_scale,
        };
        float probs[3] = {p.wake, p.other, p.noise};
        dumpTensorF32("wuw_dscnn", probs, 3);
        return p;
    }
};

// =========================================================
// SILERO VAD — loaded once, reset between detections
// =========================================================
struct SileroVAD {
    Ort::Env env; Ort::Session* session=nullptr;
    std::vector<float> state;  // shape [2, 1, 128] — v5 model
    SileroVAD():env(ORT_LOGGING_LEVEL_WARNING,"silero"){}
    bool init(const char* path) {
        Ort::SessionOptions opts; opts.SetIntraOpNumThreads(1);
        try {
            session=new Ort::Session(env,path,opts);
            state.assign(2*1*128,0.f);
            std::cout<<"✅ Silero VAD loaded\n"; return true;
        } catch(...) { std::cerr<<"VAD: failed to load\n"; return false; }
    }
    void reset() { std::fill(state.begin(),state.end(),0.f); }
    float process(const float* data,int len) {
        if (!session) return 1.0f;
        Ort::MemoryInfo mi=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        std::vector<float> audio(data,data+len);
        std::vector<int64_t> aShape={1,(int64_t)len}, sShape={2,1,128}, srShape={};
        int64_t srVal=16000;
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(mi,audio.data(),audio.size(),aShape.data(),2));
        inputs.push_back(Ort::Value::CreateTensor<float>(mi,state.data(),state.size(),sShape.data(),3));
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(mi,&srVal,1,srShape.data(),0));
        const char* in[]={"input","state","sr"}; const char* out[]={"output","stateN"};
        auto outputs=session->Run(Ort::RunOptions{nullptr},in,inputs.data(),3,out,2);
        float prob=outputs[0].GetTensorData<float>()[0];
        auto* sn=outputs[1].GetTensorData<float>();
        std::copy(sn,sn+state.size(),state.begin());
        return prob;
    }
    ~SileroVAD(){ delete session; }
    SileroVAD(const SileroVAD&)=delete;             // owns a raw Ort::Session*
    SileroVAD& operator=(const SileroVAD&)=delete;  // → forbid copy/move (double-free)
    SileroVAD(SileroVAD&&)=delete;
    SileroVAD& operator=(SileroVAD&&)=delete;
};

// =========================================================
// SERVER COMMAND CLIENT — STT + intent matching live on the server.
// Pipeline: record -> denoise -> VAD -> serialize to 16-bit PCM WAV
//        -> HTTP POST audio/wav to {SERVER_BASE_URL}{WAKE_CMD_ENDPOINT}
//        -> server returns JSON { "matched": "<Farsi command>" }
// =========================================================

// Build a minimal 16-bit PCM WAV blob from float audio in [-1, 1].
static std::vector<uint8_t> floatToWavPCM16(const std::vector<float>& audio, int sample_rate) {
    auto u32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x & 0xff);    v.push_back((x >> 8)  & 0xff);
        v.push_back((x >> 16) & 0xff); v.push_back((x >> 24) & 0xff);
    };
    auto u16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff);
    };
    const uint16_t channels = 1, bits = 16;
    const uint32_t byteRate = sample_rate * channels * (bits / 8);
    const uint16_t blockAlign = channels * (bits / 8);
    const uint32_t dataBytes = static_cast<uint32_t>(audio.size() * sizeof(int16_t));

    std::vector<uint8_t> wav;
    wav.reserve(44 + dataBytes);
    // RIFF header
    const char r[]="RIFF"; wav.insert(wav.end(), r, r+4);
    u32(wav, 36 + dataBytes);
    const char w[]="WAVE"; wav.insert(wav.end(), w, w+4);
    // fmt chunk
    const char f[]="fmt "; wav.insert(wav.end(), f, f+4);
    u32(wav, 16);             // PCM fmt chunk size
    u16(wav, 1);              // PCM
    u16(wav, channels);
    u32(wav, sample_rate);
    u32(wav, byteRate);
    u16(wav, blockAlign);
    u16(wav, bits);
    // data chunk
    const char d[]="data"; wav.insert(wav.end(), d, d+4);
    u32(wav, dataBytes);
    // PCM samples
    for (float s : audio) {
        float clamped = std::max(-1.0f, std::min(1.0f, s));
        int16_t q = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        wav.push_back(q & 0xff); wav.push_back((q >> 8) & 0xff);
    }
    return wav;
}

struct ServerCommandClient {
    std::string base_url;

    bool init(const char* base) {
        base_url = base;
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            std::cerr << "curl_global_init failed\n";
            return false;
        }
        std::cout << "✅ Server command client ready (target: " << base_url << ")\n";
        return true;
    }
    ~ServerCommandClient() { curl_global_cleanup(); }

    // ── Single endpoint the Pi cares about: POST the 5-s post-wake clip
    //    as audio/wav (with Device-ID + Authentication-Code headers so the
    //    server knows which device's reminders/inbox to touch). The server
    //    runs STT + intent match + routing; the Pi just checks HTTP 200.
    // Server's /Wake_Command response intent. The Pi acts on PILL (silences
    // the active alarm); CALL and MESSAGE are handled server-side via FCM.
    enum class WakeIntent { UNKNOWN, PILL, CALL, MESSAGE };

    bool sendWakeAudio(const std::vector<float>& audio,
                       WakeIntent* out_intent = nullptr) {
        std::cout << "📡 POST " << WAKE_CMD_ENDPOINT << "  ("
                  << audio.size() << " samples)\n";

        std::vector<uint8_t> wav = floatToWavPCM16(audio, SAMPLE_RATE);
        std::string url = base_url + WAKE_CMD_ENDPOINT;
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string id_hdr = std::string("Device-ID: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: audio/wav");
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, wav.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)wav.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        // Wake-command upload: 2s connect (fail fast if server unreachable so
        // we fall back / log instead of hanging) + 15s total (STT round-trip on
        // the server can take a few seconds; was HTTP_TIMEOUT_S=30 with no
        // connect cap, which could block the wake flow for the full 30s).
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](void* p, size_t s, size_t n, void* u) -> size_t {
                static_cast<std::string*>(u)->append((char*)p, s*n); return s*n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            std::cerr << "    HTTP error: " << curl_easy_strerror(rc) << "\n";
            return false;
        }
        if (http_code != 200) {
            std::cerr << "    Server returned HTTP " << http_code
                      << ": " << response.substr(0, 200) << "\n";
            return false;
        }
        std::cout << "    Server accepted the clip.\n";

        if (out_intent != nullptr) {
            *out_intent = WakeIntent::UNKNOWN;
            // Response body: {"intent":"PILL|CALL|MESSAGE","transcript":...,"message_id":...}
            auto pos = response.find("\"intent\"");
            if (pos != std::string::npos) {
                auto q1 = response.find('"', pos + 8);
                if (q1 != std::string::npos) {
                    auto q2 = response.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        std::string val = response.substr(q1 + 1, q2 - q1 - 1);
                        if      (val == "PILL")    *out_intent = WakeIntent::PILL;
                        else if (val == "CALL")    *out_intent = WakeIntent::CALL;
                        else if (val == "MESSAGE") *out_intent = WakeIntent::MESSAGE;
                    }
                }
            }
        }
        return true;
    }

    // Parsed /Live_Device response. All fields default to "nothing to do".
    struct LiveStatus {
        int  image_req_id   = -1;       // -1 if no snapshot requested
        bool call_pending   = false;    // app posted a Call_Offer
        int  guest_cmd      = -1;       // app guest toggle: -1 none, 0 wake, 1 sleep
        // Active pill alarms — server has computed which pills are currently
        // due for this device based on schedule + last_ack_at.
        struct PillAlarm {
            int    id      = 0;          // server-side pill_schedules.id
            int    pill_id = 0;          // app's local pill_id
            std::string name;
            std::string audio_url;       // relative path, prepend base_url
        };
        std::vector<PillAlarm> pill_alarms;
        std::string voice_msg_url;   // one-shot caregiver voice note (empty = none)
    };

    // ── /Live_Device heartbeat. Returns parsed status struct. Mirrors
    //    Python's send_live_packet behavior. Quick GET, ~10–30 ms on LAN.
    LiveStatus sendLivePacket() {
        LiveStatus st;
        std::string url = base_url + LIVE_ENDPOINT;
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return st;

        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        std::string mac_hdr = std::string("Device-Mac: ") + g_mac;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());
        if (!g_mac.empty()) hdrs = curl_slist_append(hdrs, mac_hdr.c_str());
        // Tell the server we're in guest/sleep mode so the app can show a
        // "sleeping / not monitoring" badge (caregiver must not be misled into
        // thinking falls are watched while the device is paused).
        hdrs = curl_slist_append(hdrs,
            g_guest_mode.load() ? "Guest-Mode: 1" : "Guest-Mode: 0");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        // Heartbeat must fail FAST — it drives pill alarms / calls / snapshots
        // on the housekeeping thread, so a transient network hiccup must never
        // stall that loop. 1s connect + 2s total: a dead/slow server costs us
        // at most ~2s instead of blocking pickup/playback for seconds.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);  // fail fast if server unreachable
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);  // don't re-resolve mDNS each beat
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](void* p, size_t s, size_t n, void* u) -> size_t {
                static_cast<std::string*>(u)->append((char*)p, s*n); return s*n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK || http_code != 200) {
            // Throttle the log so a dead server doesn't flood stdout (heartbeat
            // fires every 100ms). One line per second per distinct failure.
            static auto      last_log = std::chrono::steady_clock::time_point{};
            static long      last_code = 0;
            static CURLcode  last_rc   = CURLE_OK;
            auto now = std::chrono::steady_clock::now();
            if (rc != last_rc || http_code != last_code ||
                now - last_log > std::chrono::seconds(1)) {
                std::fprintf(stderr,
                    "[heartbeat] %s/Live_Device — curl=%d (%s) http=%ld\n",
                    base_url.c_str(), rc, curl_easy_strerror(rc), http_code);
                last_log = now; last_code = http_code; last_rc = rc;
            }
            return st;
        }

        // Minimal JSON parse — we know the exact shape from myserver.py and
        // each field is a primitive or a flat object. Avoids dragging in a
        // full JSON library for ~4 fields.
        auto get_field = [&](const std::string& key,
                             std::string::size_type from = 0) -> std::string::size_type {
            return response.find("\"" + key + "\"", from);
        };

        // image_req / image_req_id
        if (auto p = get_field("image_req"); p != std::string::npos) {
            if (response.find("true", p) != std::string::npos &&
                response.find("true", p) - p < 30) {
                if (auto id_p = get_field("image_req_id"); id_p != std::string::npos) {
                    auto colon = response.find(':', id_p);
                    if (colon != std::string::npos) {
                        // The value may be a JSON number (139610079) or a
                        // quoted string ("139610079") depending on the server.
                        // Skip past whitespace/quotes to the first digit so
                        // std::stoi doesn't choke on a leading quote.
                        size_t vs = colon + 1;
                        while (vs < response.size() &&
                               !std::isdigit(static_cast<unsigned char>(response[vs])))
                            ++vs;
                        try { st.image_req_id = std::stoi(response.substr(vs)); }
                        catch (...) {}
                    }
                }
            }
        }

        // call_pending — true/false primitive
        if (auto p = get_field("call_pending"); p != std::string::npos) {
            auto colon = response.find(':', p);
            if (colon != std::string::npos &&
                response.find("true", colon) - colon < 30) {
                st.call_pending = true;
            }
        }

        // guest_set — one-shot remote guest command: true=sleep, false=wake,
        // null=none. App toggles it via /Set_Guest_App; server consumes-on-read.
        if (auto p = get_field("guest_set"); p != std::string::npos) {
            auto colon = response.find(':', p);
            if (colon != std::string::npos) {
                auto t = response.find("true",  colon);
                auto f = response.find("false", colon);
                if      (t != std::string::npos && t - colon < 12) st.guest_cmd = 1;
                else if (f != std::string::npos && f - colon < 12) st.guest_cmd = 0;
            }
        }

        // voice_msg_url — one-shot caregiver voice note URL (string) or null.
        if (auto p = get_field("voice_msg_url"); p != std::string::npos) {
            auto colon = response.find(':', p);
            if (colon != std::string::npos) {
                auto q1 = response.find('"', colon);
                auto nullp = response.find("null", colon);
                if (q1 != std::string::npos &&
                    (nullp == std::string::npos || q1 < nullp)) {
                    auto q2 = response.find('"', q1 + 1);
                    if (q2 != std::string::npos)
                        st.voice_msg_url = response.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        // active_pill_alarms — array of objects. Tiny manual walker since
        // each object has fixed fields (id, pill_id, name, audio_url).
        if (auto arr_p = get_field("active_pill_alarms"); arr_p != std::string::npos) {
            auto br_open  = response.find('[', arr_p);
            auto br_close = response.find(']', br_open);
            if (br_open != std::string::npos && br_close != std::string::npos) {
                std::string arr = response.substr(br_open + 1, br_close - br_open - 1);
                size_t cur = 0;
                while ((cur = arr.find('{', cur)) != std::string::npos) {
                    size_t end = arr.find('}', cur);
                    if (end == std::string::npos) break;
                    std::string obj = arr.substr(cur, end - cur);
                    LiveStatus::PillAlarm pa;
                    auto extract_int = [&](const char* key) -> int {
                        auto kp = obj.find(std::string("\"") + key + "\"");
                        if (kp == std::string::npos) return 0;
                        auto col = obj.find(':', kp);
                        if (col == std::string::npos) return 0;
                        try { return std::stoi(obj.substr(col + 1)); }
                        catch (...) { return 0; }
                    };
                    auto extract_str = [&](const char* key) -> std::string {
                        auto kp = obj.find(std::string("\"") + key + "\"");
                        if (kp == std::string::npos) return {};
                        auto col = obj.find(':', kp);
                        if (col == std::string::npos) return {};
                        auto q1 = obj.find('"', col);
                        auto q2 = obj.find('"', q1 + 1);
                        if (q1 == std::string::npos || q2 == std::string::npos) return {};
                        return obj.substr(q1 + 1, q2 - q1 - 1);
                    };
                    pa.id        = extract_int("id");
                    pa.pill_id   = extract_int("pill_id");
                    pa.name      = extract_str("name");
                    pa.audio_url = extract_str("audio_url");
                    if (pa.id > 0) st.pill_alarms.push_back(pa);
                    cur = end + 1;
                }
            }
        }
        return st;
    }

    // ── POST /Pill_Ack_Device when wake-fire returns PILL intent.
    bool sendPillAck(int pill_schedule_id = -1) {
        std::string url = base_url + "/Pill_Ack_Device";
        std::string body = pill_schedule_id >= 0
            ? std::string("{\"pill_schedule_id\":") + std::to_string(pill_schedule_id) + "}"
            : "{}";
        std::string response;
        CURL* curl = curl_easy_init(); if (!curl) return false;
        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK && code == 200;
    }

    // ── Fetch a pill audio file by its server-side id; save to /tmp.
    //    Returns the local file path on success, empty on failure.
    std::string fetchPillAudio(int pill_schedule_id) {
        std::string url = base_url + "/Pill_Audio/" + std::to_string(pill_schedule_id);
        std::string out_path = "/tmp/pill_" + std::to_string(pill_schedule_id) + ".wav";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) return {};
        CURL* curl = curl_easy_init(); if (!curl) { std::fclose(fp); return {}; }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // /Pill_Audio is device-authenticated server-side; send the same
        // Device-Id + Key-Hash headers the other device calls use.
        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](void* p, size_t s, size_t n, void* u) -> size_t {
                return std::fwrite(p, s, n, static_cast<FILE*>(u));
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        // Pill audio can be ~1MB so keep the 30s total, but bail fast if the
        // server is unreachable (5s connect) and abort a STALLED transfer
        // (< 1KB/s for 10s) instead of hanging the whole 30s.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        std::fclose(fp);
        if (rc != CURLE_OK || code != 200) {
            std::remove(out_path.c_str());
            return {};
        }
        return out_path;
    }

    // ── Fetch the one-shot caregiver voice note; save to /tmp. Empty on fail.
    std::string fetchVoiceMsg(const std::string& url_path) {
        std::string url = base_url + url_path;
        std::string out_path = "/tmp/voice_msg.wav";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) return {};
        CURL* curl = curl_easy_init(); if (!curl) { std::fclose(fp); return {}; }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](void* p, size_t s, size_t n, void* u) -> size_t {
                return std::fwrite(p, s, n, static_cast<FILE*>(u));
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        // Voice note can be ~1MB → keep 30s total, but 5s connect + stalled-
        // transfer abort (< 1KB/s for 10s) so a flaky network can't hang it.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        std::fclose(fp);
        if (rc != CURLE_OK || code != 200) { std::remove(out_path.c_str()); return {}; }
        return out_path;
    }

    // ── POST a JPEG snapshot to /Message_Device with Message Type
    //    "image captured" — mirrors Python's send_image_message.
    bool sendImageMessage(int image_req_id,
                          const std::vector<uint8_t>& jpeg) {
        return postMessageDevice("image captured", image_req_id, jpeg, {});
    }

    // ── POST a fall event (thumbnail + A/V clip) to /Message_Device. The
    //    danger level comes from the pain feedback (0=fine/green, 1=unclear/
    //    orange, 2=pain/red) and drives the notification + app card colour.
    bool sendEventPacket(const std::vector<uint8_t>& jpeg,
                         const std::vector<uint8_t>& mp4,
                         int danger_level) {
        return postMessageDevice("Event", -1, jpeg, mp4, danger_level);
    }

    // ── Replace the newest fall Event's video with the finished A/V clip
    //    (already E2E-encrypted by the caller). Device-authenticated multipart.
    bool postFallClipReplace(const std::vector<uint8_t>& enc_clip) {
        if (enc_clip.empty()) return false;
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "video file");
        curl_mime_filename(part, "fall_av.mp4");
        curl_mime_data(part, reinterpret_cast<const char*>(enc_clip.data()),
                       enc_clip.size());
        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());
        std::string url = base_url + "/Fall_Clip_Replace_Device";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        // Fall-clip upload: keep 30s total (clip can be large), but fail fast on
        // unreachable server (5s connect) and abort a stalled upload
        // (< 1KB/s for 15s) so a flaky network can't hang it indefinitely.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK && code == 200;
    }

private:
    // Shared multipart POST to /Message_Device. mp4 may be empty (image-only).
    // danger_level (Event only): 0/1/2 → green/orange/red, from pain feedback.
    bool postMessageDevice(const std::string& msg_type,
                           int image_req_id,
                           const std::vector<uint8_t>& jpeg,
                           const std::vector<uint8_t>& mp4,
                           int danger_level = 2) {
        std::string url = base_url + MSG_ENDPOINT;
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // Build the text part JSON.
        // {"Message Type":"...", "image_req_id":<n>, "Message Time":"<iso>"}
        // For "Event" type, Python uses "Event Title" + "Danger Level" — we
        // include both to match the app's existing parser.
        char tbuf[64];
        std::time_t now = std::time(nullptr);
        struct tm tmv;  // localtime_r — std::localtime's static buffer races T4/T5
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tmv));
        // ── Phase 3.1 E2E media crypto ──────────────────────────────────
        // Encrypt the image/video with the device password (shared only with
        // subscribed phones) before upload. The server is a blind relay and
        // stores/serves the ciphertext. The app decrypts with the same
        // password from DeviceSecretStore. We set "encrypted":true so the app
        // knows to decrypt; if encryption is unavailable (no libsodium / no
        // password) we fall back to plaintext + "encrypted":false.
        std::vector<uint8_t> jpeg_out = jpeg, mp4_out = mp4;
        bool encrypted = false;
        if (!g_password.empty()) {
            std::string cerr_msg;
            std::vector<uint8_t> ej, em;
            if (!jpeg.empty())
                ej = mc::encrypt_for_subscriber(g_device_id, g_password, jpeg, &cerr_msg);
            bool mp4_ok = true;
            if (!mp4.empty()) {
                em = mc::encrypt_for_subscriber(g_device_id, g_password, mp4, &cerr_msg);
                mp4_ok = !em.empty();
            }
            // Only flip to encrypted if every present part encrypted cleanly.
            if ((jpeg.empty() || !ej.empty()) && (mp4.empty() || mp4_ok)
                && (!jpeg.empty() || !mp4.empty())) {
                if (!jpeg.empty()) jpeg_out = std::move(ej);
                if (!mp4.empty())  mp4_out  = std::move(em);
                encrypted = true;
            } else if (!cerr_msg.empty()) {
                std::cerr << "    [msg] media encrypt failed (" << cerr_msg
                          << ") — sending plaintext\n";
            }
        }

        std::string json = "{\"Message Type\":\"" + msg_type + "\"";
        if (image_req_id >= 0) {
            json += ",\"image_req_id\":" + std::to_string(image_req_id);
        }
        if (msg_type == "Event") {
            // Danger Level from pain feedback: 0 green / 1 orange / 2 red
            // (matches devices.dart + server fanout colour maps).
            json += ",\"Event Title\":\"Fall Detection\",\"Danger Level\":"
                  + std::to_string(danger_level);
        }
        json += ",\"encrypted\":" + std::string(encrypted ? "true" : "false");
        json += ",\"Message Time\":\"" + std::string(tbuf) + "\"}";

        std::string id_hdr = std::string("Device-Id: ") + DEVICE_ID;
        std::string ac_hdr = std::string("Key-Hash: ") + KEY_HASH;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, id_hdr.c_str());
        hdrs = curl_slist_append(hdrs, ac_hdr.c_str());

        curl_mime* form = curl_mime_init(curl);
        // text part (JSON)
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, "text");
        curl_mime_data(part, json.c_str(), json.size());
        curl_mime_type(part, "application/json");
        // image file (JPEG, or its ciphertext when encrypted)
        if (!jpeg_out.empty()) {
            part = curl_mime_addpart(form);
            curl_mime_name(part, "image file");
            curl_mime_filename(part, msg_type == "Event" ? "event_image.jpg" : "image.jpg");
            curl_mime_data(part, reinterpret_cast<const char*>(jpeg_out.data()), jpeg_out.size());
            curl_mime_type(part, encrypted ? "application/octet-stream" : "image/jpeg");
        }
        // video file (MP4, or its ciphertext when encrypted) — only on event
        if (!mp4_out.empty()) {
            part = curl_mime_addpart(form);
            curl_mime_name(part, "video file");
            curl_mime_filename(part, "event_video.mp4");
            curl_mime_data(part, reinterpret_cast<const char*>(mp4_out.data()), mp4_out.size());
            curl_mime_type(part, encrypted ? "application/octet-stream" : "video/mp4");
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
        // Snapshot / fall-event upload (image, or image+A/V clip): keep 30s
        // total (the clip can be large), but fail fast on unreachable server
        // (5s connect) and abort a stalled upload (< 1KB/s for 15s) so a flaky
        // network can't hang it. Snapshot uploads run on a detached thread (see
        // housekeepingThread) so this never blocks the heartbeat either way.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](void* p, size_t s, size_t n, void* u) -> size_t {
                static_cast<std::string*>(u)->append((char*)p, s*n); return s*n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_mime_free(form);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            std::cerr << "    [msg] HTTP error: " << curl_easy_strerror(rc) << "\n";
            return false;
        }
        if (http_code != 200) {
            std::cerr << "    [msg] HTTP " << http_code << ": "
                      << response.substr(0, 200) << "\n";
            return false;
        }
        return true;
    }
};

// =========================================================
// ALARM
// =========================================================
void playAlarm() {
    const int   ALARM_RATE     = 16000;
    const int   ALARM_CHANNELS = 1;
    const float ALARM_HZ       = 880.0f;  // tone frequency
    const float ALARM_SECS     = 0.3f;    // duration
    int nSamples = (int)(ALARM_RATE * ALARM_SECS);

    std::vector<int16_t> buf(nSamples);
    for (int i = 0; i < nSamples; i++) {
        float t = (float)i / ALARM_RATE;
        float env = std::min(t / 0.01f, 1.0f) * std::min((ALARM_SECS - t) / 0.01f, 1.0f); // fade in/out
        buf[i] = (int16_t)(env * 28000.0f * std::sin(2.0f * M_PI * ALARM_HZ * t));
    }

    snd_pcm_t* handle;
    snd_pcm_hw_params_t* params;
    if (snd_pcm_open(&handle, ALSA_PLAYBACK_DEV, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        std::cerr << "ALARM: failed to open playback (" << ALSA_PLAYBACK_DEV << ")\n"; return;
    }
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, params, ALARM_CHANNELS);
    unsigned int rate = ALARM_RATE;
    snd_pcm_hw_params_set_rate_near(handle, params, &rate, nullptr);
    // If hw_params fails the handle is left misconfigured and snd_pcm_writei
    // would spin in an error loop, wedging the calling (WUW) thread. Bail.
    if (snd_pcm_hw_params(handle, params) < 0) {
        std::cerr << "ALARM: snd_pcm_hw_params failed; aborting playback\n";
        snd_pcm_close(handle);
        return;
    }
    snd_pcm_writei(handle, buf.data(), nSamples);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
}

// =========================================================
// HELPERS
// =========================================================
// LED status helper. RGB LED on user's wiring:
//   BCM 7  = Red
//   BCM 1  = Green
//   BCM 12 = Blue
//   common cathode (anode in middle pin tied to GND)
//
// Driven via libgpiod v2 (gpio_io.cpp). Pi OS Trixie removed the legacy
// /sys/class/gpio sysfs, so the previous export/value approach no longer
// worked. gpioio::init() is shared with the button; on a dev box without
// the chip it's a no-op and setLed just prints.
constexpr int LED_R_BCM = 7;
constexpr int LED_G_BCM = 1;
constexpr int LED_B_BCM = 12;

enum class LedColor { GREEN, RED, BLUE, OFF };

void setLed(LedColor c) {
    const char* name = (c == LedColor::GREEN) ? "GREEN"
                     : (c == LedColor::RED)   ? "RED"
                     : (c == LedColor::BLUE)  ? "BLUE" : "OFF";
    std::cout << "[LED] " << name << "\n";

    gpioio::init();   // idempotent; no-op if already up or unavailable
    // common-cathode → channel lit when true
    gpioio::led(c == LedColor::RED, c == LedColor::GREEN, c == LedColor::BLUE);
}

// LED scheme:
//   • normal idle      = BLUE   (device on + working, listening)
//   • recording audio  = GREEN  (wake command after "Noban"/"Hey Noban", and
//                                the post-fall "speak now" recording)
//   • guest/sleep idle = RED    (privacy mode — caregiver can tell at a glance)
//   • BLE setup        = blinking BLUE (see runWifiSetup / LedBlinker)
// g_guest_mode is declared earlier.
LedColor idleColor() { return g_guest_mode.load() ? LedColor::RED : LedColor::BLUE; }
LedColor recColor()  { return LedColor::GREEN; }

// ── OLED interactive "eyes" ────────────────────────────────────────────────
// The eyes are drawn by a separate persistent daemon (oled_eyes.py) that polls
// a tiny state file; the pipeline just writes the current state here at each
// transition. States:
//   "none"    blank screen — unprovisioned / BLE setup / factory reset / reboot
//   "idle"    open eyes (provisioned, listening)
//   "excited" heard its name "Noban" (normal wake) — big sparkly eyes
//   "happy"   warm closed-eye smile — used for greetings (greet / wakegreet)
//   "listen"  woke from sleep on "Hey Noban" (guest) — curious scan
//   "sleep"   guest/sleep mode (eyes closed)
//   "worried" post-fall "speak now" recording
//   "greet"     first-ever setup: daemon shows happy ~30s, then idle
//   "wakegreet" guest→active: daemon shows idle 2s, happy 10s, then idle
static void setOled(const char* state) {
    const char* f = std::getenv("OLED_STATE_FILE");
    std::string path = f ? f : "/tmp/noban_oled_state";
    std::string tmp  = path + ".tmp";
    std::ofstream o(tmp, std::ios::trunc);
    if (o) {
        o << state << "\n";
        o.close();
        std::rename(tmp.c_str(), path.c_str());   // atomic swap for the reader
    }
    std::cout << "[OLED] " << state << "\n";
}

// "First setup" greeting marker. The very first provisioned boot shows a 30s
// happy greeting (the daemon resolves the "greet" meta-state); we drop a marker
// so later boots start calm. Factory reset wipes the marker so a re-onboarded
// device greets again. Tries /var/lib/elderly-care (setup chowns it to pi) then
// ~/.config/elderly-care — the same two locations as id.txt.
static std::string greetMarkerPath() {
    return "/var/lib/elderly-care/.greeted";
}
static bool hasGreeted() {
    if (std::ifstream("/var/lib/elderly-care/.greeted").good()) return true;
    const char* home = std::getenv("HOME");
    if (home && std::ifstream(std::string(home) + "/.config/elderly-care/.greeted").good())
        return true;
    return false;
}
static void markGreeted() {
    const char* home = std::getenv("HOME");
    for (const std::string& p : {std::string("/var/lib/elderly-care/.greeted"),
                                 home ? std::string(home) + "/.config/elderly-care/.greeted"
                                      : std::string()}) {
        if (p.empty()) continue;
        std::ofstream o(p, std::ios::trunc);
        if (o) { o << "1\n"; return; }
    }
}

// Blink the LED a colour on a background thread until destroyed. Used to show
// "blinking blue" while BLE/Wi-Fi setup runs (runWifiSetup blocks on the
// sidecar, so a thread drives the blink).
struct LedBlinker {
    std::atomic<bool> stop_{false};
    std::thread th_;
    void start(LedColor c) {
        th_ = std::thread([this, c] {
            bool on = true;
            while (!stop_.load()) {
                setLed(on ? c : LedColor::OFF);
                on = !on;
                for (int i = 0; i < 5 && !stop_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
        });
    }
    ~LedBlinker() { stop_.store(true); if (th_.joinable()) th_.join(); }
};

// recordCommand pulls RECORD_SECONDS of post-wake audio from the AudioRing in
// ~100ms slices so the caller can preempt mid-record (the fall-preempts-wake
// rule). `should_abort` is polled between slices; if it returns true, recording
// stops and whatever has been captured so far is returned. No ALSA handle of
// its own — the dedicated capture thread (T1) owns micleft; we just read the
// stream it's already producing, starting at "now" (the moment after the wake).
std::vector<float> recordCommand(AudioRing& ring, const std::atomic<bool>& running,
                                 const std::function<bool()>& should_abort = []{ return false; }) {
    std::cout<<"\n🎙️  Recording command ("<<RECORD_SECONDS<<"s)...\n";
    const size_t total = static_cast<size_t>(SAMPLE_RATE) * RECORD_SECONDS;
    const size_t hop   = SAMPLE_RATE / 10;          // 100ms — preemption granularity
    std::vector<float> audio; audio.reserve(total);
    uint64_t cursor = ring.now();                   // start fresh (post-wake)
    std::vector<float> chunk;
    while (audio.size() < total) {
        if (should_abort()) {
            std::cout << "    Recording aborted at " << audio.size() << " samples.\n";
            break;
        }
        size_t want = std::min(hop, total - audio.size());
        uint64_t nc = ring.read(cursor, want, chunk, running);
        if (chunk.empty()) {            // 500ms timeout (producer idle) or stopping
            if (!running.load()) break;
            continue;
        }
        cursor = nc;
        audio.insert(audio.end(), chunk.begin(), chunk.end());
    }
    std::cout<<"    Done ("<<audio.size()<<" samples).\n"; return audio;
}

std::vector<float> denoise(const std::vector<float>& audio) {
    std::cout<<"🔇 Denoising...\n";
    DenoiseState* st=rnnoise_create(nullptr);
    std::vector<float> out(audio.size(),0.0f);
    size_t i=0;
    while (i+RNNOISE_FRAMES<=audio.size()) {
        float frame[RNNOISE_FRAMES];
        for (int j=0;j<RNNOISE_FRAMES;j++) frame[j]=audio[i+j]*32768.0f;
        rnnoise_process_frame(st,frame,frame);
        for (int j=0;j<RNNOISE_FRAMES;j++) out[i+j]=frame[j]/32768.0f;
        i+=RNNOISE_FRAMES;
    }
    rnnoise_destroy(st); return out;
}

std::vector<float> applyVAD(const std::vector<float>& audio, SileroVAD& vad) {
    std::cout<<"🔍 Applying VAD...\n"; vad.reset();

    // Trim-only mode: find first and last speech windows, keep the contiguous
    // span between them (with padding). Avoids chopping quiet phonemes /
    // brief inter-syllable silences out of the middle of the utterance.
    const int PAD_WINDOWS = 5;            // ~160 ms padding on each end

    std::vector<float> probs;
    for (size_t i=0; i+VAD_WINDOW<=audio.size(); i+=VAD_WINDOW) {
        probs.push_back(vad.process(audio.data()+i, VAD_WINDOW));
    }

    int first = -1, last = -1;
    for (int w=0; w<(int)probs.size(); w++) {
        if (probs[w] >= VAD_THRESHOLD) {
            if (first < 0) first = w;
            last = w;
        }
    }

    if (first < 0) {
        std::cout<<"    No speech detected — using full audio.\n";
        return audio;
    }

    first = std::max(0, first - PAD_WINDOWS);
    last  = std::min((int)probs.size() - 1, last + PAD_WINDOWS);

    size_t start_sample = (size_t)first * VAD_WINDOW;
    size_t end_sample   = std::min(audio.size(), (size_t)(last + 1) * VAD_WINDOW);

    int kept_ms = (int)((end_sample - start_sample) * 1000 / SAMPLE_RATE);
    int total_ms = (int)(audio.size() * 1000 / SAMPLE_RATE);
    std::cout<<"    Kept "<<kept_ms<<"/"<<total_ms<<" ms ("
             <<(last-first+1)<<"/"<<probs.size()<<" windows).\n";

    return std::vector<float>(audio.begin() + start_sample,
                              audio.begin() + end_sample);
}

// (No local dispatcher — the server routes the audio. Pi is fire-and-forget
//  past this point.)

// =========================================================
// FALL-STATE WATCHER — minimal stdin-driven toggle for testing.
// =========================================================
// CAMERA + FALL DETECTION pipeline (single thread)
//
// One thread does both capture and processing. Capture pauses during
// inference, but inference is ~20ms typical (1000ms inter-batch budget
// at 16fps), so no frames are dropped in the normal path. Worst-case
// (suspected-fall trigger with 16 MoVeNet calls) is ~1s of processing,
// during which capture pauses — those frames are missed, but by then
// we're confirming a fall anyway and the pain session is about to take
// over, so the gap is harmless.
//
// Camera config matches all_together.py: 640x480 @ 16fps, BGR.
// Python flips frames vertically (cv2.flip(frame, 0)) — we mirror.
// =========================================================
constexpr int CAM_W            = 640;
constexpr int CAM_H            = 480;
constexpr int CAM_FPS          = 16;
constexpr int EVENT_CLIP_SEC   = 5;                          // mirrors Python
constexpr int EVENT_CLIP_FRAMES = EVENT_CLIP_SEC * CAM_FPS;  // 80 frames
constexpr auto HEARTBEAT_PERIOD = std::chrono::milliseconds(100);

// Set to true by fallPipelineThread when an incoming /Call_Offer is being
// answered by the aiortc helper. While set, main()'s WUW loop pauses (it
// needs to release the mic for the helper) and the fall pipeline skips
// inference too. Cleared when the helper exits.
std::atomic<bool> g_in_call{false};

// Set by audioCaptureThread (T1) when it has CLOSED its micleft handle because
// a call or a pain session needs the device exclusively; cleared when it
// reopens. The mic owner before opening (pain session in S1, the call helper)
// waits on this so there is never a two-opener race on micleft. The audio
// thread releases the mic while (g_in_call || g_fall).
std::atomic<bool> g_mic_free{false};

// Pain feedback → fall thread handoff: the WUW+pain thread (T3) stores the
// danger level derived from the pain SessionResult (0 fine/green, 1 unclear/
// orange, 2 pain/red) BEFORE clearing g_fall; the fall thread (T4) reads it to
// stamp the fall event it sends after the session. -1 = not yet decided.
std::atomic<int> g_fall_danger_level{-1};

// ── Wi-Fi setup button (GPIO BCM 24 — match Python's `all_together.py`) ──
// Polled inside fallPipelineThread (same loop, ~62 ms cadence is fine for
// debounce). Held LOW = pressed (pull-up to 3V3). On a confirmed press we
// shell out to wifi_setup.sh which brings up an AP and a tiny HTTP server;
// see wifi_setup.sh + wifi_setup_server.py for the provisioning flow.
constexpr int BUTTON_BCM_PIN   = 17;   // user-wired on Pi: BCM17 (physical pin 11)
constexpr auto BUTTON_DEBOUNCE = std::chrono::milliseconds(50);

// On a laptop / dev machine where the GPIO pin doesn't exist, you can
// simulate a button press by sending the process SIGUSR1:
//     kill -USR1 $(pgrep -f pipeline)
// The flag below is set by the signal handler and consumed by readButton().
static std::atomic<bool> g_button_simulated{false};
extern "C" void on_sigusr1(int) { g_button_simulated.store(true); }

// Graceful shutdown. systemd `systemctl stop` sends SIGTERM; the default action
// is immediate terminate, which skips thread cleanup and can leave ALSA/GPIO in
// a bad state. Instead we set the run flag so every thread breaks its loop and
// main() joins them cleanly. Only an atomic store happens here — async-signal-safe.
static std::atomic<bool>* g_run_flag = nullptr;
extern "C" void on_term(int) { if (g_run_flag) g_run_flag->store(false); }

// Try to read the button via the sysfs GPIO interface (universally available
// on Pi OS without extra packages). Returns true if pressed.
//
// On a laptop the sysfs export will silently fail (no /sys/class/gpio); we
// detect that on first call and switch to "signal-only" mode so the same
// binary works on both platforms. Press behavior on laptop: send SIGUSR1.
static bool readButton() {
    enum class Mode { UNKNOWN, GPIOD, SIGNAL_ONLY };
    static Mode mode = Mode::UNKNOWN;

    if (mode == Mode::UNKNOWN) {
        // Pi OS Trixie dropped /sys/class/gpio; use libgpiod v2 (char-dev).
        if (gpioio::init()) {
            mode = Mode::GPIOD;
        } else {
            mode = Mode::SIGNAL_ONLY;
            std::signal(SIGUSR1, on_sigusr1);
            std::cout << "[button] no libgpiod chip — using SIGUSR1 mode "
                         "(send `kill -USR1 " << getpid()
                      << "` to simulate a press)\n";
        }
    }

    // Signal-driven press takes priority — handy for forcing a press in
    // either mode (e.g. remote test of the Pi without physical access).
    if (g_button_simulated.exchange(false)) return true;
    if (mode != Mode::GPIOD) return false;
    return gpioio::button_pressed();   // active-low handled inside
}

// ── Multi-mode button state machine ────────────────────────────────────────
// Called every T5 tick (~30 ms). One physical button, mode-aware.
//
// AWAKE (normal monitoring):
//   3 rapid taps              → ENTER_GUEST (privacy/sleep — pause everything)
//   5 rapid taps              → FACTORY_RESET
//   hold 3-9.99 s             → ENTER_SETUP (BLE provisioning)
//   hold ≥10 s                → REBOOT (a Pi has no soft power-on)
//   1 / 2 / 4 taps            → nothing
//   Tap-bursts dispatch on SETTLE: we wait BURST_SETTLE after the last tap,
//   then act on the FINAL count — so a 3-tap (guest) is never mistaken for the
//   prefix of a 5-tap (factory) burst.
//
// ASLEEP (guest mode):
//   any press                 → WAKE (resume). Consumed — does NOT count toward
//                               a burst or trigger a hold gesture.
//
// LED feedback while holding (AWAKE only):
//   0-3 s : nothing    3-10 s : blue ("release→setup")    ≥10 s : red ("release→reboot")
//
// `tick(pressed_now, asleep)` must be polled at ≥10 Hz. Returns a ButtonGesture
// for the caller to dispatch (kept inline next to action helpers).
enum class ButtonGesture { NONE, ENTER_SETUP, REBOOT, FACTORY_RESET, ENTER_GUEST, WAKE };

class ButtonStateMachine {
public:
    using Clock = std::chrono::steady_clock;

    ButtonGesture tick(bool pressed_now, bool asleep) {
        auto now = Clock::now();
        // How long taps must stop before a burst is dispatched (3→guest, 5→reset).
        constexpr auto BURST_SETTLE = std::chrono::milliseconds(700);

        // ── Edge detection
        if (pressed_now && !pressed_) {       // press-down
            press_start_ = now;
            led_phase_ = 0;
            pressed_ = true;
        } else if (!pressed_now && pressed_) { // release
            auto held = now - press_start_;
            pressed_ = false;

            // ASLEEP: any release wakes the device. Consume it (no burst/hold).
            if (asleep) {
                tap_times_.clear();
                return ButtonGesture::WAKE;
            }

            // Restore LED to idle (RED/BLUE hold feedback ends). Mode-aware so a
            // button press in guest mode doesn't flip the LED to green.
            setLed(idleColor());

            // Holds dispatch immediately on release.
            // REBOOT if held ≥10s (a Pi has no soft power-on, so we reboot
            // instead of halting — the device comes back up on its own).
            if (held >= std::chrono::seconds(10)) {
                tap_times_.clear();
                return ButtonGesture::REBOOT;
            }
            // ENTER_SETUP if held the medium window.
            if (held >= std::chrono::seconds(3)) {
                tap_times_.clear();
                return ButtonGesture::ENTER_SETUP;
            }
            // Short tap — feeds the burst counter. We DON'T dispatch here; the
            // settle check below acts once tapping stops, so 3 (guest) is never
            // read as the prefix of a 5 (factory-reset) burst.
            tap_times_.push_back(now);
            while (!tap_times_.empty() &&
                   now - tap_times_.front() > std::chrono::seconds(3)) {
                tap_times_.pop_front();
            }
            return ButtonGesture::NONE;
        }

        // ── Live LED feedback while held (awake only)
        if (pressed_ && !asleep) {
            auto held = now - press_start_;
            int phase = 0;
            if (held >= std::chrono::seconds(10)) phase = 2;
            else if (held >= std::chrono::seconds(3)) phase = 1;
            if (phase != led_phase_) {
                led_phase_ = phase;
                if (phase == 1) setLed(LedColor::BLUE);   // "release to setup"
                if (phase == 2) setLed(LedColor::RED);    // "release to reboot"
            }
        }

        // ── Burst settle dispatch (awake only). Once tapping has stopped for
        // BURST_SETTLE, act on the FINAL count: 3 → guest, ≥5 → factory reset.
        if (!pressed_ && !asleep && !tap_times_.empty() &&
            now - tap_times_.back() >= BURST_SETTLE) {
            size_t n = tap_times_.size();
            tap_times_.clear();
            if (n >= 5) return ButtonGesture::FACTORY_RESET;
            if (n == 3) return ButtonGesture::ENTER_GUEST;
            return ButtonGesture::NONE;       // 1 / 2 / 4 taps → nothing
        }
        return ButtonGesture::NONE;
    }

private:
    bool pressed_ = false;
    Clock::time_point press_start_;
    std::deque<Clock::time_point> tap_times_;
    int led_phase_ = 0;
};

// Wipe id.txt + NetworkManager Wi-Fi config + drop a marker so the next
// boot enters BLE setup mode. Called on 5-rapid-taps. Best-effort: if
// any step fails the device still boots into factory mode because id.txt
// is gone, and loadDeviceIdentity() falls back to FACTORY_ID="Noban".
static void doFactoryReset() {
    std::cout << "\n[button] FACTORY RESET requested — confirming purge with server FIRST\n";
    setLed(LedColor::RED);
    setOled("none");   // maintenance action → no eyes (restored below only if aborted)
    // Wipe the local identity ONLY AFTER the server confirms it purged this
    // device's data. The device loses its creds on wipe and can't retry, so a
    // flaky network here would otherwise orphan everything server-side. We retry
    // with backoff; if the server stays unreachable we ABORT and leave the
    // device fully intact so it keeps working and the user can retry later.
    bool confirmed = (g_device_id == FACTORY_ID || g_key_hash.empty());  // nothing to purge
    if (!confirmed) {
        const char* surl = std::getenv("SERVER_URL");
        std::string base = surl ? surl : SERVER_BASE_URL;
        // libcurl (no shell) — the device_id/key_hash never touch a shell command.
        std::string url   = base + "/Factory_Reset_Device";
        std::string h_dev = "Device-Id: " + g_device_id;
        std::string h_key = "Key-Hash: " + g_key_hash;
        for (int attempt = 1; attempt <= 6 && !confirmed; ++attempt) {
            std::cout << "[button] purge attempt " << attempt << "/6...\n";
            long http_code = 0;
            CURL* c = curl_easy_init();
            if (c) {
                struct curl_slist* hdrs = nullptr;
                hdrs = curl_slist_append(hdrs, h_dev.c_str());
                hdrs = curl_slist_append(hdrs, h_key.c_str());
                curl_easy_setopt(c, CURLOPT_URL, url.c_str());
                curl_easy_setopt(c, CURLOPT_POST, 1L);
                curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
                curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                                 +[](char*, size_t sz, size_t nm, void*) { return sz * nm; });
                if (curl_easy_perform(c) == CURLE_OK)
                    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                curl_slist_free_all(hdrs);
                curl_easy_cleanup(c);
            }
            if (http_code == 200) { confirmed = true; break; }
            std::cout << "[button] purge not confirmed (http=" << http_code
                      << ") — retry in " << attempt << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(attempt));   // linear backoff
        }
    }
    if (!confirmed) {
        std::cout << "[button] FACTORY RESET ABORTED — server unreachable; "
                     "device left intact (retry when back online)\n";
        for (int b = 0; b < 6; ++b) {                 // blue blink = aborted
            setLed(LedColor::BLUE); std::this_thread::sleep_for(std::chrono::milliseconds(150));
            setLed(LedColor::OFF);  std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        setLed(idleColor());                           // back to idle (device intact)
        setOled(g_guest_mode.load() ? "sleep" : "idle");   // eyes back
        return;
    }
    std::cout << "[button] server confirmed purge — wiping id.txt + Wi-Fi, rebooting\n";
    // Use sudo for the /var/lib paths: deleting a file needs write permission on
    // its DIRECTORY, and on some installs /var/lib/elderly-care is root-owned —
    // a plain rm (we run as pi) then fails silently and the device reboots STILL
    // provisioned but orphaned server-side. (pi has passwordless sudo on Pi OS.)
    std::system("sudo rm -f /var/lib/elderly-care/id.txt /var/lib/elderly-care/.greeted 2>/dev/null; "
                "rm -f ~/.config/elderly-care/id.txt ~/.config/elderly-care/.greeted 2>/dev/null");
    // Hard guarantee: if id.txt somehow survived, truncate it to empty so
    // loadDeviceIdentity() treats the device as UNPROVISIONED (never orphaned).
    if (std::ifstream("/var/lib/elderly-care/id.txt").good()) {
        std::cerr << "[button] WARNING: id.txt survived rm — truncating to force unprovisioned\n";
        std::system("sudo sh -c ': > /var/lib/elderly-care/id.txt' 2>/dev/null");
    }
    // Same hard-guarantee for the home-dir fallback that loadDeviceIdentity ALSO
    // reads — a survivor there (partial wipe / permissions) would re-provision the
    // now server-purged identity, booting the device orphaned.
    if (const char* home = std::getenv("HOME")) {
        std::string h = std::string(home) + "/.config/elderly-care/id.txt";
        if (std::ifstream(h).good()) {
            std::cerr << "[button] WARNING: home id.txt survived — truncating\n";
            std::system(("sh -c ': > \"" + h + "\"' 2>/dev/null").c_str());
        }
    }
    std::system("nmcli -t -f UUID,TYPE c show 2>/dev/null | awk -F: "
                "  '$2==\"802-11-wireless\"{print $1}' "
                "  | xargs -r -n1 sudo nmcli c delete 2>/dev/null");
    std::system("sync");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::system("sudo systemctl reboot --no-block");
}

// Graceful reboot — a Pi has no soft power-on, so a 10s hold reboots instead
// of halting (the device comes back up + systemd relaunches the pipeline).
static void doReboot() {
    std::cout << "\n[button] REBOOT — graceful restart\n";
    setLed(LedColor::RED);
    setOled("none");   // no eyes while rebooting
    std::system("sync");
    std::system("sudo systemctl reboot --no-block");
}

// Spawn the BLE provisioning sidecar (ble_provisioning.py) and block until
// it exits. Replaces the legacy AP-mode wifi_setup.sh — companion app finds
// the device via flutter_blue_plus, walks Wi-Fi pick + id/password set, then
// the sidecar writes id.txt + exits.
//
// Lookup order:
//   1. /usr/local/bin/ble_provisioning.py  (Pi production install)
//   2. ./ble_provisioning.py               (build dir relative)
//   3. fall back to old wifi_setup.sh (legacy AP mode) if BLE sidecar missing
//   4. dev simulation (5 s wait) if nothing found
//
// LED stays BLUE while the sidecar is alive (see ButtonStateMachine which
// sets it during the hold), then goes back to GREEN on success.
static bool runWifiSetup() {
    std::cout << "\n[setup] button-hold released — entering setup mode\n";
    setOled("none");                 // no eyes while unprovisioned / in setup

    auto exists = [](const char* p) {
        std::ifstream f(p); return f.good();
    };
    const char* ble_installed = "/usr/local/bin/ble_provisioning.py";
    const char* ble_local     = "./ble_provisioning.py";
    const char* legacy_inst   = "/usr/local/bin/wifi_setup.sh";
    const char* legacy_local  = "./wifi_setup.sh";

    int rc = 1;
    {
        // Blinking blue for the whole setup window. Scoped so the blinker thread
        // is stopped/joined before we set the final solid LED below (no race).
        LedBlinker blink;
        blink.start(LedColor::BLUE);

        std::string path;
        if      (exists(ble_installed)) path = ble_installed;
        else if (exists(ble_local))     path = ble_local;
        if (!path.empty()) {
            std::string cmd = "python3 " + path;
            std::cout << "[setup] launching BLE sidecar: " << cmd << "\n";
            rc = std::system(cmd.c_str());
        } else if (exists(legacy_inst) || exists(legacy_local)) {
            std::string p = exists(legacy_inst) ? legacy_inst : legacy_local;
            std::cout << "[setup] BLE sidecar not found, falling back to legacy "
                         "AP-mode wifi_setup.sh\n";
            std::string cmd = "DEVICE_ID=" + std::string(DEVICE_ID) + " " + p;
            rc = std::system(cmd.c_str());
        } else {
            std::cout << "[setup-DEV] no setup script found — simulating (5 s)\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            rc = 0;
        }
    }   // blinker stopped + joined here

    setLed(idleColor());
    if (rc == 0) {
        std::cout << "[setup] completed (rc=0) — applying new creds.\n";
        // Cleanest way to "rerun with the new creds": if we're managed by
        // systemd (production), restart the unit so a fresh process re-reads
        // id.txt from scratch — avoids any stale in-memory identity. systemd
        // (Restart=always) relaunches us within seconds. If NOT under systemd
        // (manual/dev run), fall back to an in-process reload.
        if (std::system("systemctl is-active --quiet elderly-pipeline") == 0) {
            std::cout << "[setup] restarting elderly-pipeline.service for new identity\n";
            std::system("sudo systemctl restart elderly-pipeline --no-block");
            // systemd will SIGTERM us momentarily; wait so we don't continue
            // running with the old identity in the meantime.
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        loadDeviceIdentity();   // fallback for non-systemd runs
        return true;
    }
    std::cerr << "[setup] failed/cancelled (rc=" << rc << ")\n";
    return false;
}

// Encode a frame as JPEG (q90) — mirrors Python's send_image_message params.
static std::vector<uint8_t> encodeJpeg(const cv::Mat& frame, int quality = 90) {
    std::vector<uint8_t> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
    cv::imencode(".jpg", frame, buf, params);
    return buf;
}

// Encode a rolling 5s buffer as MP4. cv::VideoWriter only writes to disk,
// so we go via a temp path then read back. Mirrors Python's behavior.
static std::vector<uint8_t> encodeMp4(const std::deque<cv::Mat>& frames) {
    if (frames.empty()) return {};
    const char* path = "/tmp/fall_event.mp4";
    int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    cv::VideoWriter w(path, fourcc, CAM_FPS,
                      cv::Size(frames.front().cols, frames.front().rows));
    if (!w.isOpened()) return {};
    for (const auto& f : frames) w.write(f);
    w.release();
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

// Mux the already-recorded continuous fall video file (pre-fall → fall →
// feedback → response, written frame-by-frame to disk so RAM stays bounded) with
// the matching continuous audio slice from the ring. Returns the muxed MP4 bytes
// (plaintext) — the caller hands these to sendEventPacket, which E2E-encrypts the
// thumbnail + clip together and uploads ONE event. Empty on failure.
static std::vector<uint8_t> muxClipFile(const std::string& video_path,
                                        const std::vector<float>& audio) {
    const std::string wav = "/tmp/clip_audio.wav", out = "/tmp/clip_av.mp4";
    std::remove(out.c_str());
    auto wavbytes = floatToWavPCM16(audio, SAMPLE_RATE);
    { std::ofstream f(wav, std::ios::binary);
      f.write(reinterpret_cast<const char*>(wavbytes.data()), wavbytes.size()); }
    // Video is already mpeg4-encoded by the recorder → copy it, just add audio.
    // `timeout 30` so a hung ffmpeg (corrupt MP4 / full disk) can't block this
    // detached upload thread forever and leak it.
    std::string cmdline = "timeout 30 ffmpeg -y -loglevel error -i " + video_path + " -i " + wav +
                          " -map 0:v -map 1:a -c:v copy -c:a aac -shortest " + out +
                          " 2>/dev/null";
    if (std::system(cmdline.c_str()) != 0) {
        std::cerr << "[fall-clip] ffmpeg mux failed\n"; return {};
    }
    std::ifstream fin(out, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(fin)),
                                 std::istreambuf_iterator<char>());
}

// ── Shared latest-frame buffer ──────────────────────────────────────────────
// The camera is read by a dedicated grabber thread (cameraGrabberThread) that
// writes the most recent frame here. The fall/heartbeat thread reads the
// latest frame WITHOUT ever calling cv::VideoCapture::read() itself — so a
// camera stutter (the v4l2loopback feeder briefly stalling) can never block
// the heartbeat (which drives pill alarms, snapshots, image requests, incoming
// calls) or the button. Before this split, a single blocking read() in the
// fall loop would hang the entire device. Frames are stored already-flipped.
struct FrameBuffer {
    std::mutex mtx;
    cv::Mat    latest;                                   // most recent (flipped)
    std::chrono::steady_clock::time_point stamp{};       // when it was grabbed
    uint64_t   seq   = 0;                                 // ++ per new frame
    bool       valid = false;
};

// Open the capture device the way the old code did (index first, path fallback)
// plus read/open timeouts so the grabber never blocks indefinitely.
static cv::VideoCapture openCaptureDevice(int cam_idx, const std::string& cam_dev) {
    cv::VideoCapture c(cam_idx, cv::CAP_V4L2);
    if (!c.isOpened()) c.open(cam_dev, cv::CAP_V4L2);
    if (c.isOpened()) {
        c.set(cv::CAP_PROP_FRAME_WIDTH,  CAM_W);
        c.set(cv::CAP_PROP_FRAME_HEIGHT, CAM_H);
        // V4L2 read/open timeouts (OpenCV ≥4.5.1): read() returns false after
        // the timeout instead of blocking forever when the feeder stalls. This
        // is the core defense — the grabber detects the stall + reopens.
        c.set(cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 3000);
        c.set(cv::CAP_PROP_READ_TIMEOUT_MSEC, 1500);
        // NOTE: never set CAP_PROP_FPS — maps to libcamera FrameDurationLimits
        // (array control) and aborts under the V4L2 compat layer.
    }
    return c;
}

// Dedicated camera grabber. Owns the cv::VideoCapture, continuously fills the
// shared FrameBuffer, self-heals on read failure (reopen), and releases the
// camera during a call so the aiortc helper can use it exclusively.
void cameraGrabberThread(std::atomic<bool>& running, FrameBuffer& fb) {
    const char* cam_dev_env = std::getenv("CAMERA_DEV");
    std::string cam_dev = cam_dev_env ? cam_dev_env : "/dev/video0";
    int cam_idx = 0;
    {
        size_t p = cam_dev.find_last_not_of("0123456789");
        if (p != std::string::npos && p + 1 < cam_dev.size())
            cam_idx = std::stoi(cam_dev.substr(p + 1));
    }

    cv::VideoCapture cap = openCaptureDevice(cam_idx, cam_dev);
    if (cap.isOpened()) {
        std::cout << "[camera] grabber opened "
                  << static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH)) << "x"
                  << static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT)) << "\n";
    } else {
        std::cerr << "[camera] grabber: initial open of " << cam_dev
                  << " (idx " << cam_idx << ") failed — will retry\n";
    }

    cv::Mat frame;
    int consec_fail = 0;
    bool was_in_call = false;

    while (running) {
        // During a call the helper needs the camera — release ours. Also release
        // in guest/sleep mode (camera is off for privacy); the was_in_call reopen
        // path below brings it back when the call ends / guest mode clears.
        if (g_in_call.load() || g_guest_mode.load()) {
            if (cap.isOpened()) { cap.release(); }
            was_in_call = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (was_in_call) {                 // call ended — reopen
            was_in_call = false;
            cap = openCaptureDevice(cam_idx, cam_dev);
        }
        if (!cap.isOpened()) {
            cap = openCaptureDevice(cam_idx, cam_dev);
            if (!cap.isOpened()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        if (!cap.read(frame) || frame.empty()) {
            // Feeder stalled or device hiccup. After a few consecutive misses,
            // reopen the device (recovers from a feeder restart). The read
            // timeout above means this returns within ~1.5s, never hangs.
            if (++consec_fail >= 3) {
                std::cerr << "[camera] grabber: " << consec_fail
                          << " read failures — reopening\n";
                cap.release();
                consec_fail = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        consec_fail = 0;
        cv::flip(frame, frame, 0);          // mirror Python cv2.flip(frame, 0)
        {
            std::lock_guard<std::mutex> lk(fb.mtx);
            frame.copyTo(fb.latest);
            fb.stamp = std::chrono::steady_clock::now();
            fb.seq++;
            fb.valid = true;
        }
    }
    if (cap.isOpened()) cap.release();
    std::cout << "[camera] grabber stopped\n";
}

// Dedicated mic capture thread — the SOLE owner of micleft. Reads STRIDE_SAMPLES
// chunks back-to-back into the AudioRing so WUW, the wake-command recorder, and
// (S2) the pain session all consume one gap-free stream, with no per-event ALSA
// open/close on the hot path. Mirrors cameraGrabberThread: releases the device
// while a call OR a pain session needs it exclusively (publishing g_mic_free so
// the other opener never races us), and self-heals on xrun / open failure.
void audioCaptureThread(std::atomic<bool>& running, AudioRing& ring) {
    snd_pcm_t* h = nullptr;
    int  consec_fail = 0;
    auto release = [&] { if (h) { snd_pcm_close(h); h = nullptr; } };
    while (running.load()) {
        // Only a CALL needs micleft exclusively (separate aiortc process) —
        // release & idle then. The pain session reads the same ring we fill, so
        // we keep capturing right through a fall/pain (no release for g_fall).
        if (g_in_call.load()) {
            release();
            g_mic_free.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!h) {
            h = openMicHandle();
            if (!h) {                       // device busy/absent — back off, retry
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            g_mic_free.store(false);        // we own it again
        }
        std::vector<float> chunk = readMicStride(h);   // [] on xrun (already prepared)
        if (chunk.empty()) {
            if (++consec_fail >= 50) {      // persistent failure → full reopen
                std::cerr << "[audio] capture stalled — reopening mic\n";
                release(); consec_fail = 0;
            }
            continue;
        }
        consec_fail = 0;
        ring.write(chunk.data(), chunk.size());
    }
    release();
    std::cout << "[audio] capture thread stopped\n";
}

// T6 — dedicated button poller. Runs at ~66 Hz INDEPENDENT of the heartbeat so
// quick taps are never missed or smeared into a "hold" by a slow /Live_Device
// curl (which used to share T5's loop and starved button sampling on a laggy
// network — the root cause of "factory-reset taps not registering"). Gestures:
// 3 taps = guest/sleep, 5 = factory reset, 3s hold = BLE setup, 10s = reboot.
void buttonThread(std::atomic<bool>& running) {
    ButtonStateMachine btn_sm;
    while (running.load()) {
        switch (btn_sm.tick(readButton(), g_guest_mode.load())) {
            case ButtonGesture::ENTER_SETUP:   runWifiSetup();   break;
            case ButtonGesture::REBOOT:        doReboot();       break;
            case ButtonGesture::FACTORY_RESET: doFactoryReset(); break;
            case ButtonGesture::ENTER_GUEST:
                g_guest_mode.store(true);  setLed(LedColor::RED); setOled("sleep");
                std::cout << "[guest] SLEEP — fall+camera paused; wake is double-gated (\"Hey\"+\"Noban\"); press once to wake\n";
                break;
            case ButtonGesture::WAKE:
                g_guest_mode.store(false); setLed(idleColor()); setOled("wakegreet");
                std::cout << "[guest] WAKE — monitoring resumed\n";
                break;
            case ButtonGesture::NONE: break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));   // ~66 Hz
    }
    std::cout << "[button] thread stopped\n";
}

// T5 — housekeeping: button + /Live_Device heartbeat (snapshot / pill alarms /
// incoming-call / image-request). Split out of the fall thread so heavy
// fall-inference bursts can never add latency to button response, pill
// playback, or call pickup. Each cmd_client call uses its own curl handle, so
// running concurrently with the fall thread's sendEventPacket is safe.
void housekeepingThread(std::atomic<bool>& running, std::shared_ptr<ServerCommandClient> cmd_client,
                        FrameBuffer& fb) {
    auto last_hb = std::chrono::steady_clock::now() - HEARTBEAT_PERIOD;
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_play_per_id;
    auto pill_player_busy = std::make_shared<std::atomic<bool>>(false);
    const auto PILL_REPLAY_PERIOD = std::chrono::minutes(5);

    while (running) {
        auto now = std::chrono::steady_clock::now();

        // Button is handled in its own thread (buttonThread) so a slow heartbeat
        // curl here can't starve tap sampling.
        if (cmd_client && now - last_hb >= HEARTBEAT_PERIOD) {
            last_hb = now;
            auto st = cmd_client->sendLivePacket();
            // Apply a one-shot remote guest toggle from the app (same effect as
            // the physical button). Reported back via Guest-Mode next heartbeat,
            // which syncs every caregiver's UI. Applied BEFORE the gated actions
            // below so a SLEEP command silences this same cycle.
            if (st.guest_cmd == 1 && !g_guest_mode.load()) {
                g_guest_mode.store(true);  setLed(LedColor::RED); setOled("sleep");
                std::cout << "[guest] SLEEP via app — fall+camera paused; wake double-gated (\"Hey\"+\"Noban\")\n";
            } else if (st.guest_cmd == 0 && g_guest_mode.load()) {
                g_guest_mode.store(false); setLed(idleColor()); setOled("wakegreet");
                std::cout << "[guest] WAKE via app — monitoring resumed\n";
            }
            if (st.image_req_id >= 0 && !g_guest_mode.load()) {  // no snapshots while asleep (privacy)
                cv::Mat snap;
                { std::lock_guard<std::mutex> lk(fb.mtx);
                  if (fb.valid) fb.latest.copyTo(snap); }
                if (!snap.empty()) {
                    auto jpeg = encodeJpeg(snap);
                    bool ok = cmd_client->sendImageMessage(st.image_req_id, jpeg);
                    std::cout << "[snapshot] image_req_id=" << st.image_req_id
                              << " upload " << (ok ? "OK" : "FAILED")
                              << " (" << jpeg.size() << " B)\n";
                } else {
                    // Log once per distinct request id — the server re-asks every
                    // heartbeat until a frame arrives, so without this it spams.
                    static int last_noframe_id = -1;
                    if (st.image_req_id != last_noframe_id) {
                        last_noframe_id = st.image_req_id;
                        std::cout << "[snapshot] image_req_id=" << st.image_req_id
                                  << " but no camera frame available yet\n";
                    }
                }
            }
            // Pill alarms: play each due one (per-ID 5-min cooldown), sequentially
            // in a detached chain so audios don't overlap and stay off this loop.
            if (!st.pill_alarms.empty() && !pill_player_busy->load()
                    && !g_in_call.load()) {   // pills still play in guest; don't fight the call for the amp
                std::vector<std::pair<int, std::string>> due;
                for (const auto& pa : st.pill_alarms) {
                    auto it = last_play_per_id.find(pa.id);
                    bool replay_due = (it == last_play_per_id.end()) ||
                                      (now - it->second >= PILL_REPLAY_PERIOD);
                    if (replay_due) { due.emplace_back(pa.id, pa.name); last_play_per_id[pa.id] = now; }
                }
                // CAS the busy flag: only spawn when no playback is in flight, so
                // rapid heartbeats can't launch overlapping pill threads that
                // would fight for the ALSA amp. (Resets to false in the thread.)
                bool _pill_expected = false;
                if (!due.empty() &&
                    pill_player_busy->compare_exchange_strong(_pill_expected, true)) {
                    // Capture cmd_client + the busy flag BY VALUE as shared_ptrs so
                    // the detached thread keeps them alive and can never dangle even
                    // if this thread / main exits while audio is still playing.
                    std::thread([due, cmd_client, pill_player_busy] {
                        for (auto& [pid, pname] : due) {
                            std::string audio = cmd_client->fetchPillAudio(pid);
                            if (audio.empty()) continue;
                            std::cout << "[pill] playing '" << pname << "' (id=" << pid << ")\n";
                            // Boost + play on the amp. The recorded files are
                            // low-level (near-inaudible raw) → dynaudnorm lifts
                            // them; bare ALSA default is an asym pcm tied to the
                            // held mic, so use plughw amp directly. PILL_AF overrides.
                            const char* _af = std::getenv("PILL_AF");
                            std::string _filt = _af ? _af : "loudnorm=I=-16:TP=-1.5:LRA=11";
                            // PILL_AF is single-quoted into a shell command; a stray
                            // quote would break out. Refuse it → safe default.
                            if (_filt.find('\'') != std::string::npos)
                                _filt = "loudnorm=I=-16:TP=-1.5:LRA=11";
                            std::string cmd = "timeout 30 ffmpeg -loglevel quiet -nostdin -i '" + audio +
                                "' -af '" + _filt + "' -f wav - 2>/dev/null"
                                " | timeout 30 aplay -q -D plughw:CARD=sndrpigooglevoi,DEV=0 -";
                            std::system(cmd.c_str());
                        }
                        pill_player_busy->store(false);
                    }).detach();
                }
            }
            // Prune cooldown entries the server no longer reports active.
            if (!last_play_per_id.empty()) {
                std::unordered_set<int> still_active;
                for (const auto& pa : st.pill_alarms) still_active.insert(pa.id);
                for (auto it = last_play_per_id.begin(); it != last_play_per_id.end(); ) {
                    if (!still_active.count(it->first)) it = last_play_per_id.erase(it);
                    else ++it;
                }
            }
            // Caregiver voice note — play ONCE on the amp, serialized with pills
            // via the SAME busy flag so the two can never collide on the device.
                bool _voice_expected = false;
                if (!st.voice_msg_url.empty() && !g_in_call.load()   // plays in guest
                    && pill_player_busy->compare_exchange_strong(_voice_expected, true)) {
                std::string vurl = st.voice_msg_url;
                std::thread([vurl, cmd_client, pill_player_busy] {
                    std::string vp = cmd_client->fetchVoiceMsg(vurl);
                    if (!vp.empty()) {
                        std::cout << "[voice] playing caregiver note\n";
                        const char* _af = std::getenv("PILL_AF");
                        std::string _filt = _af ? _af : "loudnorm=I=-16:TP=-1.5:LRA=11";
                        if (_filt.find('\'') != std::string::npos)
                            _filt = "loudnorm=I=-16:TP=-1.5:LRA=11";  // refuse quote breakout
                        std::string c = "timeout 30 ffmpeg -loglevel quiet -nostdin -i '" + vp +
                            "' -af '" + _filt + "' -f wav - 2>/dev/null"
                            " | timeout 30 aplay -q -D plughw:CARD=sndrpigooglevoi,DEV=0 -";
                        std::system(c.c_str());
                        std::remove(vp.c_str());
                    }
                    pill_player_busy->store(false);
                }).detach();
            }

            // Incoming call — pause WUW + fall (devices released), run the helper.
            // Calls still connect in guest mode (only fall + camera are gated).
            if (st.call_pending && !g_in_call.load()) {
                std::cout << "[call] incoming call — launching aiortc helper\n";
                g_mic_free.store(false);
                g_in_call.store(true);
                // T1 (audio) + camera grabber release their devices on g_in_call.
                // Wait for the audio thread to publish g_mic_free (race-free mic
                // handoff), capped so a stuck thread can't wedge the call.
                for (int i = 0; i < 20 && !g_mic_free.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::this_thread::sleep_for(std::chrono::milliseconds(150)); // camera release
                setLed(LedColor::BLUE);
                std::string cmd =
                    std::string("DEVICE_ID='") + DEVICE_ID + "' " +
                    "KEY_HASH='" + KEY_HASH + "' " +
                    "SERVER_URL='" + cmd_client->base_url + "' " +
                    // INMP441 is a clean mic with no DC rumble → skip the SPH0645
                    // high-pass+30x-gain default (it would distort).
                    "MIC_FILTER='none' " +
                    "sh -c 'python3 /usr/local/bin/wake_call_helper.py 2>/dev/null "
                    "|| python3 ./wake_call_helper.py'";
                int rc = std::system(cmd.c_str());
                std::cout << "[call] helper exited rc=" << rc << "\n";
                setLed(idleColor());
                g_in_call.store(false);   // T4 sees this → inserts the camera gap
                // Authoritatively clear the server call state now that OUR helper
                // has exited — otherwise a stalled/crashed app that never sent
                // /Call_Stop leaves janus:calls set and we loop straight back into
                // the call by ourselves. Fail-fast curl; harmless if already clear.
                {
                    std::string stopcmd =
                        "timeout 5 curl -s -m 5 -o /dev/null -X POST '" +
                        cmd_client->base_url + "/Call_Stop' "
                        "-H 'Device-Id: " + DEVICE_ID + "' -H 'Key-Hash: " + KEY_HASH + "' "
                        "-H 'Content-Type: application/json' "
                        "--data '{\"device_id\":\"" + DEVICE_ID + "\"}'";
                    std::system(stopcmd.c_str());
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    std::cout << "[housekeeping] thread stopped\n";
}

void fallPipelineThread(std::atomic<bool>& running, std::shared_ptr<ServerCommandClient> cmd_client,
                        FrameBuffer& fb, AudioRing& ring) {
    // cv::VideoCapture uses V4L2 backend; on Pi OS Bookworm V4L2 is wired
    // to libcamera. Falls back to numeric device index if name open fails.
    // CAMERA_DEV lets us point at a v4l2loopback device (e.g. /dev/video9)
    // fed by rpicam-vid, instead of the raw CSI /dev/video0. The libcamera
    // V4L2 compat layer (libcamerify) aborts on control enumeration with
    // OpenCV, so on the Pi we feed frames through a loopback that presents
    // as a plain UVC camera. Defaults to /dev/video0 for USB-cam/dev boxes.
    // Camera frames now come from the grabber thread via `fb` — this thread
    // never touches cv::VideoCapture, so a camera stall can't block the
    // heartbeat or button below.
    std::unique_ptr<fall::FallDetection> det;
    try {
        det = std::make_unique<fall::FallDetection>(CAM_H, CAM_W);
        std::cout << "[fall] FallDetection initialized.\n";
    } catch (const std::exception& e) {
        std::cerr << "[fall] init failed: " << e.what() << " — thread exiting.\n";
        return;
    }

    std::vector<fall::FrameClass> batch;
    batch.reserve(16);
    // Rolling 5s buffer (80 frames at 16fps) — feeds the fall-event MP4 upload.
    std::deque<cv::Mat> rolling;
    auto last     = std::chrono::steady_clock::now();
    cv::Mat frame;
    uint64_t last_seq = 0;       // last frame seq we consumed from the buffer

    // Fall A/V clip state. On a fall we open a VideoWriter and record the WHOLE
    // event continuously to disk (pre-roll frames first, then live frames right
    // through the pain feedback session) so RAM stays bounded. When the session
    // ends we cut the matching continuous audio slice from the ring, mux, stamp
    // the pain-derived danger level, and send ONE encrypted event.
    bool clip_pending   = false;
    bool clip_recording = false;
    cv::VideoWriter clip_writer;
    cv::Mat  clip_thumb;                  // fall-moment frame (event thumbnail)
    uint64_t clip_audio_at_fall = 0;
    int      clip_frames = 0;
    const std::string CLIP_VIDEO_PATH = "/tmp/clip_video.mp4";
    constexpr size_t CLIP_PREROLL_SAMPLES = static_cast<size_t>(SAMPLE_RATE) * EVENT_CLIP_SEC;
    const int CLIP_MAX_FRAMES = CAM_FPS * 30;   // safety cap (~30 s)

    while (running) {
        auto now = std::chrono::steady_clock::now();

        // Camera is released during a call (housekeepingThread launches the
        // helper + sets g_in_call). Skip fall processing while in-call; on
        // call-end drop the stale batch + insert a gap sentinel so the fall
        // smoother doesn't bridge the missing frames. Button / heartbeat /
        // pills / calls now live in housekeepingThread (T5).
        // Also paused by guest/sleep mode (true do-not-disturb). Reuse the same
        // pause+gap-sentinel path so fall detection cleanly restarts on wake
        // instead of bridging across the sleep.
        static bool was_in_call = false;   // MUST persist across loop iterations:
        if (g_in_call.load() || g_guest_mode.load()) {  // a plain local resets each
            was_in_call = true;            // iteration → the gap-sentinel resume
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;                      // path below would never fire.
        }
        if (was_in_call) {
            was_in_call = false;
            if (det) det->insertGapSentinel();
            batch.clear(); batch.reserve(16);
        }

        // ── Pain session just ended → finalize the continuous clip and send the
        // event with the pain-derived danger level. The video file already holds
        // the whole event (pre-roll → fall → feedback → response); cut the one
        // continuous audio slice [fall−5 s, now], mux, and send ONE encrypted
        // event (thumbnail + A/V clip) on a detached thread so fall detection
        // resumes immediately. This is the only fall upload — colour-correct.
        if (clip_pending && pain::g_fall.load() == 0) {
            clip_pending = false;
            if (clip_recording) { clip_writer.release(); clip_recording = false; }
            int danger = g_fall_danger_level.load();
            if (danger < 0) danger = 2;                 // fallback: treat as serious
            uint64_t pre_from = clip_audio_at_fall > CLIP_PREROLL_SAMPLES
                              ? clip_audio_at_fall - CLIP_PREROLL_SAMPLES : 0;
            uint64_t now_c = ring.now();
            std::vector<float> audio;
            ring.slice(pre_from, static_cast<size_t>(now_c - pre_from), audio);  // whole window
            std::string vpath = CLIP_VIDEO_PATH;
            cv::Mat thumb = clip_thumb;
            // Bound concurrent fall uploads: if a previous one is still in flight
            // (slow network / hung mux), skip rather than stacking detached
            // threads that each pin a multi-MB audio+video buffer.
            static std::atomic<bool> fall_upload_busy(false);
            bool _fall_exp = false;
            if (fall_upload_busy.compare_exchange_strong(_fall_exp, true)) {
                std::thread([cmd_client, danger, vpath, thumb,
                             audio = std::move(audio)]() mutable {
                    auto clip = muxClipFile(vpath, audio);
                    auto jpeg = encodeJpeg(thumb);
                    if (cmd_client) cmd_client->sendEventPacket(jpeg, clip, danger);
                    std::cout << "[fall] event sent (danger=" << danger
                              << ", clip " << clip.size() << "B)\n";
                    fall_upload_busy.store(false);
                }).detach();
            } else {
                std::cout << "[fall] previous upload still in flight — skipping\n";
            }
        }

        // ── Fetch the latest frame from the grabber. Never blocks. If there's
        // no FRESH frame (feeder hiccup, or camera released during a call) we
        // skip fall processing this iteration — but the heartbeat/button above
        // already ran, so the device stays responsive regardless of the camera.
        cv::Mat fbuf;
        uint64_t seq = 0;
        std::chrono::steady_clock::time_point stamp{};
        {
            std::lock_guard<std::mutex> lk(fb.mtx);
            if (fb.valid) { fb.latest.copyTo(fbuf); seq = fb.seq; stamp = fb.stamp; }
        }
        bool fresh = !fbuf.empty() && seq != last_seq &&
                     (now - stamp) < std::chrono::milliseconds(700);
        if (!fresh) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        last_seq = seq;
        frame = fbuf;
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        double fps = dt > 0.0 ? 1.0 / dt : 0.0;

        // Rolling 5s buffer (the clip pre-roll).
        rolling.push_back(frame.clone());
        if (static_cast<int>(rolling.size()) > EVENT_CLIP_FRAMES) rolling.pop_front();

        // While a fall clip is recording, append every live frame to the file
        // (this runs through the whole pain feedback session). Bounded by a hard
        // frame cap so a stuck session can't grow the file without limit.
        if (clip_recording && clip_writer.isOpened()) {
            clip_writer.write(frame);
            if (++clip_frames >= CLIP_MAX_FRAMES) { clip_writer.release(); clip_recording = false; }
        }

        batch.push_back(fall::FrameClass{frame.clone(), fps});
        if (batch.size() < 16) continue;

        try {
            auto res = det->frame_inference(std::move(batch));
            // Run inference unconditionally so the FallDetection internal
            // deques (bboxes/out1/2/3/frame_info_list) stay continuous —
            // pause-don't-reset. But IGNORE positive fall_label while a
            // pain session is already in progress (would just re-fire
            // what's already running).
            if (res.fall_label && pain::g_fall.load() == 0) {
                std::cout << "[fall] !!! FALL CONFIRMED — starting pain feedback NOW\n";
                // Capture the fall instant (audio cursor + thumbnail frame) and
                // signal the pain feedback FIRST — nothing (upload, encode, mux)
                // runs before this, so WUW stops and the "speak now" prompt fires
                // immediately. The event is sent ONCE, after the session, stamped
                // with the pain-derived danger colour.
                clip_audio_at_fall = ring.now();
                clip_thumb = frame.clone();
                g_fall_danger_level.store(-1);
                pain::g_fall.store(1);
                // Start recording the WHOLE event to disk: pre-roll frames now,
                // then every live frame through the pain session (fed above).
                std::remove(CLIP_VIDEO_PATH.c_str());
                clip_writer.open(CLIP_VIDEO_PATH,
                                 cv::VideoWriter::fourcc('m','p','4','v'), CAM_FPS,
                                 cv::Size(frame.cols, frame.rows));
                if (clip_writer.isOpened()) {
                    for (const auto& fr : rolling) clip_writer.write(fr);
                    clip_frames = static_cast<int>(rolling.size());
                    clip_recording = true;
                }
                clip_pending = true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[fall] frame_inference threw: " << e.what() << "\n";
        }
        batch.clear();
        batch.reserve(16);
    }
}

// WUW WAV debug: feed a raw capture through the EXACT live streaming path
// (per-STRIDE_SAMPLES computeFeatures + inferFromFeatures) and print per-stride
// raw_wake. Handles S32 (raw micleft capture, scaled by the live MIC_SCALE so
// this mirrors the mic) and S16 (÷32768) WAVs — bits/chans read from the header
// (offsets 34/22). Combine with MIC_SCALE_POW env to sweep the scale offline
// without recompiling, until the device WAV fires like the Python reference.
int runWuwWavDebug(const std::string& wuw_wav, WakeWordDetector& wwd) {
    std::ifstream f(wuw_wav, std::ios::binary);
    if (!f) { std::cerr << "could not open " << wuw_wav << "\n"; return 1; }
    std::vector<unsigned char> hdr(44);
    f.read(reinterpret_cast<char*>(hdr.data()), 44);
    uint16_t bits  = hdr[34] | (uint16_t(hdr[35]) << 8);
    uint16_t chans = hdr[22] | (uint16_t(hdr[23]) << 8);
    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(f)),
         std::istreambuf_iterator<char>{});
    std::vector<float> audio;
    if (bits == 32) {
        const int32_t* s32 = reinterpret_cast<const int32_t*>(bytes.data());
        size_t n = bytes.size() / sizeof(int32_t);
        for (size_t i = 0; i < n; i += (chans ? chans : 1))
            audio.push_back(float(s32[i]) * MIC_SCALE);  // ch0, live scaling
    } else {
        const int16_t* s16 = reinterpret_cast<const int16_t*>(bytes.data());
        size_t n = bytes.size() / sizeof(int16_t);
        for (size_t i = 0; i < n; i += (chans ? chans : 1))
            audio.push_back(s16[i] / 32768.0f);
    }
    double s = 0; float pk = 0;
    for (float v : audio) { s += double(v)*v; pk = std::max(pk, std::fabs(v)); }
    double rms = audio.empty() ? 0 : std::sqrt(s/audio.size());
    std::cerr << "[wuw-wav] " << wuw_wav << "  bits=" << bits
              << " chans=" << chans << " samples=" << audio.size()
              << "  MIC_SCALE=1/" << (1.0f/MIC_SCALE)
              << "  rms=" << rms << " peak=" << pk << "\n";
    wwd.reset();
    float ema_wake = 0.0f, peak_raw = 0.0f, peak_ema = 0.0f;
    int stride = 0;
    for (size_t off = 0; off + STRIDE_SAMPLES <= audio.size();
         off += STRIDE_SAMPLES, ++stride) {
        std::vector<float> chunk(audio.begin() + off,
                                 audio.begin() + off + STRIDE_SAMPLES);
        auto feats = wwd.computeFeatures(chunk);
        auto p     = wwd.inferFromFeatures(feats);
        ema_wake   = ALPHA*p.wake + (1.0f-ALPHA)*ema_wake;
        peak_raw   = std::max(peak_raw, p.wake);
        peak_ema   = std::max(peak_ema, ema_wake);
        std::cerr << "  stride " << stride
                  << ": raw_wake=" << p.wake
                  << " raw_other=" << p.other
                  << " raw_noise=" << p.noise
                  << " ema_wake=" << ema_wake << "\n";
    }
    std::cerr << ">>> peak raw_wake=" << peak_raw
              << "  peak ema_wake=" << peak_ema
              << "  (UP=" << THRESHOLD_UP << ")\n";
    return 0;
}

// =========================================================
// MAIN — state machine: fall==0 -> WUW loop, fall==1 -> pain session
// =========================================================
// Parity-fixtures mode: iterate fixtures/, run each .wav through WUW + pain
// pipelines and each .jpg through fall_detection, then exit. The dump hooks
// (gated by DUMP_TENSORS at build) write tensors to DUMP_TENSORS_DIR.
static int runParityFixtures(const std::string& fixture_dir,
                             WakeWordDetector& wwd,
                             pain::PainPipeline& pain_pipe) {
    namespace fs = std::filesystem;
    if (!fs::exists(fixture_dir)) {
        std::cerr << "parity: fixture dir not found: " << fixture_dir << "\n";
        return 1;
    }
    std::cout << "[parity] iterating fixtures under " << fixture_dir << "\n";

    // ── WAV fixtures → WUW + pain
    for (auto& e : fs::directory_iterator(fixture_dir)) {
        if (e.path().extension() != ".wav") continue;
        g_parity_fixture_stem = e.path().stem().string();
        std::cout << "[parity] " << g_parity_fixture_stem << " (wav)\n";
        // Load WAV → mono float [-1,1] @16kHz. Minimal parser: assume
        // 16-bit PCM mono 16kHz (parity fixtures should be normalized).
        // Read as raw bytes first; reinterpret as int16_t (vector<int16_t>
        // with a char-iter constructor would store one byte per int16_t,
        // which corrupts every sample).
        std::ifstream f(e.path(), std::ios::binary);
        f.seekg(44);   // skip standard WAV header
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>{});
        const int16_t* s16 = reinterpret_cast<const int16_t*>(bytes.data());
        size_t n_samples = bytes.size() / sizeof(int16_t);
        std::vector<float> audio(n_samples);
        for (size_t i = 0; i < n_samples; ++i)
            audio[i] = s16[i] / 32768.0f;
        // Pad short audio (leading zeros for pain, trailing for WUW — same as
        // each pipeline's own behavior). For each pipeline, slice to the
        // 1-second window the matching Python ref uses:
        //   • WUW: FIRST 16000 samples (Python: `y[:TARGET_WIDTH*HOP]`)
        //   • Pain: LAST 16000 samples (Python: `y[-16000:]` in frontends.py)
        std::vector<float> audio_wuw, audio_pain;
        if (audio.size() < 16000) {
            audio_wuw = audio; audio_wuw.resize(16000, 0.0f);                   // trail-pad
            audio_pain.assign(16000 - audio.size(), 0.0f);
            audio_pain.insert(audio_pain.end(), audio.begin(), audio.end());     // leading-pad
        } else {
            audio_wuw.assign(audio.begin(), audio.begin() + 16000);
            audio_pain.assign(audio.end() - 16000, audio.end());
        }
        // Reset WUW state between fixtures so each one starts with a fresh
        // PCEN M / lookback / mel ring buffer (parity ref recomputes from
        // leading-zero prefix every time; without this reset the fixtures'
        // first frames would carry over each other's tails).
        wwd.reset();
        auto feats = wwd.computeFeatures(audio_wuw);
        (void)wwd.inferFromFeatures(feats);
        // Pain path: score (logmel5 + YAMNet dump inside)
        (void)pain_pipe.score(audio_pain);
    }

    // ── JPG fixtures → fall_detection
    auto fall_det = std::make_unique<fall::FallDetection>(CAM_H, CAM_W);
    for (auto& e : fs::directory_iterator(fixture_dir)) {
        if (e.path().extension() != ".jpg") continue;
        g_parity_fixture_stem = e.path().stem().string();
        std::cout << "[parity] " << g_parity_fixture_stem << " (jpg)\n";
        cv::Mat frame = cv::imread(e.path().string());
        if (frame.empty()) {
            std::cerr << "  could not read\n"; continue;
        }
        // Feed 32 copies (2 batches) — matches dump_python_reference.py
        std::vector<fall::FrameClass> batch;
        for (int i = 0; i < 16; ++i) batch.push_back({frame, 16.0});
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<fall::FrameClass> b = batch;
            fall_det->frame_inference(std::move(b));
        }
    }

    g_parity_fixture_stem.clear();
    std::cout << "[parity] done\n";
    return 0;
}

int main(int argc, char** argv) {
    // Line-buffer stdout so journald timestamps reflect when each line was
    // actually printed (under systemd stdout is a pipe → fully buffered by
    // default, which batches timestamps and makes latency impossible to read).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Load device identity (device_id + password) from id.txt before anything
    // else — every subsequent HTTP call uses g_key_hash for auth.
    loadDeviceIdentity();
    g_mac = readDeviceMac();   // stable MAC for server-side re-provision dedup
    std::cerr << "[id] device MAC=" << (g_mac.empty() ? "(none)" : g_mac) << "\n";

    // Offline WUW debug: --wuw-wav <file> streams a raw capture through the
    // exact live feature path and exits. Intercept BEFORE the provisioning /
    // MQTT / model-heavy startup so it runs on a dev box with no id.txt and no
    // server. Loads ONLY the wake model.
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--wuw-wav") {
            WakeWordDetector wwd;
            if (!wwd.init(TFLITE_MODEL)) return 1;
            wwd.warmUp();
            return runWuwWavDebug(argv[i + 1], wwd);
        }
    }

    // Unprovisioned guard: a fresh SD card or a post-factory-reset wipe leaves
    // no id.txt, so loadDeviceIdentity() booted us as FACTORY_ID with no creds.
    // In that state we must NOT start MQTT / models / server polling — run ONLY
    // the BLE provisioning sidecar, looping until it writes a real id.txt (at
    // which point runWifiSetup() reloads the identity or systemd restarts us).
    setOled("none");   // unprovisioned → no eyes until setup completes
    while (g_device_id == FACTORY_ID) {
        std::cerr << "[id] UNPROVISIONED — entering BLE setup; server endpoints "
                     "stay disabled until provisioned.\n";
        runWifiSetup();
        // ALWAYS back off before re-checking. runWifiSetup() can return true
        // (sidecar/dev-sim exited 0) WITHOUT writing id.txt, leaving
        // g_device_id == FACTORY_ID; without this sleep the loop would respawn
        // the sidecar in a tight 100% CPU spin on an always-on device.
        if (g_device_id == FACTORY_ID) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    // Phase 5.1 — open the persistent MQTT subscription. Broker URI defaults
    // to the same host as SERVER_BASE_URL on port 1883, override via
    // MQTT_HOST + MQTT_PORT env. Coexists with the HTTP /Live_Device poll
    // for now (safety net); once MQTT delivery is verified, polling can drop
    // to a low-frequency liveness ping.
    auto _mqtt_broker_uri = [] () {
        const char* host = std::getenv("MQTT_HOST");
        const char* port = std::getenv("MQTT_PORT");
        std::string h = host ? host : "192.168.1.104";
        std::string p = port ? port : "1883";
        return "tcp://" + h + ":" + p;
    }();
    static mq::Client mqtt_client(_mqtt_broker_uri, g_device_id);
    std::cerr << "[mqtt] client started → " << _mqtt_broker_uri << "\n";

    // Parity test mode: --parity-fixtures <dir>
    // Fall-video test:  --fall-video <path.mp4> [--fall-loops N]
    //   loops the video N times to exceed the 7-batch threshold (Stage 6
    //   needs batch_number >= 7 to fire the MLP confirm). Default N = 3.
    std::string parity_dir, fall_video, wuw_wav;
    int fall_loops = 3;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--parity-fixtures") parity_dir = argv[i + 1];
        if (std::string(argv[i]) == "--fall-video")     fall_video = argv[i + 1];
        if (std::string(argv[i]) == "--fall-loops")     fall_loops = std::atoi(argv[i + 1]);
        if (std::string(argv[i]) == "--wuw-wav")        wuw_wav    = argv[i + 1];
    }

    std::cout<<"╔══════════════════════════════════════════════╗\n";
    std::cout<<"║  Farsi WUW + Pain Detection — fall-driven   ║\n";
    std::cout<<"╚══════════════════════════════════════════════╝\n\n";

    std::cout<<"Loading models — please wait...\n\n";

    WakeWordDetector wwd;  if (!wwd.init(TFLITE_MODEL)) return 1;
    wwd.warmUp();
    // "Hey" pre-gate model for the guest-mode double-gate. Both detectors run
    // every stride so their PCEN state stays warm and continuous.
    WakeWordDetector hey_wwd;  if (!hey_wwd.init(HEY_MODEL)) return 1;
    hey_wwd.warmUp();
    SileroVAD vad;         vad.init(SILERO_MODEL);
    auto cmd_client = std::make_shared<ServerCommandClient>();
    // Honor the SERVER_URL env (set by systemd) for ALL server comms — heartbeat,
    // snapshots, events, calls. Previously hardcoded SERVER_BASE_URL, so the device
    // silently kept hitting the build-time IP after any network/IP change.
    const char* surl_main = std::getenv("SERVER_URL");
    if (!cmd_client->init(surl_main ? surl_main : SERVER_BASE_URL)) return 1;

    pain::PainPipeline pain_pipe;
    if (!pain_pipe.init(YAMNET_MODEL, PAIN_MODEL)) {
        std::cerr << "Pain pipeline init failed — continuing WUW-only.\n";
    }

    if (!parity_dir.empty()) {
        return runParityFixtures(parity_dir, wwd, pain_pipe);
    }
    if (!fall_video.empty()) {
        // Per-batch parity dump against /tmp/fall_python.json.
        cv::VideoCapture cap(fall_video);
        if (!cap.isOpened()) {
            std::cerr << "could not open " << fall_video << "\n"; return 1;
        }
        int W = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        int H = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        double fps = cap.get(cv::CAP_PROP_FPS);
        if (fps <= 0) fps = 16.0;
        // Preload all frames so loop iterations don't pay decode jitter and
        // we feed the detector identical pixels each pass.
        std::vector<cv::Mat> frames;
        while (true) {
            cv::Mat f;
            if (!cap.read(f) || f.empty()) break;
            frames.push_back(f);
        }
        cap.release();
        std::cout << "video " << fall_video << "  " << W << "x" << H
                  << "  fps=" << fps << "  " << frames.size()
                  << " frames × " << fall_loops << " loops\n";
        auto det = std::make_unique<fall::FallDetection>(H, W);
        std::vector<fall::FrameClass> batch;
        int batch_idx = 0;
        std::ofstream js("/tmp/fall_cpp.json");
        js << "[\n";
        bool first = true;
        for (int lp = 0; lp < fall_loops; ++lp)
        for (auto& frame : frames) {
            batch.push_back({frame, fps});
            if ((int)batch.size() == 16) {
                batch_idx++;
                auto r = det->frame_inference(std::move(batch));
                auto s = det->getStateSummary();
                printf("  batch %2d  fall=%s  out1Σ=%2d out2Σ=%2d out3Σ=%2d "
                       "bbox=%d/16  smooth=%d/16  ignore=%d\n",
                       batch_idx, r.fall_label ? "True" : "False",
                       s.out1_sum, s.out2_sum, s.out3_sum,
                       s.bbox_present, s.smoothed_present, s.ignore);
                if (!first) js << ",\n";
                first = false;
                js << "  {\"batch\": " << batch_idx
                   << ", \"fall_label\": " << (r.fall_label ? "true" : "false")
                   << ", \"out1_sum\": " << s.out1_sum
                   << ", \"out2_sum\": " << s.out2_sum
                   << ", \"out3_sum\": " << s.out3_sum
                   << ", \"bbox_present\": " << s.bbox_present
                   << ", \"smoothed_present\": " << s.smoothed_present
                   << ", \"ignore\": " << s.ignore << "}";
                batch.clear();
            }
        }
        js << "\n]\n";
        std::cout << "\nwrote /tmp/fall_cpp.json (" << batch_idx << " batches)\n";
        return 0;
    }

    std::cout<<"\n✅ All models loaded. Starting pipeline.\n";
    setLed(idleColor());   // BLUE = idle/listening (RED in guest)
    // First-ever provisioned boot greets (daemon shows happy ~30s); later boots
    // start calm. markGreeted() drops a marker so it only happens once.
    if (!hasGreeted()) { setOled("greet"); markGreeted(); }
    else               { setOled(g_guest_mode.load() ? "sleep" : "idle"); }

    // Open ALSA capture handle right here in main. WUW inference runs in this
    // same thread; the 300ms blocking read paces everything. Saves one
    // long-lived thread vs. the producer-consumer split.
    // Concurrent contexts (dedicated-capture architecture):
    //   • T1 audio capture — owns micleft, fills the AudioRing, self-heals,
    //     releases the device for calls/pain. Sole ALSA capture opener.
    //   • T2 camera grabber — owns cv::VideoCapture, fills the frame buffer.
    //   • T3 this main() thread — WUW + pain state machine; CONSUMES the ring.
    //   • T4 fall+heartbeat — fall detection + /Live_Device (pills/calls/button).
    std::atomic<bool> fallRunning(true);
    g_run_flag = &fallRunning;
    std::signal(SIGTERM, on_term);
    std::signal(SIGINT,  on_term);
    static FrameBuffer g_frame_buffer;
    static AudioRing   g_audio_ring(static_cast<size_t>(SAMPLE_RATE) * RING_SECONDS);
    std::thread audioCap(audioCaptureThread, std::ref(fallRunning),
                         std::ref(g_audio_ring));
    std::thread camGrab(cameraGrabberThread, std::ref(fallRunning),
                        std::ref(g_frame_buffer));
    std::thread fallProc(fallPipelineThread, std::ref(fallRunning), cmd_client,
                         std::ref(g_frame_buffer), std::ref(g_audio_ring));
    std::thread houseKeep(housekeepingThread, std::ref(fallRunning), cmd_client,
                          std::ref(g_frame_buffer));
    std::thread btnThread(buttonThread, std::ref(fallRunning));

    // WUW reads strides from the ring via this monotonic cursor. After any
    // pause (call/pain/wake-handling) we jump it to ring.now() so we resume on
    // live audio and never replay the seconds buffered while we were busy.
    uint64_t wuw_cursor = g_audio_ring.now();

    // No audio_buffer — the WakeWordDetector keeps its own 50-frame mel ring
    // buffer (PCENState::mel_buffer), updated each stride. Matches Python's
    // streaming inference recipe.
    //
    // Decision-layer state (matches Python cells 18 & 19):
    //   ema_wake/other/noise : per-class EMAs of the 3-class softmax
    //   schmitt_armed        : false from fire until ema_wake dips below DOWN
    //   cooldown             : hard-backstop frame counter
    //
    // Inference runs every stride regardless of state — keeps the PCEN filter
    // and EMAs continuous. Only the *fire decision* is gated.
    float ema_wake=0.0f, ema_other=0.0f, ema_noise=0.0f;
    int   cooldown=0;
    bool  schmitt_armed=true;

    // ── "Hey" pre-gate state (guest-mode double-gate). In guest mode the "Hey"
    // EMA opens a window; the FULL strict-EMA "Noban" must then fire within
    // HEY_GATE_WINDOW_MS for the wake to count. BOTH models gate on their EMA.
    float ema_hey=0.0f, ema_hey_other=0.0f, ema_hey_noise=0.0f;
    bool  hey_gate_open=false;
    float gate_noban_peak=0.0f;      // max Noban EMA while gate open (diag)
    float gate_noban_raw_peak=0.0f;  // max Noban RAW prob while gate open (diag)
    std::chrono::steady_clock::time_point hey_opened{};
    constexpr int   HEY_GATE_WINDOW_MS = 8000;   // generous room to say "Noban" after "Hey"
    // "Hey" gate uses the SMOOTHED EMA (same as Noban) but with a LOW threshold:
    // "Hey" is short so its EMA only peaks ~0.2-0.37 live (never ~0.6 like a
    // sustained "Noban"). 0.16 sits between that and the idle baseline. Tunable —
    // the gate-open log prints the ema so it's easy to re-tune.
    constexpr float HEY_EMA_UP         = 0.16f;

    // After any pause+resume (wake-fire, pain session, incoming call) the
    // 50-frame mel ring buffer still contains pre-pause frames mixed with
    // fresh ones for ~3 strides (~1 s). Suppress firing during that window
    // so the model sees a coherent context before we trust its decision.
    int post_resume_settle = 0;
    constexpr int POST_RESUME_SETTLE_STRIDES = 3;

    std::cout<<"\n👂 Listening for wake word...\n";
    std::cout<<"   α="<<ALPHA<<"  up="<<THRESHOLD_UP<<"  down="<<THRESHOLD_DOWN
             <<"  margin>"<<MARGIN_MIN<<"  cooldown="<<COOLDOWN_FRAMES<<"\n";
    std::cout<<std::string(45,'=')<<"\n";

    while (fallRunning.load()) {
        // ── State machine: incoming call has priority over WUW. Close the
        // mic so the aiortc helper can open ALSA exclusively; wait for the
        // call to end (helper exits → g_in_call flips back to false).
        if (g_in_call.load()) {
            std::cout << "\n[call] pausing WUW for incoming call.\n";
            // T1 releases micleft for the call helper; we just stop consuming.
            while (g_in_call.load() && fallRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!fallRunning.load()) break;
            // PAUSE-DON'T-RESET: WUW state preserved across the call.
            wuw_cursor = g_audio_ring.now();   // skip audio buffered during the call
            post_resume_settle = POST_RESUME_SETTLE_STRIDES;
            ema_hey = 0.0f; hey_gate_open = false;   // don't leak a stale Hey gate across the pause
            std::cout << "[call] ended — resuming WUW.\n";
            continue;
        }

        // ── Guest/sleep mode no longer pauses WUW. WUW keeps running so a
        // double-gated "Hey … Noban" can still wake the elder for pills/calls/
        // voice; only fall detection + camera are gated off in guest (T4 fall +
        // camera grabber). The wake fire decision is double-gated below.

        // ── State machine: when fall flips 0->1, pause WUW, run pain
        // session, then wait for fall to drop back to 0 before re-arming
        // WUW. Sticky fall=1 will NOT keep retriggering. ────────────────
        if (pain::g_fall.load() != 0) {
            std::cout << "\n[fall=1] pausing WUW, entering pain session.\n";
            // GREEN = recording for the whole pain session (beeps + "speak now"),
            // mirroring the wake-word handling. Eyes go "worried" during the
            // post-fall check. Restored to idle at session end.
            setLed(recColor());
            setOled("worried");
            // Pain reads the SAME ring T1 is filling (T1 keeps the mic). Give it
            // its own cursor starting at "now" so warmup begins on live audio.
            uint64_t pain_cursor = g_audio_ring.now();
            auto pain_reader = [&](std::vector<float>& out, int n) {
                out.clear();
                while (out.empty() && fallRunning.load())
                    pain_cursor = g_audio_ring.read(pain_cursor, (size_t)n, out, fallRunning);
            };
            pain::SessionResult res = pain::runPainSession(pain_pipe, pain_reader);
            std::cout << "[pain] session result: "
                      << pain::sessionResultName(res) << "\n";

            // Map the pain feedback → fall danger colour and publish it BEFORE
            // clearing g_fall, so the fall thread (which sends the event when it
            // sees g_fall==0) stamps the event with the right severity:
            //   PAIN → 2 (red)   FINE → 0 (green)   UNDECIDED/ERROR → 1 (orange)
            int danger = (res == pain::SessionResult::PAIN) ? 2
                       : (res == pain::SessionResult::FINE) ? 0 : 1;
            g_fall_danger_level.store(danger);

            // Auto-clear g_fall now that pain session finished. The fall
            // detector's internal `ignore=5` counter prevents immediate
            // re-fire for the next ~5 batches (~5s @ 16fps/16-batch), giving
            // the person time to recover before WUW would be paused again.
            pain::g_fall.store(0);
            if (!fallRunning.load()) break;

            // Pain session over: back to idle + WUW back on (covers both
            // direct fall->pain and wake-preempted-by-fall->pain paths).
            setLed(idleColor());
            setOled(g_guest_mode.load() ? "sleep" : "idle");
            // PAUSE-DON'T-RESET: WUW state (PCEN M, mel buffer, EMAs, cooldown,
            // schmitt) is preserved. Jump the cursor to live audio (T1 reopens
            // the mic now that g_fall cleared) so we don't replay the session.
            wuw_cursor = g_audio_ring.now();
            post_resume_settle = POST_RESUME_SETTLE_STRIDES;
            ema_hey = 0.0f; hey_gate_open = false;   // don't leak a stale Hey gate across the pause
            std::cout << "\n👂 Listening for wake word...\n"
                      << std::string(45,'=') << "\n";
            continue;
        }

        // Pull the next stride from the ring (T1 produces it). Blocks ≤500ms;
        // empty on timeout (T1 momentarily paused/reopening) → just retry.
        std::vector<float> chunk;
        wuw_cursor = g_audio_ring.read(wuw_cursor, STRIDE_SAMPLES, chunk, fallRunning);
        if (!fallRunning.load()) break;
        if (chunk.size() < static_cast<size_t>(STRIDE_SAMPLES)) continue;

        // Run feature extraction + inference every stride.
        auto features = wwd.computeFeatures(chunk);
        auto p        = wwd.inferFromFeatures(features);

        // Per-class EMAs.
        ema_wake  = ALPHA*p.wake  + (1.0f-ALPHA)*ema_wake;
        ema_other = ALPHA*p.other + (1.0f-ALPHA)*ema_other;
        ema_noise = ALPHA*p.noise + (1.0f-ALPHA)*ema_noise;

        float margin = ema_wake - std::max(ema_other, ema_noise);

        // ── "Hey" pre-gate inference (runs every stride to stay warm). EMAs are
        // kept for diagnostics, but the gate opens on a RAW per-frame "Hey".
        auto hp = hey_wwd.inferFromFeatures(hey_wwd.computeFeatures(chunk));
        ema_hey       = ALPHA*hp.wake  + (1.0f-ALPHA)*ema_hey;
        ema_hey_other = ALPHA*hp.other + (1.0f-ALPHA)*ema_hey_other;
        ema_hey_noise = ALPHA*hp.noise + (1.0f-ALPHA)*ema_hey_noise;
        auto now_t = std::chrono::steady_clock::now();
        // Gate opens on the "Hey" EMA (smoothed, same as Noban) crossing a LOW
        // threshold while it's the dominant class. Low threshold because "Hey" is
        // short (its EMA peaks ~0.2, not ~0.6).
        bool hey_now = (ema_hey > HEY_EMA_UP) && (ema_hey > ema_hey_other) && (ema_hey > ema_hey_noise);
        if (hey_now) {
            if (!hey_gate_open) { printf("\n[hey] gate OPEN (ema=%.2f) — say Noban now\n", ema_hey); gate_noban_peak=0.0f; gate_noban_raw_peak=0.0f; hey_opened=now_t; }
            hey_gate_open = true;
        }
        if (hey_gate_open) {
            gate_noban_peak     = std::max(gate_noban_peak, ema_wake);
            gate_noban_raw_peak = std::max(gate_noban_raw_peak, p.wake);
        }
        if (hey_gate_open && std::chrono::duration_cast<std::chrono::milliseconds>(now_t - hey_opened).count() >= HEY_GATE_WINDOW_MS) {
            printf("\n[hey] window expired — Noban ema peak=%.2f (>%.2f) / raw peak=%.2f (>%.2f)\n",
                   gate_noban_peak, GUEST_NOBAN_UP, gate_noban_raw_peak, NOBAN_GATE_RAW);
            hey_gate_open = false;
        }

        // Schmitt re-arm: once EMA dips below the LOW threshold we're eligible
        // to fire again. This is what stops continuous kid babble from
        // re-firing the moment cooldown expires.
        if (!schmitt_armed && ema_wake < THRESHOLD_DOWN) schmitt_armed = true;

        if (cooldown > 0) cooldown--;

        // Live per-stride meter — only to an interactive terminal. Under systemd
        // there's no TTY, and the trailing '\r' makes journald log every stride
        // as a "[blob data]" entry, flooding the journal many times/sec (which
        // also evicted the one-time "Listening" line and tripped the watchdog).
        if (g_stdout_is_tty) {
            printf("  w=%.2f m=%.2f %s cd=%d\r",
                   ema_wake, margin,
                   schmitt_armed ? "ARM" : "off", cooldown);
            fflush(stdout);
        }

        // Decrement the post-resume settle counter every stride. While > 0,
        // suppress firing — the mel buffer still has stale pre-pause frames.
        if (post_resume_settle > 0) post_resume_settle--;

        bool noban_fired = (cooldown == 0) && schmitt_armed && (post_resume_settle == 0)
                        && (ema_wake > THRESHOLD_UP) && (margin > MARGIN_MIN);
        bool can_fire;
        if (g_guest_mode.load()) {
            // Double-gate: the "Hey" EMA gate opens the window, then "Noban" fires
            // within it. The Hey gate supplies the precision, so the gated Noban
            // uses a slightly LOWER EMA bar (GUEST_NOBAN_UP) — still EMA + margin +
            // Schmitt + cooldown, just enough that a deliberate "Hey … Noban"
            // (which lands ema_wake ~0.56-0.65, right on the strict 0.60) fires
            // reliably instead of half the time.
            bool noban_gated = (cooldown == 0) && schmitt_armed && (post_resume_settle == 0)
                            && (ema_wake > GUEST_NOBAN_UP) && (margin > MARGIN_MIN);
            // Fluid "Hey Noban": when the words run together the EMA never builds
            // (the 1 s window mixes them) but the raw frame prob still spikes.
            // Accept a raw "Noban" peak too — SAFE because it only counts INSIDE
            // the open Hey window, so it can't false-alarm on its own (the reason
            // raw was dropped from normal mode).
            bool noban_raw = (cooldown == 0) && schmitt_armed && (post_resume_settle == 0)
                            && (p.wake > NOBAN_GATE_RAW) && (p.wake > p.other) && (p.wake > p.noise);
            can_fire = hey_gate_open && (noban_gated || noban_raw);
            if (can_fire) hey_gate_open = false;   // consume on success
        } else {
            can_fire = noban_fired;   // normal mode: single strict-EMA Noban (0.60)
        }
        if (can_fire) {
            schmitt_armed = false;
            cooldown      = COOLDOWN_FRAMES;
            printf("\n\n🔔 WAKE WORD DETECTED! (ema_w=%.3f  margin=%.3f)\n",
                   ema_wake, margin);

            // GREEN (recording) for the entire wake handling — turns back to
            // idle at the very end (or when fall preempts). Eyes: "happy" when
            // it hears its name normally; in guest it just woke from sleep on
            // "Hey Noban" so it's "listen"ing.
            setLed(recColor());
            setOled(g_guest_mode.load() ? "listen" : "excited");
            // T1 keeps owning micleft and filling the ring; recordCommand reads
            // the post-wake audio straight from it — no mic open/close churn.
            playAlarm();
            // Let the beep tail clear so the recorded command/voice message
            // doesn\'t start with the alarm beep (recordCommand starts at now()).
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            // Fall preempts wake at every breakpoint. recordCommand polls the
            // predicate between 100ms chunks; the other steps check inline.
            auto fall_pending = []{ return pain::g_fall.load() != 0; };
            bool preempted = false;

            auto recorded = recordCommand(g_audio_ring, fallRunning, fall_pending);
            if (fall_pending()) { preempted = true; }

            if (!preempted) { playAlarm(); if (fall_pending()) preempted = true; }
            if (!preempted) {
                // Send the RAW recorded audio — NO VAD, NO denoise. Silero VAD
                // concatenated speech segments → discontinuity glitches; and the
                // RNNoise denoiser runs at the wrong rate (it expects 48 kHz, our
                // capture is 16 kHz) → spectral distortion. Whisper is robust to
                // background noise, and the caregiver hears the true audio. This
                // is cleaner for both STT and the stored voice message.
                ServerCommandClient::WakeIntent intent =
                    ServerCommandClient::WakeIntent::UNKNOWN;
                cmd_client->sendWakeAudio(recorded, &intent);
                if (intent == ServerCommandClient::WakeIntent::PILL) {
                    // The server marks the active pill(s) consumed atomically
                    // inside /Wake_Command — no separate ack round-trip. The
                    // alarm self-silences once the server drops it from the next
                    // heartbeat's active_pill_alarms.
                    std::cout << "[wake] intent=PILL — server marked consumed\n";
                } else if (intent == ServerCommandClient::WakeIntent::CALL) {
                    std::cout << "[wake] intent=CALL — caregivers notified\n";
                } else if (intent == ServerCommandClient::WakeIntent::MESSAGE) {
                    std::cout << "[wake] intent=MESSAGE — audio stored\n";
                }
            }

            if (preempted) {
                std::cout << "[wake] preempted by fall — handing off to pain\n";
                // Fall through; the top-of-loop pain branch picks it up next
                // iteration. LED stays RED until pain session finishes; the
                // pain branch turns it back to GREEN and resumes WUW.
            } else {
                // Idle = wake-handling done AND WUW pipeline back on
                // (BLUE normal / RED guest). Eyes go back to open (idle) or,
                // if we're in guest/sleep, back to sleeping.
                setLed(idleColor());
                setOled(g_guest_mode.load() ? "sleep" : "idle");
            }

            // PAUSE-DON'T-RESET: preserve PCEN M, mel buffer, EMAs.
            // The cooldown was already set to COOLDOWN_FRAMES above when we
            // fired; it'll continue counting down as new strides arrive.
            // EMAs are also preserved — by the time cooldown expires (~2 s),
            // they've decayed naturally since no inference ran during the
            // 5+ s wake handling window.

            // Resume on live audio (T1 has been filling the ring throughout the
            // wake handling); skip the buffered handling-window seconds.
            wuw_cursor = g_audio_ring.now();
            post_resume_settle = POST_RESUME_SETTLE_STRIDES;
            ema_hey = 0.0f; hey_gate_open = false;   // don't leak a stale Hey gate across the pause
            std::cout<<"\n👂 Listening for wake word...\n"<<std::string(45,'=')<<"\n";
        }
    }
    fallRunning = false;
    if (btnThread.joinable()) btnThread.join();
    if (audioCap.joinable())  audioCap.join();
    if (houseKeep.joinable()) houseKeep.join();
    if (fallProc.joinable())  fallProc.join();
    if (camGrab.joinable())   camGrab.join();
    return 0;
}
