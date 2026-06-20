# Noban — Server

Backend for the **Noban** elderly-care system. It is a **model-free async API**
(`app_async.py`, FastAPI/uvicorn) that enqueues wake-commands to **Redis**, plus a
separate **GPU worker** (`wake_worker.py`) that runs the heavy models —
**faster-whisper-large-v3** (Persian STT) + **Qwen2.5-7B-Instruct** (intent LLM).
Calls are server-relayed through a **Janus** VideoRoom SFU (+ coturn). Persistent
state lives in **PostgreSQL**; push notifications go out over **mosquitto (MQTT)** and
**FCM**.

The decoupling point is Redis: the API holds **no models**, so it can run multiple
HTTP workers without OOMing the GPU, while exactly **one** GPU worker drains the
`wake:jobs` queue.

```
 devices ─┐                          ┌─ Postgres   (durable state)
 app ─────┤  API (N uvicorn workers) ├─ Redis      (wake-job queue + call/lock state)
          │       │  enqueue          └─ mosquitto  (MQTT push to devices/app)
          │       ▼
          │  wake:jobs ──► GPU worker (whisper-large-v3 + Qwen-7B)  ◄── the ONE GPU
          │
 calls ───┴──► Janus SFU (+ coturn)  ── relays RTP, no transcoding
```

**Data flow for a wake command:** device records the spoken command → `POST /Wake_Command`
(raw WAV, `Device-Id` + `Key-Hash` headers) → API writes the WAV to `uploads/` and pushes
`{id, wav_path}` to `wake:jobs`, then `BLPOP`s `wake:res:{id}` → worker pops the job, runs
whisper → Persian transcript → Qwen (grammar-constrained to one of 5 intents) → `RPUSH`es
the result → API routes by intent and notifies subscribers over MQTT/FCM.

---

## Repository layout

| Path | What it is |
|------|------------|
| `app_async.py` | **The API entrypoint** (`uvicorn app_async:app`). Async FastAPI port of the legacy `myserver.py` (Flask, kept for reference only). |
| `wake_command.py` | STT + intent classifier loaded by the worker (whisper-large-v3 → Qwen-7B, grammar-constrained). |
| `wake_worker.py` | The GPU worker — the **only** process that loads the models; drains the Redis `wake:jobs` queue. |
| `local-lan/` | **Non-Docker, single-machine LAN** deployment: `run_async.sh`, `run_worker.sh`, `secrets.env`, and `LOCAL_LAN_SETUP.md`. |
| `remote-docker/` | **Dockerized production** deployment: `docker-compose.yml`, `Dockerfile.api`, `Dockerfile.worker`, `janus/`, `mosquitto.conf`, `coturn.conf`, `systemd/`, and `REMOTE_DEPLOYMENT.md`. |
| `fetch_models.sh` | Downloads the ~8 GB of models into `models/` (git-ignored). |
| `schema_postgres.sql` | The PostgreSQL schema (re-runnable). |
| `requirements-server.txt` / `requirements-wake.txt` | API deps / GPU-worker deps. |
| `REQUIRED_FILES.md` | The files **not** in the repo (models, Firebase key, secrets) that you must supply. |
| `docs/SERVER_SIZING.md` | Hardware & internet **sizing/cost estimates** for ~200 users — server tiers, bandwidth, worst-case latency, and Iran hosting costs. |
| `models/` | Git-ignored, ~8 GB. Populated by `fetch_models.sh`. |

### The five wake intents

`wake_command.py` grammar-constrains the LLM to exactly one of:

| Intent | Meaning |
|--------|---------|
| `PILL` | The elder took / is talking about a pill or dose. |
| `CALL` | Call / contact a family member or caregiver. |
| `MESSAGE` | None of the others (greetings, small talk, garbled audio). |
| `SLEEP` | Put the device to sleep / deactivate monitoring (Persian: «غیرفعالش کن»). |
| `WAKE` | Wake the device / resume monitoring (Persian: «فعالش کن»). |

`SLEEP`/`WAKE` map to the device's sleep mode (see the Device repo).

---

## Quick start

```bash
git clone https://github.com/rayanobinorg/Server.git && cd Server
./fetch_models.sh        # downloads whisper + Qwen-7B into models/ (~8 GB)
# then provide the secrets/config in REQUIRED_FILES.md (Firebase key, SMTP, DB)
```

Then pick a deployment path:

- **(A) Local / LAN (non-Docker)** — single laptop/PC on a LAN router → §A below and
  [`local-lan/LOCAL_LAN_SETUP.md`](local-lan/LOCAL_LAN_SETUP.md).
- **(B) Remote / Docker (production)** — cloud box, public calls → §B below and
  [`remote-docker/REMOTE_DEPLOYMENT.md`](remote-docker/REMOTE_DEPLOYMENT.md).

> ⚠️ **Not in the repo (see [`REQUIRED_FILES.md`](REQUIRED_FILES.md)):** the ~8 GB of
> models (`fetch_models.sh`), the Firebase service-account key (FCM push), and the
> SMTP/DB secrets. Provide all three before running.

---

## (A) Local / LAN deployment (non-Docker)

Run the server on one machine plugged into a LAN router, with the device and the
caregiver app on the **same router**. Driven by `local-lan/run_worker.sh` +
`local-lan/run_async.sh`. Full detail in
[`local-lan/LOCAL_LAN_SETUP.md`](local-lan/LOCAL_LAN_SETUP.md).

### A.1 Prerequisites

- **Conda env `app`** with the API deps:
  ```bash
  conda activate app
  pip install -r requirements-server.txt        # API deps
  pip install -r requirements-wake.txt           # worker deps (faster-whisper, llama-cpp-python)
  ```
- **CUDA runtime** reachable by llama-cpp + faster-whisper. On the reference laptop the
  CUDA libs live in the `agent` conda env; both run scripts prepend them to
  `LD_LIBRARY_PATH` automatically. A 6 GB GPU fits exactly **one** worker
  (whisper int8_float16 ~1.5 GB + Qwen-7B q4 at partial offload, `WAKE_LLM_GPU_LAYERS=20`).
- **System services** running (the scripts do **not** start them):

  | Service | Endpoint | Used for |
  |---------|----------|----------|
  | Redis | `redis://127.0.0.1:6379` | wake queue, call state, retention lock |
  | PostgreSQL | `127.0.0.1:5432`, db `elderly_care`, user `elderly` | all persistent state |
  | mosquitto (MQTT) | `127.0.0.1:1883` | push to device/app |
  | Janus (SFU) | `:8088` (HTTP) / `:8188` (WS) | call media relay |

### A.2 Models

```bash
./fetch_models.sh
# → models/faster-whisper-large-v3/   (STT — FULL large-v3, do NOT swap for turbo)
# → models/qwen2.5-7b-instruct-q4_k_m.gguf   (intent LLM, ~4.7 GB)
```

### A.3 Provide secrets

```bash
# edit local-lan/secrets.env (gitignored; sourced by both run scripts) — at minimum:
#   SMTP_SENDER_PASSWORD=<gmail app password>   # for account-validation emails
```
`run_async.sh` carries the dev `DATABASE_URL` inline; override `DATABASE_URL` /
`DB_PASSWORD` via env or the secrets file for your DB. Place the Firebase service-account
key in the Server root (default filename `elderly-care-assistant-1bab66e453cb.json`, or
override with `FCM_KEY_FILE=/path/to/key.json`).

### A.4 Start order (services → worker → API)

```bash
# 1) system services (Postgres/Redis often do NOT auto-start after reboot)
sudo systemctl start postgresql@18-main    # named cluster on the reference box
sudo systemctl start redis-server mosquitto janus

# 2) GPU worker FIRST (so models warm while the API comes up). Run EXACTLY ONE.
cd /path/to/Server && ./local-lan/run_worker.sh
#    wait for: "models ready in N.Ns — waiting for jobs on wake:jobs"

# 3) API (second terminal)
cd /path/to/Server && ./local-lan/run_async.sh
#    wait for: asyncpg pool ready + uvicorn "Application startup complete"
```

### A.5 Health check (the 403 probe)

Hitting an authenticated endpoint with junk creds returns **403** when routing + DB + auth
are all alive:

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://localhost:5000/Pill_Status_App \
     -H 'Username: x' -H 'Password: x'        # → 403  = healthy
```

### A.6 Point clients at the server

The API listens on `0.0.0.0:5000` → reachable at `http://<LAN-IP>:5000` (`hostname -I`).
Device, app phone, and server must share the **same router** (no public IP/TLS/TURN in
this path). The app's `SERVER_URL` is **compile-time** (`--dart-define`) — rebuild the APK
to change it.

> **Pitfalls:** only one GPU worker on 6 GB; never run two API instances; keep
> `WORKERS=1` until `image_reqs` is migrated to Redis; if the worker is down,
> `/Wake_Command` hangs ~30 s then errors. See `local-lan/LOCAL_LAN_SETUP.md §6`.

---

## (B) Remote / Docker deployment (production)

A supervised single-box stack on a cloud instance or your own hardware, with public calls.
Full detail in [`remote-docker/REMOTE_DEPLOYMENT.md`](remote-docker/REMOTE_DEPLOYMENT.md).

### B.1 Prerequisites

- Docker Engine + the `docker compose` v2 plugin.
- **nvidia-container-toolkit** (so the `worker` container sees the GPU):
  ```bash
  docker run --rm --gpus all nvidia/cuda:12.4.1-base-ubuntu22.04 nvidia-smi
  ```
- A recent NVIDIA driver compatible with CUDA 12.4.
- **AVX2 caveat:** `Dockerfile.worker` source-builds `llama-cpp-python` with AVX512 OFF
  because the prebuilt CUDA wheels are AVX512-only (SIGILL on AVX2 CPUs). On a clean
  cloud host, revert the arvancloud apt mirror / `PIP_PROXY` build-args to defaults.

### B.2 Provide the not-in-repo files

```bash
cd remote-docker
cp -r /path/to/models ../models           # or: export MODELS_DIR=/abs/path/to/models  (mounted ro, never baked)
# Firebase key at the repo root (or set FCM_KEY_FILE_HOST=/abs/path/key.json in .env):
#   ../elderly-care-assistant.json   → mounted ro into the api container as /app/elderly-care-assistant.json
```
The compose file references `mosquitto.conf` and `janus/*.jcfg` (both present in
`remote-docker/`) and reads `DB_PASSWORD`, `LLM_GGUF`, `GPU_LAYERS`, `MODELS_DIR`,
`FCM_KEY_FILE_HOST` from the environment / a `.env` file.

### B.3 Build & up

```bash
cd remote-docker
export DB_PASSWORD='<strong-password>'
docker compose build          # restricted net: add --build-arg PIP_PROXY=http://host:port
docker compose up -d
docker compose ps             # every service Up / healthy
```

### B.4 Public / remote calls (Janus + coturn)

Janus and coturn run `network_mode: host` (WebRTC needs a wide UDP RTP range):

1. `janus/janus.jcfg` → `nat_1_1_mapping = "<SERVER_PUBLIC_IP>"`, `full_trickle = true`.
2. `coturn.conf` → replace `external-ip=PUBLIC_IP` and `CHANGE_ME_STRONG_SECRET`.
3. Point app + device ICE config at `turn:<PUBLIC_IP>:3478`.
4. Firewall: TCP 8088/8188 (Janus), UDP 3478 + 49152-65535 (TURN/RTP).
5. `docker compose restart janus coturn`.

**Test from cellular** (phone off the server's Wi-Fi) and verify two-way audio + video.

### B.5 Health check

```bash
curl -s -o /dev/null -w '%{http_code}' -X POST localhost:5000/Pill_Status_App \
     -H 'Username: x' -H 'Password: x'    # → 403
docker compose logs worker | grep "models ready"
```

### B.6 Scaling knobs (compose env / `.env`)

| Var | Meaning | Default | Notes |
|-----|---------|---------|-------|
| `API_WORKERS` | uvicorn workers (HTTP/CPU) | `4` | the HTTP lever; migrate `image_reqs` to Redis before raising in LAN mode |
| `GPU_WORKERS` | GPU worker replicas | **`1`** | **fixed at 1 on 6 GB** — a 2nd copy OOMs |
| `GPU_LAYERS` | LLM layers offloaded to GPU | `20` (7B partial) | `99` = full offload for a 3B/1.5B |
| `LLM_GGUF` | intent LLM path | qwen-7B q4 | swap to 3B/1.5B q4 for ~2× throughput (validate intents first) |
| `DB_PASSWORD` | Postgres password | `elderly_local_pw` | move to `.env`/secrets for prod |

> whisper-large-v3 (FULL) is **fixed** — turbo loses far-field accuracy. The 6 GB GPU is a
> serial resource (~0.9 s/warm command, ~66 cmds/min); true parallelism needs a ≥24 GB GPU
> + a batched backend (vLLM). See `REMOTE_DEPLOYMENT.md §2-3`.

---

## Database

Schema in `schema_postgres.sql` (re-runnable): `users`, `devices`, `active_devices`
(carries `guest`/`guest_requested` and `sleep`/`sleep_requested` flags), `messages`,
`unread_messages`, `subscriptions` (3-subscriber cap trigger), `subscription_history`,
`pill_schedules`. The asyncpg pool (min 2 / max 20) is created in the app's lifespan.

---

## Related repos

- **App** (caregiver phone app): https://github.com/rayanobinorg/App
- **Device** (Raspberry Pi): https://github.com/rayanobinorg/Device
