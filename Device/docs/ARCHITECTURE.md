# Noban Device — Architecture & Function Reference

This is the developer reference for the C++ pipeline in `Data/Device/Noban/`.
It maps the runtime to the source: the thread topology, each pipeline stage,
each file's responsibility with `file:line` anchors, the model table, the
systemd topology, and the MQTT/HTTP/call wire protocols.

All line references are to the files as they exist in this repo.

---

## 1. Process & thread topology

`main()` (`main.cpp:2517`) loads all models once, then spawns five concurrent
contexts (the "dedicated-capture" architecture, `main.cpp:2676-2696`):

| Thread | Function | Owns / does |
|---|---|---|
| **T1 audio capture** | `audioCaptureThread` (`main.cpp:1991`) | Sole owner of `micleft`. Reads `STRIDE_SAMPLES` chunks back-to-back into the shared `AudioRing`. Releases the mic for calls (`g_in_call`), self-heals on xrun. |
| **T2 camera grabber** | `cameraGrabberThread` (`main.cpp:1915`) | Owns `cv::VideoCapture` on `/dev/video9`, fills `FrameBuffer`, flips frames vertically, releases the camera during calls. |
| **T3 main thread** | the `while` loop in `main()` (`main.cpp:2763`) | WUW + pain state machine; **consumes** the AudioRing. Highest-priority state: call > fall/pain > WUW. (Sleep/guest does **not** pause WUW — it only double-gates the wake and gates off fall+camera.) |
| **T4 fall pipeline** | `fallPipelineThread` (`main.cpp:2201`) | Reads `FrameBuffer`, runs the 6-stage fall detector in 16-frame batches, records the fall A/V clip, sends the fall Event after the pain session. |
| **T5 housekeeping** | `housekeepingThread` (`main.cpp:2060`) | `/Live_Device` heartbeat every 100 ms: snapshots, pill alarms, voice notes, incoming-call launch, remote guest toggle. |
| **T6 button** | `buttonThread` (`main.cpp:2033`) | ~66 Hz button poller → gestures (split out so a slow heartbeat curl can't smear taps). |

Shared state (atomics + the ring/frame buffers):
- `AudioRing` (`main.cpp:341`) — 20 s float ring; one producer (T1), many
  consumers (WUW cursor read, `recordCommand`, pain reader, fall-clip `slice`).
- `FrameBuffer` (`main.cpp:1885`) — latest camera frame + seq/stamp.
- `g_in_call` (`main.cpp:1512`), `g_mic_free` (`main.cpp:1519`),
  `g_guest_mode` (`main.cpp:163`), `pain::g_fall` (`fall_detection`/declared in
  `pain_pipeline.hpp:162`), `g_fall_danger_level` (`main.cpp:1525`).

### Coordination rules
- **Mic handoff**: a call releases the mic; T5 sets `g_in_call`, waits for T1 to
  publish `g_mic_free` before launching the call helper (`main.cpp:2170-2179`).
  A fall/pain session does **not** release the mic — pain reads the same ring
  T1 keeps filling.
- **Fall preempts wake** at every breakpoint in wake handling
  (`main.cpp:2866-2899`).
- **Pause-don't-reset**: after any pause (call/pain/wake — **not** guest, which
  no longer pauses WUW), WUW state (PCEN M, mel buffer, EMAs, Schmitt, cooldown)
  is preserved; only the ring cursor jumps to `now()`, the "Hey" gate is cleared
  so it can't leak stale, and a 3-stride settle suppresses firing while the mel
  buffer re-fills (`main.cpp:2774-2777, 2828-2830`).

---

## 2. Wake-word (WUW) pipeline

The whole live path runs on T3, one `STRIDE_SAMPLES` (4800 = 300 ms) stride at
a time (`main.cpp:84`).

### Stage 1 — capture
T1 reads from ALSA (`openMicHandle` `main.cpp:284`, `readMicStride`
`main.cpp:322`), scales S32 samples by `MIC_SCALE` (`1/2^27`,
`main.cpp:271-278`), writes into `AudioRing`. T3 pulls a stride via
`AudioRing::read` with a monotonic `wuw_cursor` (`main.cpp:2815`).

### Stage 2 — features: mel + PCEN (streaming)
`WakeWordDetector::computeFeatures` (`main.cpp:680`) → `extractFeatures`
(`main.cpp:481`):
- `buildMelFilterbank` (`main.cpp:412`) — **Slaney** mel scale + Slaney area
  norm (matches librosa, not HTK).
- `hannWindow` (`main.cpp:454`) — periodic Hann (`fftbins=True`).
- pocketfft real FFT (`N_FFT=512`, `HOP=320`, 192-sample overlap carried in
  `PCENState::lookback`, `main.cpp:466-474`).
- Streaming **PCEN** AGC with carried per-bin IIR state `M`; rolling 50-frame
  (= 1 s) mel buffer. Constants `main.cpp:102-106` (`α=0.90, δ=2.0, r=0.5,
  time_c=0.10, eps=1e-3`). Output is `(50×32)` flattened.
- `warmUp` (`main.cpp:664`) zero-initializes PCEN state ("silence-warmed").

### Stage 3 — DS-CNN inference
**Two** DS-CNN models run, both quantized int8 with the same I/O `(50×32)` →
3 class probs `{wake, other, noise}` (`WakeProbs`, `main.cpp:633`), both loaded
once at startup and inferred **every** stride to keep PCEN/EMAs continuous:
- The **primary "Noban"** model `noban_int8.tflite` (`TFLITE_MODEL`,
  `main.cpp:107`; loaded in `WakeWordDetector::init`, `main.cpp:642`) via
  `inferFromFeatures` (`main.cpp:687`).
- The **"Hey" pre-gate** model `hey_int8.tflite` (`HEY_MODEL`, `main.cpp:108`;
  second `WakeWordDetector hey_wwd`, `main.cpp:2613`). Used only for the
  sleep-mode double-gate (Stage 4); inferred every stride to stay warm
  (`main.cpp:2856`).

### Stage 4 — decision layer
In the T3 loop (`main.cpp:2847-2899`):
- Per-class EMAs (`ALPHA=0.3`, `main.cpp:90`) for both the "Noban" and "Hey"
  models.
- `margin = ema_wake − max(ema_other, ema_noise)`.
- **Strict "Noban" fire gate** (`noban_fired`, `main.cpp:2891`):
  `cooldown==0 && schmitt_armed && settle==0 && ema_wake>0.60 && margin>0.20`
  (`THRESHOLD_UP`, `MARGIN_MIN`, `main.cpp:91-94`).
- Schmitt re-arm: must dip below `THRESHOLD_DOWN=0.30` before re-firing
  (`main.cpp:2878`) — kills continuous-babble re-triggers.
- `COOLDOWN_FRAMES=7` (~2.1 s) hard backstop.
- **Normal (awake) mode**: `noban_fired` alone fires the wake — a single
  strict-EMA "Noban".
- **Sleep/guest mode — double-gate** (`main.cpp:2894-2899`): firing also
  requires the "Hey" gate to be open. The smoothed "Hey" EMA crossing a **low**
  threshold (`HEY_EMA_UP=0.16`, `main.cpp:2749`) while it dominates its own
  classes opens an **8 s window** (`HEY_GATE_WINDOW_MS`, `main.cpp:2744`); the
  full strict "Noban" gate (identical to normal mode) must then fire inside it.
  The threshold is low because "Hey" is short — its EMA only peaks ~0.2, never
  ~0.6 like a sustained "Noban". Net effect: in sleep the elder must say
  **"Hey … Noban"**. Both EMAs run in parallel every stride. The Hey gate is
  cleared on any pause/resume so it can't leak stale across a call/pain/wake.

### Stage 5 — on fire: record → server round-trip
`main.cpp:2850-2920`:
1. RED LED, `playAlarm` beep (`main.cpp:1333`), 300 ms tail wait.
2. `recordCommand` (`main.cpp:1401`) pulls 5 s of post-wake audio from the ring
   in 100 ms slices, polling a `fall_pending` predicate so a fall preempts it.
3. Second beep, then `sendWakeAudio` (`main.cpp:816`) POSTs the **raw** clip
   (no VAD/denoise — see `main.cpp:2876-2881`) to `/Wake_Command`. The server
   returns an `intent` (PILL/CALL/MESSAGE); the device just logs it (pill is
   marked consumed server-side; the alarm self-silences when the next heartbeat
   drops it).

`SileroVAD` (`main.cpp:709`) + `denoise` (`main.cpp:1426`) + `applyVAD`
(`main.cpp:1441`) exist but are **not** on the live fire path; they are helpers
retained for offline/experimental use.

---

## 3. Fall detection → pain pipeline (camera path)

### Fall detection (T4, `fallPipelineThread` `main.cpp:2201`)
`fall::FallDetection` (`fall_detection.hpp:78`) is a line-for-line port of the
Python `fall_detection.py`. Per 16-frame batch (`frame_inference`,
`fall_detection.hpp:94`):
1. Motion-aware background (`ExtractBackground`, `extract_background.hpp:17`) —
   16×16 grid; a cell still for 5 frames is copied into the static background.
2. Coarse batch bbox over 4 downsampled frames (`extract_approximate_batch_bbox`).
3. Per-frame fine bbox inside the crop (`extract_main_bboxes`).
4. Interpolate missing + 13-tap Gaussian smooth across time
   (`interpolate_and_smooth_bboxes`).
5. 80-dim relative feature → **SVM** (`SVM_rbf.onnx`, `svm_predict`).
6. Two postprocess windows `out1→out2→out3`.
7. On an all-1 batch: build a 416-dim MoVeNet-keypoint feature (`4.tflite` via
   `get_movenet_keypoints`), run the **MLP confirmer** (`Last_stage_Model.onnx`,
   `split_features` → `[1,15,16,2]`); fire if `final_predict > 0.9`. Gated by
   `batch_number >= 7`.

On a confirmed fall T4 captures the audio cursor + thumbnail, sets
`pain::g_fall = 1`, and starts recording the whole event to disk
(`main.cpp:2341-2363`). `insertGapSentinel` (`fall_detection.hpp:122`) is called
on resume after a call/guest pause so smoothing won't bridge the gap.

### Pain session (T3, `runPainSession` `pain_pipeline.cpp:724`)
When T3 sees `g_fall != 0` (`main.cpp:2767`) it pauses WUW, sets RED, and runs
the pain session against the shared ring (a `PainAudioReader` lambda,
`main.cpp:2775-2779`):
1. Warmup `0 s` (pain features are stateless log-mel, no PCEN convergence
   needed — `pain_pipeline.hpp:52-64`).
2. Alarm: 3 × 300 ms beeps on a background thread while still draining capture
   (`pain_pipeline.cpp:768-787`), then skip 300 ms beep tail.
3. Score every 300 ms over a 5 s window; EMA `α=0.4`; cascade `PainPipeline::score`
   (`pain_pipeline.cpp:659`):
   - YAMNet scream ≥ 0.30 → PAIN (scream branch).
   - else YAMNet speech ≥ 0.30 → run the `pain_logmel5` specialist; ≥ 0.36 → PAIN.
   - else NEITHER.
   (`LogMel5Extractor`, `pain_pipeline.hpp:126`; 50×32×5 log-mel + delta features.)
4. Sustained-EMA decision (`pain_pipeline.cpp:830-858`): PAIN / FINE / UNDECIDED
   (`SUSTAINED_N=3`, `PAIN_THRESHOLD=0.30`, `FINE_THRESHOLD=0.10`).

T3 maps the result to a danger level — PAIN=2 (red), FINE=0 (green),
UNDECIDED/ERROR=1 (orange) — stores it in `g_fall_danger_level`, then clears
`g_fall` (`main.cpp:2788-2796`). T4 sees `g_fall==0`, cuts the matching
continuous audio slice from the ring, muxes it with the recorded video
(`muxClipFile`, `main.cpp:1858`), and sends **one** encrypted fall Event
stamped with the danger color (`main.cpp:2274-2293`).

---

## 4. Source file responsibilities

| File | Responsibility | Key symbols |
|---|---|---|
| `main.cpp` | Entry point, config, all threads, WUW loop, ALSA capture, mel/PCEN, decision layer, HTTP client, alarms/LED/button, camera+fall orchestration, CLI debug modes | `main` (2517), `ServerCommandClient` (794), `WakeWordDetector` (635), `extractFeatures` (481), `AudioRing` (341) |
| `pain_pipeline.{hpp,cpp}` | Pain cascade (YAMNet router + log-mel5 specialist) + post-fall session loop | `PainPipeline::score` (cpp:659), `runPainSession` (cpp:724), `YamnetRouter`/`SpeechSpecialist`/`LogMel5Extractor` |
| `fall_detection.{hpp,cpp}` | 6-stage fall detector (bg → bbox → smooth → SVM → postprocess → MoVeNet+MLP), port of `fall_detection.py` | `FallDetection::frame_inference` (hpp:94) |
| `extract_background.{hpp,cpp}` | 16×16 grid motion-aware static-background extractor | `ExtractBackground::update_bg` |
| `mqtt_client.{hpp,cpp}` | Device-side MQTT subscriber (pill/call/image state); no-op stub if paho absent | `mq::Client` (hpp:38) |
| `media_crypto.{hpp,cpp}` | XChaCha20-Poly1305 E2E media encryption (salt‖nonce‖ciphertext), mirrors server/app | `mc::encrypt_for_subscriber` (hpp:39) |
| `gpio_io.{hpp,cpp}` | libgpiod v2 button + RGB LED; compiles to no-op if libgpiod absent | `gpioio::init/button_pressed/led` |
| `aec_wrap.cpp` | C shim around WebRTC AudioProcessing (AEC3) for the call helper; built into `libaecwrap.so` | `aec_create/aec_reverse/aec_near` |
| `parity_dump.hpp` | Compile-gated tensor dump for Python-parity tests (`DUMP_TENSORS`) | `dumpTensorF32` |
| `wake_call_helper.py` | WebRTC call client (Janus SFU via HTTP/long-poll); `--serve` warm daemon | `run_call`, `serve`, `Janus` |
| `ble_provisioning.py` | BLE GATT Wi-Fi + identity provisioning sidecar | GATT service `1111…555555555555` |
| `wifi_setup_server.py` | Legacy AP-mode HTTP provisioning server (`/scan`, `/connect`) | `Handler` |
| `show_noban.py` | One-shot OLED brand display (I2C 0x3C) | `Oled.show_centered` |
| `record_samples.py` | INMP441 sample collection for WUW retraining (`~/samples/`) | `wake_mode`, `general_mode` |

Important nuance: `mqtt_client` is **constructed** in `main()` (`main.cpp:2572`)
but the live loop still drives everything off the HTTP `/Live_Device` heartbeat;
MQTT is wired as the Phase 5.1 transport and runs in parallel as a safety net,
its cached state not yet consumed by the loop.

---

## 5. Models

| File | Format / I/O | Loaded in | Role |
|---|---|---|---|
| `models/noban_int8.tflite` | TFLite int8, in `(50×32)` → 3 probs | `WakeWordDetector::init` (`main.cpp:642`) | **Primary** "Noban" wake word (DS-CNN-QAT); fires in both modes |
| `models/hey_int8.tflite` | TFLite int8, in `(50×32)` → 3 probs | `hey_wwd.init` (`main.cpp:2613`) | "Hey" pre-gate (DS-CNN); opens the double-gate window in sleep mode only |
| `models/silero_vad.onnx` | ONNX, audio+state → speech prob | `SileroVAD::init` (`main.cpp:713`) | VAD (helper, off live path) |
| `models/yamnet.tflite` | TFLite f32, `15600` samples → 521 classes | `YamnetRouter::init` (`pain_pipeline.cpp:83`) | Pain router (scream/speech) |
| `models/pain_logmel5.tflite` | TFLite int8, `(50×32×5)` → sigmoid | `SpeechSpecialist::init` (`pain_pipeline.cpp:119`) | Pain speech specialist |
| `4.tflite` | TFLite, `[1,192,192,3]`u8 → `[1,1,17,3]` | `FallDetection` ctor | MoVeNet keypoints |
| `SVM_rbf.onnx` | ONNX, `X[16,80]` → `[16,2]` | `FallDetection` ctor | Fall SVM stage |
| `Last_stage_Model.onnx` | ONNX, `[1,15,16,2]` → prob | `FallDetection` ctor | Fall MLP confirmer |

TFLite is pinned to **r2.13** (model trained on TF 2.13; newer kernels drift
int8 — `CMakeLists.txt:150-160`).

---

## 6. systemd topology
See SETUP.md §5 for install. Summary:
- `elderly-camera.service` → `run_camera.sh` → `/dev/video9`.
- `elderly-pipeline.service` → `build/pipeline` (owns mic/GPIO/LED/camera-read);
  `oled.conf` drop-in runs `show_noban.py` post-start.
- `elderly-call-daemon.service` → `wake_call_helper.py --serve` (warm aiortc).
- `noban-watchdog.timer` → `noban-watchdog.service` → `noban-watchdog.sh`
  (reboot once if WUW never reaches "Listening"; **script not in repo**).

---

## 7. Wire protocols

### MQTT (`mqtt_client.cpp`, broker `tcp://<MQTT_HOST>:1883`)
Client id `device-<id>`; mirrors server's `mqtt_pub.py`.

| Topic | Direction | Payload |
|---|---|---|
| `device/<id>/state/pill` | subscribe (QoS1, retained) | `{"active_pill_alarms":[{id,pill_id,name,audio_url,dose,of}]}` |
| `device/<id>/state/call_pending` | subscribe | `{"pending":bool,"in_call_by":str}` |
| `device/<id>/state/image_req` | subscribe | `{"req_id":int}` |
| `device/<id>/online` | publish (retained) + LWT | `"1"` on connect, `"0"` on clean disconnect / LWT on drop |

Connect opts: `clean_session=false`, keepalive 30 s, auto-reconnect
(`mqtt_client.cpp:84-117`).

### HTTP endpoints the device calls
Base = `SERVER_URL` env (else `SERVER_BASE_URL`, `main.cpp:126/2601`). All carry
`Device-Id` + `Key-Hash` (SHA-256 of password) headers; some add `Device-Mac`,
`Guest-Mode`.

| Method · Endpoint | Caller (file:line) | Purpose |
|---|---|---|
| `POST /Wake_Command` (audio/wav) | `sendWakeAudio` (816) | Post-wake clip → STT + intent (PILL/CALL/MESSAGE) |
| `GET /Live_Device` | `sendLivePacket` (902) | 100 ms heartbeat → `image_req[_id]`, `call_pending`, `guest_set`, `voice_msg_url`, `active_pill_alarms` |
| `POST /Pill_Ack_Device` | `sendPillAck` (1067) | Acknowledge a pill schedule |
| `GET /Pill_Audio/<id>` | `fetchPillAudio` (1096) | Fetch pill reminder WAV → `/tmp` |
| `GET <voice_msg_url>` | `fetchVoiceMsg` (1131) | Fetch caregiver voice note → `/tmp` |
| `POST /Message_Device` (multipart) | `postMessageDevice` (1209) via `sendImageMessage` (1162) / `sendEventPacket` (1170) | Snapshot ("image captured") or fall "Event" (thumbnail + A/V clip + Danger Level), E2E-encrypted |
| `POST /Fall_Clip_Replace_Device` (multipart) | `postFallClipReplace` (1178) | Replace newest fall Event's video with finished A/V clip |
| `POST /Factory_Reset_Device` | `doFactoryReset` (1690) | Confirm server purged this device before local wipe |
| `GET /Live_Device` | `wake_call_helper.py` | Helper polls `call_pending` to detect call end |

### Call flow (Janus WebRTC)
1. App posts a Call_Offer to the server; the device sees `call_pending:true` in
   the heartbeat (`main.cpp:2170`).
2. T5 sets `g_in_call`, waits for `g_mic_free` (T1 releases mic) + camera
   release, then shells the helper (`main.cpp:2181-2190`):
   `wake_call_helper.py` (warm daemon via `--serve` if running, else direct).
3. The helper (`aiortc`) joins the device's Janus VideoRoom (HTTP/long-poll API,
   deterministic room = CRC32 of device_id) as a **publisher** (camera +
   `micleft` mic, vertically flipped) and **subscribes** to the caregiver's
   audio feed → speaker amp. Env: `MIC_DEVICE`, `SPEAKER_DEV`, `CAM_DEVICE`,
   `MIC_FILTER` (`'none'` for the clean INMP441 — `main.cpp:2187`).
4. The helper polls `/Live_Device` until `call_pending` clears, then exits.
5. T5 clears `g_in_call`; T1 reopens the mic, T2/T4 reopen the camera (with a
   gap sentinel), T3 resumes WUW.

AEC: the warm daemon runs with `AEC_MODE=off` by default
(`elderly-call-daemon.service`); `aec_wrap.cpp` / `libaecwrap.so` provide AEC3
as an optional path (Janus relay carries two-way audio without it).

---

## 8. CLI / debug modes
Handled at the top of `main()` before the heavy startup:
- `--wuw-wav <file>` (`main.cpp:2533`) → `runWuwWavDebug` (`main.cpp:2379`):
  stream a WAV through the exact live WUW path, print per-stride `raw_wake`.
  Combine with `MIC_SCALE_POW` to sweep the mic scale offline.
- `--parity-fixtures <dir>` (`main.cpp:2609`) → `runParityFixtures`
  (`main.cpp:2438`): tensor-dump parity vs Python references (DUMP_TENSORS build).
- `--fall-video <mp4> [--fall-loops N]` (`main.cpp:2612`): run the fall detector
  on a clip, write `/tmp/fall_cpp.json` per-batch state.
