# /Wake_Command server route

Pipeline that the Pi POSTs its 5-second post-wake clip to. The server runs
STT + LLM intent classification on the audio and routes the result.

```
Pi  --POST audio/wav-->  /Wake_Command
                              |
                  +-----------+-----------+
                  v                       v
            faster-whisper          (silence -> MESSAGE)
            large-v3-turbo
                  |
                  v   Persian transcript
            Qwen-2.5-14B-Instruct (grammar-constrained)
                  |
                  v   one of PILL | CALL | MESSAGE
            +-----+-----+-----+
            v           v     v
          PILL        CALL  MESSAGE
            |           |     |
            v           v     v
       PillAck     CallRequest  Voice
       row + FCM   row + FCM    audio saved + row + FCM
```

## Models

Both models are downloaded once and read from disk. Defaults:

```
Server/models/whisper-large-v3-turbo/    (CTranslate2 format, ~1.5 GB)
Server/models/qwen2.5-14b-instruct-q4_k_m.gguf   (~8.5 GB)
```

Override with env vars (`WAKE_WHISPER_DIR`, `WAKE_LLM_GGUF`) if you keep them
elsewhere.

### Download

```bash
mkdir -p Server/models && cd Server/models

# Whisper large-v3-turbo (CTranslate2 quantized) — pulled via HuggingFace
pip install huggingface_hub
huggingface-cli download Systran/faster-whisper-large-v3-turbo \
    --local-dir whisper-large-v3-turbo --local-dir-use-symlinks False

# Qwen-2.5-14B-Instruct q4_K_M (GGUF) — ~8.5 GB
wget https://huggingface.co/Qwen/Qwen2.5-14B-Instruct-GGUF/resolve/main/qwen2.5-14b-instruct-q4_k_m.gguf \
     -O qwen2.5-14b-instruct-q4_k_m.gguf
```

## Install Python deps (conda `app` env)

```bash
conda activate app
pip install -r Server/requirements-wake.txt
```

If your CUDA is 11.x (not 12.x), edit `requirements-wake.txt` and switch the
extra-index-url tag from `cu122` to your version (e.g. `cu118`).

## Hardware tuning

Targeted at NVIDIA 3050 8 GB + 64 GB RAM. The defaults put both models on
GPU with ~32 / 48 LLM layers offloaded. If you OOM, lower
`WAKE_LLM_GPU_LAYERS`:

```bash
WAKE_LLM_GPU_LAYERS=28 python myserver.py
```

If you have a bigger card (e.g. 16 GB), bump it to 48 for full-GPU LLM and
better latency.

## Latency

Round-trip, 5-s clip, 3050 + 64 GB rig:

| stage | time |
|---|---|
| HTTP upload (LAN) | ~50 ms |
| whisper-large-v3-turbo (int8_float16) | ~100 ms |
| Qwen-14B q4_K_M, partial offload, 4 output tokens | ~1.0–1.5 s |
| DB write + FCM push | ~50 ms |
| **end-to-end** | **~1.2–1.7 s** |

## Tuning the intent prompt

If you ever want to add a fourth intent (say `EMERGENCY`), edit
`wake_command.py`:

1. Add the line to `SYSTEM_PROMPT` with at least 2 Persian examples
2. Extend the grammar:
   ```
   LLM_GRAMMAR = 'root ::= "PILL" | "CALL" | "MESSAGE" | "EMERGENCY"'
   ```
3. Add a matching `elif intent == "EMERGENCY":` branch in `/Wake_Command`

Restart the server. No retraining needed.

## Quick test

```bash
# from the Pi (or any host with curl):
curl -X POST \
     -H "Content-Type: audio/wav" \
     -H "Device-ID: ID_12233945" \
     -H "Authentication-Code: 1223344556677889" \
     --data-binary @sample.wav \
     http://<server>:5000/Wake_Command
```

Reply:
```json
{"intent": "PILL", "transcript": "آره قرصمو خوردم", "message_id": 42}
```

## Production deployment (concurrent requests)

`python myserver.py` uses Flask's dev server — single-threaded, fine for
testing only. For production switch to Gunicorn:

```bash
pip install gunicorn
cd Server
gunicorn -w 1 --threads 8 --timeout 120 -b 0.0.0.0:5000 myserver:app
```

Why these settings:

| flag | value | reason |
|---|---|---|
| `-w 1` (workers) | **1** | Whisper + Qwen-14B sit in GPU memory. Multiple workers would each try to load their own copy → instant OOM. ONE worker, multiple threads. |
| `--threads 8` | 8 | All non-`/Wake_Command` routes are I/O- or DB-bound and run concurrently across threads (Python's GIL is released around file/socket/SQLite ops). 8 covers heartbeats + logins + signaling at hundreds of devices comfortably. |
| `--timeout 120` | 2 min | `/Wake_Command` itself takes ~1.5 s; under load N requests pile up at the GPU lock so 8th-in-line waits ~12 s. Keep the timeout generous. |

### Concurrency model

```
N concurrent app/Pi clients
       │
       ▼
gunicorn worker (1 process, 8 threads)
       │
       ├── DB / file / signaling routes — run in parallel across threads
       │
       └── /Wake_Command — wake_command._GPU_LOCK serializes GPU work
                    │
                    ▼
              whisper-turbo (50–150 ms)
              Qwen-14B q4   (1.0–1.5 s)
```

Under sustained load, /Wake_Command throughput is ~0.6 req/s (one request
clears every ~1.6 s). For an elderly-care system with 200 devices each
firing ~3 times/hour, that's 0.17 req/s on average — 70% headroom. Fine.

If you ever push past ~30 concurrent active devices and queue depth becomes
visible, the next step would be switching the LLM to vLLM (continuous
batching, ~10× LLM throughput) — but that's a separate-process refactor.

### Models stay loaded forever

The first request after server start triggers `wake_command.warm_up()`
(already called in `if __name__ == '__main__'`). Whisper takes ~3 s to
load, Qwen takes ~8 s. After that they live in VRAM for the process
lifetime. Restart only when you change models or the GGUF file on disk.
