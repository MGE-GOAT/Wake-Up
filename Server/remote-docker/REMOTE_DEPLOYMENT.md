# Noban — Remote / Production Deployment (Dockerized)

This is the standalone production guide for running the Noban server on a paid cloud
instance or the team's own hardware. It covers a Dockerized single-box deployment and
the non-Docker (systemd) alternative.

Everything here is grounded in the actual files under `App/Server/remote-docker/`:
`docker-compose.yml`, `Dockerfile.api`, `Dockerfile.worker`, `coturn.conf`,
`systemd/*`, `health_watchdog.sh`, `wake_bench.py`, and the repo-root `.dockerignore`.

---

## 0. Architecture (what runs where)

```
 devices ─┐                          ┌─ Postgres   (durable state)
 app ─────┤  API (N uvicorn workers) ├─ Redis      (wake-job queue + shared call/lock state)
          │       │  enqueue          └─ mosquitto  (MQTT push to devices)
          │       ▼
          │  wake:jobs ──► GPU worker (whisper-large-v3 + intent LLM)  ◄── the ONE GPU
          │
 calls ───┴──► Janus SFU (+ coturn)  ── relays RTP, no transcoding
```

The decoupling is the whole point: **the API holds no models** — it sets
`WAKE_NO_AUTO_WARMUP=1` and only enqueues wake-commands to Redis. A separate
**GPU worker** drains `wake:jobs` and runs whisper + the intent LLM. That lets you:

- scale **API workers** for HTTP/CPU load, and
- size the **worker** for transcription load,

independently. Bursts **queue** instead of being dropped or serialized at the HTTP layer.

Services (from `remote-docker/docker-compose.yml`):

| Service | Image | Network | Notes |
|---|---|---|---|
| `redis` | `redis:7-alpine` | bridge | `--save ""` (no persistence), 512 MB LRU cap — it's a queue/cache |
| `postgres` | `postgres:16` | bridge | `restart: unless-stopped`, `pg_isready` healthcheck |
| `mosquitto` | `eclipse-mosquitto:2` | bridge | port 1883; mounts `./mosquitto.conf` |
| `janus` | `canyan/janus-gateway` | **host** | WebRTC needs a wide UDP RTP range; mounts `./janus` |
| `coturn` | `coturn/coturn` | **host** | TURN relay; mounts `./coturn.conf` |
| `api` | built `Dockerfile.api` | bridge | port 5000; `WORKERS=${API_WORKERS:-4}` |
| `worker` | built `Dockerfile.worker` | bridge | the only GPU consumer; `replicas: 1`, reserves all nvidia GPUs |

Named volumes: `pg-data`, `redis-data`, `uploads`. Models are **mounted**
(`${MODELS_DIR:-./models}:/models:ro`), never baked into the image.

> **Two files the compose references that you must supply before `up`:**
> `remote-docker/mosquitto.conf` and `remote-docker/janus/janus.jcfg` (a `./janus` config dir). They
> are not committed. Add a minimal mosquitto config and a Janus config with the public-IP
> settings from Phase 3 before bringing the stack up.

> **Firebase service-account key (FCM push):** the key is **gitignored AND
> `.dockerignore`'d** — it is never copied into the image (it would leak the private
> key). Instead it is **mounted read-only at runtime** into the `api` service:
> ```yaml
> volumes:
>   - ${FCM_KEY_FILE_HOST:-../elderly-care-assistant.json}:/app/elderly-care-assistant.json:ro
> ```
> The `api` service sets `FCM_KEY_FILE=/app/elderly-care-assistant.json` (the
> in-container path the app loads) so the app reads the key from the mount. Before
> `docker compose up`:
> 1. Place the key at the repo root as `elderly-care-assistant.json` (or point the
>    HOST path with `FCM_KEY_FILE_HOST=/abs/path/to/key.json` in your `.env`).
> 2. Get the key from Firebase Console → Project settings → Service accounts →
>    **Generate new private key**.
>
> Without it the API starts fine but FCM push notifications return an error.

---

## 1. Target hardware & prerequisites

**Reference target box:** 1 server, 1 GPU, ≥16 vCPU, ≥64 GB RAM, gigabit NIC, a public IP
for calls. GPU sizing is the crux — see the reality note below.

**Host software:**

- Docker Engine + the `docker compose` v2 plugin
- **nvidia-container-toolkit** (so the `worker` container can see the GPU) — verify with:
  ```bash
  docker run --rm --gpus all nvidia/cuda:12.4.1-base-ubuntu22.04 nvidia-smi
  ```
- A recent NVIDIA driver compatible with CUDA 12.4 (the worker base image is
  `nvidia/cuda:12.4.1-cudnn-devel-ubuntu22.04`).

### The AVX2 caveat (read this before you build the worker)

`Dockerfile.worker` **source-builds** `llama-cpp-python` instead of installing the
prebuilt CUDA wheel. Why: the upstream (abetlen) prebuilt CUDA wheels are compiled
**with AVX512**, but the deploy CPU is **AVX2-only** (verified: ~9534 AVX512 instructions
in the wheel → `SIGILL` / exit-132 the moment a model loads). The Dockerfile builds with:

```
-DGGML_CUDA=on -DGGML_CUDA_NO_VMM=ON -DGGML_NATIVE=OFF
-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_AVX512=OFF -DGGML_FMA=ON -DGGML_F16C=ON
-DCMAKE_SHARED_LINKER_FLAGS=-lcuda
```

- `GGML_AVX512=OFF` + `GGML_NATIVE=OFF` → the binary runs on AVX2 hardware and won't
  auto-enable AVX512 even if the *build* host happens to support it.
- `GGML_CUDA_NO_VMM=ON` → drops ggml's CUDA VMM memory-pool code, whose unresolved
  driver symbols (`cuMem*`, `cuDeviceGet`, `cuGetErrorString`) need the CUDA **driver**
  lib that's absent at build time. Plain `cudaMalloc` pooling is used instead (negligible
  perf cost for a 7B on 6 GB). `-lcuda` + the stub `LIBRARY_PATH` are belt-and-suspenders
  for any stragglers.

**If your target CPU has AVX512** you could skip the source build and use a prebuilt CUDA
wheel — but the default Dockerfile is safe on both, so just build it as-is unless build
time matters.

### Build-host network notes (restricted networks)

- The worker Dockerfile drops the NVIDIA CUDA apt repo (`developer.download.nvidia.com`
  may not resolve; the toolkit is already in the base image) and rewrites the Ubuntu apt
  sources to an `arvancloud.ir` mirror with a 6× retry loop for mirror-sync flakiness.
  **On a normal cloud host with clean internet, edit `Dockerfile.worker` to use the
  default `archive.ubuntu.com` mirror** — the arvancloud rewrite is for a region-blocked
  build environment, not generic.
- `PIP_PROXY` is a build-arg: pass `--build-arg PIP_PROXY=http://host:port` for an
  HTTPS-capable proxy on restricted networks; **omit it on a good-net host** (it defaults
  to empty → no proxy) and the loader clears `http_proxy`/`https_proxy` so the proxy never
  leaks into the runtime container.

### Models (mounted, not baked)

Per `.dockerignore`, `models/`, `remote-docker/models/`, `uploads/`, `*.wav`, `*.mp4` are all
excluded from the build context. Place the model files on the host and mount them:

```
models/
  faster-whisper-large-v3/          # whisper-large-v3 (FULL — fixed, see Phase 2)
  qwen2.5-7b-instruct-q4_k_m.gguf   # default intent LLM (swappable, see Phase 2)
```

Point the worker at them with `MODELS_DIR` (defaults to `./models`, mounted read-only at
`/models`).

---

## 2. Phase 1 — Bring up the stack in Docker

**Goal:** the whole stack running and supervised on the real server.

```bash
# on the server (Docker + compose + nvidia-container-toolkit installed)
git clone <repo> && cd .../App/Server/deploy

# 1) models on the host (mounted read-only, never baked)
cp -r /path/to/models ./models          # or: export MODELS_DIR=/abs/path/to/models

# 2) config the compose references but does NOT ship — create both:
#    - ./mosquitto.conf   (minimal broker config)
#    - ./janus/janus.jcfg (Janus config; fill public-IP bits in Phase 3)

# 3) secrets (do NOT hardcode in compose — see Phase 5)
export DB_PASSWORD='<strong-password>'

# 4) build (validates Dockerfile.api + the AVX2 source-build in Dockerfile.worker)
docker compose build
#    restricted net:  docker compose build --build-arg PIP_PROXY=http://host:port

# 5) up + supervise
docker compose up -d
docker compose ps                        # every service Up / healthy
```

**Health / "models ready" / 403 checks:**

```bash
# API is up and routing: a bad-cred probe returns 403 (auth reached, creds rejected)
curl -s -o /dev/null -w '%{http_code}' -X POST localhost:5000/Pill_Status_App \
     -H 'Username: x' -H 'Password: x'    # → 403 = healthy

# GPU worker finished loading whisper + LLM:
docker compose logs worker | grep "models ready"

# Postgres healthcheck (pg_isready) — compose gates `api` on it via depends_on
docker compose ps postgres               # → healthy
```

**Done when:** `docker compose ps` shows all services up/healthy, the API returns **403**
to the bad-cred probe, and the worker logged **"models ready"**.

> Build fallback: if the worker's CUDA `llama-cpp` source-build fails on your host, install
> a prebuilt CUDA wheel instead (`pip install llama-cpp-python --extra-index-url <cu124
> wheel index>`) — but only if the target CPU supports AVX512 (otherwise you hit the SIGILL
> described above).

---

## 3. Phase 2 — Throughput tuning on a constrained GPU

### The hard reality (state this to stakeholders)

The reference box has **one ~6 GB GPU**. On 6 GB, whisper-large-v3 (FULL) + Qwen-7B q4
barely fit **one** instance (with **partial** LLM offload). That means:

- **no vLLM, no batching headroom, no second model copy;**
- the GPU is a **serial** resource;
- **"200 simultaneous transcriptions instantly" is physically impossible on 6 GB** — no
  code change fixes that. Size around **throughput-per-minute + the queue**, not "instant
  for everyone at once."

What you *do* get:

- The **queue + 1 worker** (already built) → bursts **queue gracefully, nothing is dropped.**
- Measured on this hardware: **~0.9 s per warm wake-command → ~66 commands/minute.**
- For 200 users this is fine **on average** (commands are occasional). A **simultaneous
  burst** just queues: ~30 at once → ~27 s for the last; ~200 at once → ~3 min for the last.

### What is fixed vs. what you can tune

- **whisper-large-v3 (FULL, NOT turbo) is FIXED — non-negotiable.** Turbo loses far-field
  accuracy on the device's distance mic. Do **not** downsize whisper. (Worker runs it at
  `WAKE_WHISPER_COMPUTE=int8_float16` on `cuda`, ~1.5 GB.)
- **The only throughput lever on 6 GB = the intent LLM.** Intent is a 3-class job
  (PILL / CALL / MESSAGE); a 7B is overkill. Swap to **Qwen2.5-3B (or 1.5B) q4**:
  large-v3 int8 (~1.5 GB) + a 3B (~2 GB) fits 6 GB with **full GPU offload** for the LLM
  (no CPU penalty), trimming the LLM share of each command (~2× throughput).

```bash
# download a 3B/1.5B gguf into ./models, then point the worker at it with FULL offload:
#   WAKE_LLM_GGUF=/models/qwen2.5-3b-instruct-q4_k_m.gguf
#   WAKE_LLM_GPU_LAYERS=99        # full offload (vs 20 partial for the 7B)
# i.e. in the env passed to compose:
export LLM_GGUF=/models/qwen2.5-3b-instruct-q4_k_m.gguf
export GPU_LAYERS=99
docker compose up -d worker
```

**Validate Persian intent accuracy on real clips BEFORE keeping the smaller model**, then
benchmark the before/after:

```bash
python wake_bench.py 30 <clip.wav> http://localhost:5000 <device_id> <key_hash>  # 7B baseline
# swap to 3B, restart worker, repeat — expect ~2× throughput, same intents
```

**GPU_WORKERS stays 1.** Only one model copy fits in 6 GB; raising `worker.deploy.replicas`
will OOM. The path to true parallelism is a **bigger GPU** (≥24 GB) + a batched backend
(vLLM + batched whisper) — that's an image swap, not a knob.

**Done when:** the smaller LLM holds intent accuracy on your clips AND roughly doubles
commands/min. Size user expectations around **~60–120 cmds/min + the queue**.

---

## 4. Phase 3 — Public / remote calls

**Goal:** calls work for caregivers on **other networks** (cellular, another LAN), not just
the server's Wi-Fi. Calls are server-relayed through Janus (SFU); coturn is the TURN
fallback for strict/symmetric NAT.

Janus and coturn run with `network_mode: host` because WebRTC media uses a wide UDP RTP
range that can't be cleanly port-mapped through Docker's bridge.

```bash
# 1) Janus — in remote-docker/janus/janus.jcfg:
#      nat_1_1_mapping = "<SERVER_PUBLIC_IP>"   # remote callers get a routable candidate
#      full_trickle    = true                    # faster ICE, trickled candidates
#      (set ice_ignore_list as needed)

# 2) coturn — edit remote-docker/coturn.conf:
#      external-ip=<SERVER_PUBLIC_IP>            # currently the literal "PUBLIC_IP"
#      user=noban:<STRONG_SECRET>                # replace CHANGE_ME_STRONG_SECRET
#      (listening-port=3478, tls-listening-port=5349, min-port=49152, max-port=65535)

# 3) Point the app + device ICE config at TURN:
#      {"urls":"turn:<PUBLIC_IP>:3478","username":"noban","credential":"<STRONG_SECRET>"}
#    - app:    call_page.dart iceServers
#    - device: STUN_URLS / TURN_URL env in wake_call_helper

# 4) Firewall — open:
#      TCP 8088 (Janus HTTP), 8188 (Janus WS)
#      UDP 3478 (TURN/STUN), 49152-65535 (TURN relay + RTP)

docker compose restart janus coturn
```

> coturn note: `coturn.conf` uses simple long-term credentials (`user=noban:...`). For
> production, prefer `use-auth-secret` + the TURN REST API (time-limited credentials) so a
> leaked static secret can't be abused indefinitely.

**The real test (do not skip):** put the **phone on cellular** (not the server's Wi-Fi) and
call the device. Verify **two-way audio + video**.

**Done when:** a call connects and has media with the phone genuinely **off-network**.

---

## 5. Phase 4 — Load test on the real box

**Goal:** confirm capacity numbers on the actual hardware, not extrapolation.

```bash
# Transcription burst through the real queue → GPU worker path:
python wake_bench.py 200 <clip.wav> http://localhost:5000 <device_id> <key_hash>
#   reports: done/N, errors, wall time, throughput (clips/s), p50/p95/worst latency

# SFU concurrent calls (Janus CPU + bandwidth):
for N in 10 25 50; do python sfu_loadtest.py $N; done
```

**Watch while testing:**

| Subsystem | Command | Looking for |
|---|---|---|
| GPU | `nvidia-smi dmon` | util pinned (serial worker), no OOM |
| Janus | `ps -o %cpu,rss -C janus` | CPU/RSS scaling with call count |
| NIC | `ifstat` / `nload` | stay **< ~70%** of line rate |
| Postgres | `SELECT count(*) FROM pg_stat_activity;` | connection count not exhausted (→ PgBouncer, Phase 5) |

`wake_bench.py` fires N concurrent `/Wake_Command` requests with a real clip and reports
throughput + latency percentiles — run it against the current worker for a baseline, then
again after the LLM swap to measure the gain.

**Done when:** at target concurrency, p95 latency is acceptable, NIC < ~70% of line rate,
and there are no errors. Provision **~70–150 Mbps** for concurrent calls.

> 200 literal real users can't be simulated locally — these synthetic bursts + the
> per-unit numbers (~0.9 s/cmd, per-call bandwidth) are how you size the box.

---

## 6. Phase 5 — Production hygiene

**Goal:** safe to expose on the public internet.

### TLS via reverse proxy (Caddy = automatic Let's Encrypt)

Terminate HTTPS/WSS at a reverse proxy in front of the API and Janus, then point the
app/device at the `https://` / `wss://` URLs.

```caddy
app.example.com   { reverse_proxy localhost:5000 }
janus.example.com { reverse_proxy localhost:8188 }   # wss for Janus
```

### Secrets out of compose

Move `DB_PASSWORD`, `SMTP_SENDER_PASSWORD`, and the TURN secret out of compose literals /
`run_*.sh` into a `.env` file or **Docker secrets**, and rotate them.

- Compose already reads `${DB_PASSWORD:-...}` and `${LLM_GGUF}`/`${GPU_LAYERS}` from the
  environment — supply them via `.env` (auto-loaded by compose) rather than exporting
  inline.
- **Audit:** `run_async.sh` currently contains a plaintext `SMTP_SENDER_PASSWORD` and a
  hardcoded `DATABASE_URL` with the dev password — strip these before any production use.

### Postgres connection pooling (PgBouncer)

N API workers (×N containers later) can exhaust Postgres connections. Add **PgBouncer**
(transaction mode) in front of Postgres and point `DATABASE_URL` at it.

### Backups

Nightly `pg_dump` of `elderly_care` + a copy of the `uploads/` volume.

### Supervision

- **Docker:** every service uses `restart: unless-stopped` → crash auto-restarts,
  host reboot auto-starts. Postgres now auto-recovers (was `Restart=no`). Postgres has a
  `pg_isready` healthcheck and `api` waits on it via `depends_on: condition: service_healthy`.
- **Non-Docker alternative (see §7):** systemd units + a health watchdog timer.

**Done when:** all traffic is TLS, no plaintext secrets in the repo, PgBouncer is in the
DB path, and backups run nightly.

---

## 7. Non-Docker alternative (systemd)

For a bare-metal host without Docker, use the units in `remote-docker/systemd/`:

| Unit | Runs | Restart |
|---|---|---|
| `noban-api.service` | `run_async.sh` (API, `WAKE_NO_AUTO_WARMUP=1`, `WORKERS=4`) | `always`, 3 s |
| `noban-worker.service` | `run_worker.sh` (GPU worker, drains `wake:jobs`) | `always`, 3 s |
| `noban-health.service` + `.timer` | `health_watchdog.sh` every 60 s (OnBootSec=90) | oneshot |

`health_watchdog.sh` (health-based auto-restart, the systemd analog of Docker
healthchecks):

- API: bad-cred probe to `/Pill_Status_App` — restart `noban-api` unless it returns 403/200.
- Worker: if `redis-cli llen wake:jobs` > 50 (queue backing up) → restart `noban-worker`.
- `redis-cli ping` / `pg_isready` — restart Redis if down, log if Postgres not ready.

```bash
sudo cp remote-docker/systemd/*.service remote-docker/systemd/*.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now noban-api noban-worker noban-health.timer
```

> The shipped units use absolute dev paths (`/home/mahrad/storage/Data/Server`) and
> `run_*.sh` carries laptop-specific CUDA/conda/SMTP env. Edit `WorkingDirectory`, the
> CUDA `LD_LIBRARY_PATH` discovery, and the secrets for the production host before
> enabling.

---

## 8. Scaling knobs (reference)

Set via the environment that `docker compose` reads (or a `.env` file).

| Var | Meaning | Default | Scales? |
|---|---|---|---|
| `API_WORKERS` | uvicorn workers in the API container (HTTP/CPU). State shared via Redis. | `4` | ✅ yes — this is the HTTP lever |
| `GPU_WORKERS` | GPU worker copies (`worker.deploy.replicas`). | **`1`** | ❌ **fixed at 1 on 6 GB** — a 2nd copy OOMs |
| `GPU_LAYERS` | LLM layers offloaded to GPU. | `20` (partial, 7B) | `99` = full offload for a 3B/1.5B |
| `LLM_GGUF` | Intent LLM path. Smaller = faster on 6 GB. | qwen-7B q4 | swap → 3B/1.5B q4 (validate intent first) |
| `MODELS_DIR` | Host path mounted read-only at `/models`. | `./models` | — |
| `DB_PASSWORD` | Postgres password (move to `.env`/secrets). | `elderly_local_pw` | — |

### Before raising `API_WORKERS` above 1 — one caveat

Already shared in Redis (multi-worker-safe): the **wake-job queue**, **call state**
(`janus:calls`), and the **retention lock** (only one worker sweeps). **Still in-process:**
`image_reqs` (the camera-snapshot request map, ~7 call-sites with a device lookup). Migrate
it to Redis (device-indexed keys, like `janus:calls`) **before running >1 API worker**, or
the snapshot feature breaks across workers. Everything else (wake, pills, calls, voice,
heartbeat, fall) is multi-worker-safe today.

### True parallelism (beyond 6 GB)

The serial-GPU ceiling is hardware, not software. To exceed ~60–120 cmds/min you need a
**bigger GPU** (≥24 GB) and a **batched backend** (vLLM for the LLM + batched whisper) —
swap the `worker` image, keep the API + Redis queue unchanged. Whisper-large-v3 FULL stays
fixed regardless.

---

## 9. Gaps / things to supply before going live

1. **`remote-docker/mosquitto.conf`** and **`remote-docker/janus/janus.jcfg`** are referenced by compose
   but not committed — create both before `docker compose up`.
2. **`sfu_loadtest.py`** is referenced in Phase 4 — confirm it exists / supply it before
   the SFU load test.
3. **`coturn.conf`** still has literal `PUBLIC_IP` and `CHANGE_ME_STRONG_SECRET` — replace.
4. **`run_async.sh`** has a plaintext SMTP password and dev `DATABASE_URL` — strip for prod.
5. The **arvancloud apt mirror + PIP_PROXY** logic in `Dockerfile.worker` is for a
   region-blocked build env — revert to default mirrors / omit the proxy on a clean host.
