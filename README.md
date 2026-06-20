# Wake-Up

An on-device **elderly-care assistant** built around a custom wake word. A Raspberry Pi
listens for *"Hey Noban"*, understands spoken commands, watches for falls and signs of pain,
and connects the elder to a caregiver over two-way video — coordinated by a self-hosted
server and a caregiver phone app.

Everything here was built and trained from scratch: the wake-word model, the audio/vision
pipeline on the device, the inference server, and the mobile app.

---

## What it does

- **Custom wake word** — a two-stage *"Hey Noban"* gate (DS-CNN, quantization-aware trained)
  running fully on the Pi, tuned for a far-field MEMS microphone.
- **Voice commands** — after the wake word, audio is transcribed (Whisper large-v3) and an
  LLM classifies intent: take a pill, call family, sleep/wake the device, or a general message.
- **Fall detection** — on-device pose estimation (MoveNet) flags falls and uploads a short clip.
- **Pain / distress detection** — ambient audio classification (YAMNet) + a speech specialist.
- **Two-way video calls** — caregiver ↔ device, relayed through a self-hosted SFU (Janus + TURN).
- **Sleep / wake modes** — a double-gated voice command pauses monitoring without going offline.
- **Caregiver app** — alerts, pill tracking, fall videos, live device status, and calls.

---

## Architecture

```
   ┌─────────────────────┐       Wi-Fi / LAN        ┌──────────────────────┐
   │   Device (Pi, C++)   │ ───── heartbeat/MQTT ──► │   Server (FastAPI)    │
   │  • wake word (TFLite)│ ◄──── commands/calls ─── │  • Whisper + LLM (GPU)│
   │  • fall (MoveNet)    │                          │  • Postgres / Redis   │
   │  • pain (YAMNet)     │ ◄──── video call ──────► │  • MQTT broker        │
   │  • OLED / LED / amp  │      (Janus SFU/TURN)    │  • Janus SFU + coturn │
   └─────────────────────┘                          └───────────┬──────────┘
                                                                 │ FCM push + REST
                                                                 ▼
                                                     ┌──────────────────────┐
                                                     │  App (Flutter)        │
                                                     │  caregiver phone       │
                                                     └──────────────────────┘
```

---

## Repository layout

| Folder | What it is |
|--------|------------|
| **`Device/`** | Raspberry Pi pipeline (C++): wake word, fall & pain detection, calls, OLED/LED, MQTT, server heartbeat. Setup: `Device/` setup scripts + README. |
| **`Server/`** | Async FastAPI backend: REST API, GPU wake-command worker (Whisper + LLM), Postgres/Redis/MQTT, Janus SFU + coturn for calls, FCM notifications. See `Server/README.md`. |
| **`App/`** | Flutter caregiver app: alerts, pills, fall videos, device status, video calls. |
| **`Train/`** | `Ds-CNN-QAT.ipynb` — the quantization-aware training notebook for the wake-word model. |

Each component keeps its own README / setup docs. Server hardware & internet sizing estimates
live in **`Server/docs/SERVER_SIZING.md`**.

---

## Tech stack

- **Device:** C++, TensorFlow Lite (DS-CNN, MoveNet, YAMNet), ALSA/i2s, MQTT, libcurl, aiortc (calls)
- **Server:** Python, FastAPI (async), faster-whisper (large-v3), llama.cpp (Qwen2.5-7B), PostgreSQL, Redis, Mosquitto, Janus, coturn
- **App:** Flutter / Dart, flutter_webrtc, Firebase Cloud Messaging
- **Training:** TensorFlow / Keras (quantization-aware training → INT8 TFLite)

---

## Trained models

The hand-trained on-device models are included under `Device/Noban/models/`
(`noban_int8.tflite`, `hey_int8.tflite` — the wake-word gate). The large server-side models
(Whisper, the LLM) are **not** committed; the Server fetch script downloads them.

---

## Setup

1. **Server** — follow `Server/README.md` (local-LAN and Docker paths). Supply the not-in-repo
   files listed in `Server/REQUIRED_FILES.md` (Firebase key, SMTP/DB secrets) and fetch the models.
2. **Device** — flash a Pi, run the device setup script, point it at the server.
3. **App** — provide your own `google-services.json`, then build with the server URL.

> **Secrets are not in this repo.** Credentials (Firebase admin key, SMTP password, DB password)
> are git-ignored and must be supplied locally — see `Server/REQUIRED_FILES.md`. The Firebase
> *Android* config (`App/android/app/google-services.json`) is client-side config; restrict the
> key in the Google Cloud console for production.
