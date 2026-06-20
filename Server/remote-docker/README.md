# Noban — single-box production deployment

One server, one GPU. Architecture:

```
 devices ─┐                         ┌─ Postgres (state)
 app ─────┤  API (N uvicorn workers)├─ Redis  (wake-job queue + shared call/lock state)
          │        │  enqueue        └─ mosquitto (MQTT push)
          │        ▼
          │   wake:jobs ──► GPU worker (whisper + LLM)  ◄── the ONE GPU
          │
 calls ───┴──► Janus SFU (+ coturn)  ── forwards RTP, no transcoding
```

The **API holds no models** — it enqueues wake-commands to Redis and a separate
**GPU worker** processes them. That decoupling is what makes it scale: scale the
**worker** for transcription load, scale **API workers** for HTTP load,
independently. Bursts queue instead of serializing.

## Deploy (Docker)
```bash
cd remote-docker
cp ../models -r ./models                 # or set MODELS_DIR to the models path
# edit: DB_PASSWORD, coturn.conf (PUBLIC_IP + secret), janus/janus.jcfg nat_1_1_mapping
docker compose up -d --build
```
`restart: unless-stopped` on every service = supervision (#5): crash → auto-restart,
reboot → auto-start. Postgres now auto-recovers (was `Restart=no`).

## Scaling knobs (env)
| var | meaning | default |
|---|---|---|
| `API_WORKERS` | uvicorn workers (CPU/HTTP scaling — this DOES scale) | 4 |
| `GPU_WORKERS` | **fixed at 1** on 6 GB (only one model copy fits) | 1 |
| `GPU_LAYERS`  | LLM offload — **20** for 7B (partial), 99 for a 3B/1.5B | 20 |
| `LLM_GGUF`    | intent model path — smaller = faster on 6 GB | qwen-7B |

**6 GB GPU reality (the target hardware):** whisper-large-v3 (full) + Qwen-7B barely fit ONE
instance — no vLLM, no batching headroom, no 2nd copy. The GPU is **serial**: ~0.9 s
per warm wake-command → **~66/min**, and the queue makes bursts wait (not drop). Keep
`GPU_WORKERS=1`. The one throughput lever is a **smaller intent LLM** (Qwen 3B/1.5B q4 →
`LLM_GGUF=...` + `GPU_LAYERS=99` full offload, ~2× faster) — validate Persian intent
accuracy first. See DEPLOYMENT.md Phase 2. "200 *simultaneous* transcriptions instantly"
is not achievable on 6 GB; size expectations around throughput-per-minute + the queue.

## Calls — public reachability (#4)
- **Janus**: in `janus/janus.jcfg` set `nat_1_1_mapping = "<PUBLIC_IP>"` and
  `full_trickle = true` so remote callers receive a routable ICE candidate.
- **coturn**: fill `coturn.conf` (PUBLIC_IP + secret) and point the app/device ICE
  config at `turn:<PUBLIC_IP>:3478`. This is the relay fallback for strict NAT.
- Open firewall: TCP 8088/8188 (Janus), UDP 3478 + 49152-65535 (TURN/RTP).

## ⚠️ Before raising `API_WORKERS` above 1
Shared state already in Redis: the **wake-job queue**, **call state** (`janus:calls`),
and the **retention lock** (only one worker sweeps). **Still in-process:** `image_reqs`
(the camera-snapshot request map, ~7 call-sites with a device lookup). Migrate it to
Redis (device-indexed keys, like `janus:calls`) before running >1 API worker, or the
snapshot feature breaks across workers. Everything else (wake, pills, calls, voice,
heartbeat, fall) is multi-worker-safe today.

## Non-Docker alternative
Use the systemd units in `systemd/` (`noban-api.service`, `noban-worker.service`) +
`health_watchdog.{sh,service,timer}` for health-based auto-restart. Run `run_async.sh`
(API, `WAKE_NO_AUTO_WARMUP=1`) and `run_worker.sh` (worker) under them.
