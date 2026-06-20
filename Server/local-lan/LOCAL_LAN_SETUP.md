# Noban — Local / LAN Setup (non-Docker, single machine)

This is the **non-Docker, single-machine** path: run the Noban server on one
laptop/PC plugged into a local router (e.g. a TP-Link LAN), with the device and
the caregiver app on the **same router**. For the containerized / public-internet
production path see `deploy/README.md` and `deploy/DEPLOYMENT.md` — this document
covers only the local LAN flow driven by `run_async.sh` + `run_worker.sh`.

Everything here is grounded in the actual code under
`/home/mahrad/storage/Data/Server/`.

---

## 1. Architecture

Two Python processes plus four system services. The decoupling point is **Redis**:
the API enqueues wake-commands, and a single GPU worker consumes them.

```
   ┌─────────────┐         ┌──────────────────────────────────┐
   │  Noban      │  HTTP   │  API process (run_async.sh)       │
   │  device     │────────►│  uvicorn app_async:app :5000      │
   │  (Pi)       │◄────────│  • holds NO models                │     ┌─────────────┐
   └─────────────┘         │  • asyncpg pool ──────────────────┼────►│ PostgreSQL  │
                           │  • redis.asyncio                   │     │ :5432       │
   ┌─────────────┐  HTTP   │  • mqtt_pub (paho) ────────────────┼──┐  │ elderly_care│
   │  Caregiver  │────────►│                                    │  │  └─────────────┘
   │  app        │◄────────│   enqueue ──► wake:jobs (Redis)    │  │
   │ (Flutter)   │         └────────────────────┬──────────────┘  │  ┌─────────────┐
   └─────────────┘                              │                 └─►│ mosquitto   │
         │                          BLPOP wake:jobs                  │ :1883 (MQTT)│
         │                                      ▼                    └─────────────┘
         │                         ┌──────────────────────────┐
         │ Janus signaling         │  GPU worker (run_worker)  │     ┌─────────────┐
         │ (SDP/ICE relayed        │  wake_worker.py           │────►│   Redis     │
         │  via API + Janus WS)    │  • faster-whisper-large-v3│     │ :6379       │
         ▼                         │  • qwen2.5-7b (gguf)      │◄────│ wake:jobs   │
   ┌─────────────┐                 │  • THE ONLY GPU process   │     │ wake:res:*  │
   │  Janus SFU  │  RTP relay      └──────────────────────────┘     │ janus:calls │
   │ :8088/:8188 │◄───── media (audio/video, no transcode) ─────┐   │ imgreq:*    │
   └─────────────┘                                              │   │ retention…  │
                                                                │   └─────────────┘
                                  device + app media ───────────┘
```

Key facts (verified in code):

- **The API holds no models.** `app_async.py` imports `wake_command` lazily and,
  in the LAN scripts, never warms it (`WAKE_NO_AUTO_WARMUP=1`). It only enqueues
  jobs to Redis (`_classify_via_queue`, app_async.py:121) and waits for a result.
- **The GPU worker is the only process on the GPU.** `wake_worker.py` calls
  `wake_command.warm_up()` to load both whisper + the LLM, then `BLPOP`s
  `wake:jobs`.
- **Whisper is `faster-whisper-large-v3` (FULL, not turbo)** — non-negotiable for
  far-field accuracy on the device's distance mic (see `wake_command.py` docstring
  and `deploy/DEPLOYMENT.md` Phase 2). Do not downsize whisper.
- **One 6 GB GPU → exactly one worker.** whisper-large-v3 (int8_float16, ~1.5 GB)
  + Qwen-7B q4 (partial offload, `WAKE_LLM_GPU_LAYERS=20`) barely fit one instance.

### Data flow for a wake command

1. Device records the spoken command and `POST`s the raw WAV bytes to
   `/Wake_Command` with `Device-Id` + `Key-Hash` headers (app_async.py:1493).
2. API authenticates the device, writes the WAV to `uploads/`, and pushes
   `{"id", "wav_path"}` to the Redis list `wake:jobs`, then `BLPOP`s
   `wake:res:{id}` (30 s timeout).
3. The GPU worker pops the job, runs `classify_wav()`:
   faster-whisper-large-v3 → Persian transcript → Qwen-2.5-7B (grammar-constrained
   to one of `PILL | CALL | MESSAGE`). It `RPUSH`es the JSON result to
   `wake:res:{id}` (TTL 60 s).
4. API receives the result, routes by intent (records a message / triggers a pill
   ack / call request), and notifies subscribers over MQTT (`mqtt_pub.py`).

---

## 2. Prerequisites

### Conda env `app`
Both launch scripts do `conda activate app`:
```bash
source /home/mahrad/miniconda3/etc/profile.d/conda.sh && conda activate app
```
Python deps are in `requirements-server.txt` (API: fastapi, uvicorn, asyncpg,
redis, paho-mqtt, pynacl, bcrypt, google-auth) and `requirements-wake.txt`
(worker: faster-whisper, llama-cpp-python, redis).

### CUDA libraries (borrowed from the `agent` env)
This box keeps the CUDA runtime libs (`libcudart` / `libcublas` / `libcudnn`) in
the **`agent`** conda env, not `app`. Both scripts prepend them to
`LD_LIBRARY_PATH` so llama-cpp + faster-whisper find CUDA:
```bash
CUDA_LIBS=$(ls -d /home/mahrad/miniconda3/envs/agent/lib/python3.11/site-packages/nvidia/*/lib | tr '\n' ':')
LD_LIBRARY_PATH="${CUDA_LIBS}${LD_LIBRARY_PATH}"
```

### System services (run as services, NOT started by the scripts)
| Service | Default endpoint | Used by | Code reference |
|---|---|---|---|
| **Redis** | `redis://127.0.0.1:6379` | wake queue, call state, image-req, retention lock | app_async.py:118, wake_worker.py:27 |
| **PostgreSQL** | `127.0.0.1:5432` db `elderly_care`, user `elderly` | all persistent state | app_async.py:64–67 |
| **mosquitto** (MQTT) | `127.0.0.1:1883` | push to device/app (`device/<id>/...`, `user/<u>/notify`) | mqtt_pub.py:43–45 |
| **Janus** (SFU) | `:8088` (HTTP) / `:8188` (WS) | call media relay (no transcode) | deploy/README.md |

### Models directory (`Server/models/`)
```
models/
├── faster-whisper-large-v3/                 # WAKE_WHISPER_DIR
└── qwen2.5-7b-instruct-q4_k_m.gguf          # WAKE_LLM_GGUF (~4.7 GB)
```
> Note: `wake_command.py`'s in-file default `LLM_GGUF` points at a `…14b…` name,
> but the launch scripts override it with `WAKE_LLM_GGUF=…qwen2.5-7b…`. The 7B is
> what actually runs on this box.

---

## 3. Start order

### Step 1 — Start the system services
Redis, mosquitto and Janus should already be enabled as services. PostgreSQL on
this box is the named cluster `postgresql@18-main`:
```bash
sudo systemctl start postgresql@18-main
sudo systemctl start redis-server      # or: redis
sudo systemctl start mosquitto
sudo systemctl start janus
```
> **Postgres (and sometimes Redis) do NOT auto-start after a reboot on this box.**
> Always run the `postgresql@18-main` start before launching the server, or the
> asyncpg pool fails at startup.

### Step 2 — Start the GPU worker (loads the models)
Start the worker **first** so the models are warming while the API comes up:
```bash
cd /home/mahrad/storage/Data/Server
./run_worker.sh
```
Wait for the log line `models ready in N.Ns — waiting for jobs on wake:jobs`
(wake_worker.py:37). **Run exactly one copy.** (`run_worker.sh` is non-executable
in the repo; invoke with `bash run_worker.sh` if needed.)

### Step 3 — Start the API
In a second terminal:
```bash
cd /home/mahrad/storage/Data/Server
./run_async.sh
```
Wait for `[async-server] asyncpg pool ready` and uvicorn's
`Application startup complete`.

### Environment variables (what each does)

`run_worker.sh`:
| Var | Value (script) | Purpose |
|---|---|---|
| `LD_LIBRARY_PATH` | `agent` env nvidia libs | CUDA for llama-cpp + faster-whisper |
| `REDIS_URL` | `redis://127.0.0.1:6379` | wake-job queue source |
| `WAKE_LLM_GGUF` | `models/qwen2.5-7b-instruct-q4_k_m.gguf` | intent LLM |
| `WAKE_WHISPER_DIR` | `models/faster-whisper-large-v3` | STT model dir |
| `WAKE_LLM_GPU_LAYERS` | `20` (override via env) | LLM layers offloaded to GPU |
| `WAKE_WHISPER_DEVICE` | `cuda` | whisper on GPU |
| `WAKE_WHISPER_COMPUTE` | `int8_float16` | whisper quantization (~1.5 GB) |

`run_async.sh` (same model env, plus):
| Var | Value (script) | Purpose |
|---|---|---|
| `WAKE_NO_AUTO_WARMUP` | `1` | **API must NOT load models** (worker owns the GPU) |
| `DATABASE_URL` | `postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care` | asyncpg pool |
| `PORT` | `5000` (default) | uvicorn listen port |
| `WORKERS` | `1` (default) | uvicorn API workers — see pitfalls before raising |
| `SMTP_PROXY_*`, `SMTP_SENDER_PASSWORD` | — | email path (verification codes) |
| `LD_LIBRARY_PATH` | `agent` env nvidia libs | (loaded even though API skips models) |

> `REDIS_URL` is also read by the API (app_async.py:118) defaulting to
> `redis://127.0.0.1:6379`; `run_async.sh` does not set it, so the default applies.
> `run_worker.sh` does not set `DATABASE_URL` (the worker never touches Postgres).
> `API_WORKERS`/`GPU_WORKERS` named in `deploy/README.md` are Docker-compose env
> names; the local scripts use `WORKERS` (uvicorn) and a single `run_worker.sh`.

---

## 4. Point the device and app at the server

1. Find the server's LAN IP (e.g. `192.168.1.50`):
   ```bash
   hostname -I
   ```
2. The server listens on `0.0.0.0:5000` (`--host 0.0.0.0 --port ${PORT:-5000}`),
   so it is reachable at **`http://<LAN-IP>:5000`**.
3. **Device** (Pi): point its server URL at `http://<LAN-IP>:5000`.
4. **App** (Flutter): `SERVER_URL` is **compile-time** — it is baked in via
   `--dart-define=SERVER_URL=http://<LAN-IP>:5000` when building the APK. You must
   rebuild the APK to change it; it cannot be changed at runtime.
5. **Same router requirement:** device, app phone, and server must all be on the
   **same LAN/router**. There is no public IP, TLS, or TURN relay in this local
   path — calls rely on Janus reachable on the LAN. (Remote/off-network calls
   require the public-IP + coturn setup in `deploy/DEPLOYMENT.md` Phase 3.)

---

## 5. Health checks & ports

### API healthy (the 403 check)
Hit an authenticated endpoint with junk creds — a **403** proves routing + DB +
auth are alive (it reached the DB, found no matching user/device, and rejected):
```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://localhost:5000/Pill_Status_App \
     -H 'Username: x' -H 'Password: x'
# → 403  (API + Postgres healthy)
```
(Same check used in `deploy/DEPLOYMENT.md` Phase 1.)

### Worker healthy
Worker stdout shows:
```
[wake-worker] HH:MM:SS models ready in N.Ns — waiting for jobs on wake:jobs
```
After a real command it logs `job <id> → PILL|CALL|MESSAGE (N.Ns)`.

### Quick liveness probes
```bash
redis-cli -u redis://127.0.0.1:6379 ping                 # → PONG
redis-cli llen wake:jobs                                  # queue depth (0 = idle)
PGPASSWORD=elderly_local_pw psql -h 127.0.0.1 -U elderly -d elderly_care -c '\dt'
mosquitto_sub -h 127.0.0.1 -t 'device/#' -v              # watch MQTT pushes
```

### Ports in use
| Port | Service |
|---|---|
| 5000 | Noban API (uvicorn) |
| 6379 | Redis |
| 5432 | PostgreSQL |
| 1883 | mosquitto (MQTT) |
| 8088 / 8188 | Janus HTTP / WebSocket |

---

## 6. Common pitfalls

- **Only ONE GPU worker on a 6 GB card.** Two `run_worker.sh` instances load two
  model copies → VRAM OOM/thrash. Keep it at one (`deploy/README.md`: "Keep
  `GPU_WORKERS=1`").
- **Never run two API server instances.** A second `run_async.sh` competes for the
  same port and shared Redis/DB state. (And on this box, a second model-loading
  process would OOM the 6 GB GPU — see MEMORY: "never 2 instances".)
- **Postgres / Redis do not auto-start after reboot.** Start
  `postgresql@18-main` (and confirm Redis) before launching the API, or the
  asyncpg pool init fails. The local scripts do **not** start any system service.
- **API workers > 1 is not safe yet.** `WORKERS`/`API_WORKERS` above 1 breaks the
  in-process camera-snapshot map (`image_reqs`) across workers — most state is in
  Redis but the snapshot path is not fully migrated (`deploy/README.md` warning).
  Wake, pills, calls, voice, heartbeat, fall are multi-worker-safe; keep
  `WORKERS=1` locally unless you've migrated `image_reqs`.
- **Worker not running ⇒ `/Wake_Command` hangs then errors.** The API `BLPOP`s
  `wake:res:{id}` for 30 s; with no worker draining `wake:jobs`, the request times
  out (the device sees its HTTP timeout). Always start the worker.
- **Stale binary / wrong model.** `wake_command.py`'s default GGUF name is the 14B;
  rely on the script's `WAKE_LLM_GGUF` (7B) override — don't run the worker without
  the env from `run_worker.sh`.

---

## Appendix — HTTP endpoints (app_async.py)

Auth column: **Device** = `Device-Id` + `Key-Hash` (`verify_device_auth`,
app_async.py:364); **User** = `Username` + `Password` (`check_user_creds`,
app_async.py:421); **Call** = either device or user (`_call_auth`, app_async.py:1343);
**Open** = no auth helper (registration/login).

| Method | Path | Auth | Notes |
|---|---|---|---|
| GET  | `/Live_Device` | Device | heartbeat; reports `image_req`, call_pending (from `janus:calls`), pills |
| POST | `/Config_Device` | Device | device config |
| POST | `/Message_Device` | Device | device → server event; image-captured uses `imgreq:*` |
| POST | `/Voice_Message_Device` | Device | voice memo upload |
| POST | `/Registeration_App` | Open | caregiver sign-up |
| POST | `/Login_App` | Open | caregiver login |
| POST | `/Config_App` | User | app config |
| POST | `/Message_Check_App` | User | fetch messages |
| POST | `/Subscribe_App` | User | subscribe app→device |
| POST | `/Image_Request_App` | User | sets `imgreq:{id}` + `imgreq:dev:{device}` (EX 600) |
| POST | `/Voice_Messages_App` | User | list voice messages |
| POST | `/Fall_Video_App` | User | lazy fall-clip fetch |
| POST | `/Pill_Sync_App` | User | create/update pill schedule |
| POST | `/Pill_Ack_Device` | Device | device acknowledges pill |
| POST | `/Pill_Delete_App` | User | delete pill |
| POST | `/Pill_Status_App` | User | pill status (used in the 403 health check) |
| POST | `/Set_Guest_App` | User | guest mode |
| GET  | `/Media/{filename}` | (Bearer/path) | media fetch |
| GET  | `/Pill_Audio/{pill_db_id}` | — | pill reminder audio |
| POST | `/Voice_To_Device` | — | caregiver→device transient voice note (not persisted) |
| GET  | `/Voice_To_Device_Audio/{device_id}` | — | device fetches pending voice note |
| POST | `/Call_Offer` | Call | store SDP offer; sets call_pending (MQTT) |
| POST | `/Call_Answer` | Call | store/fetch SDP answer |
| POST | `/Call_Ice` | Call | exchange ICE candidates (side=app\|device) |
| POST | `/Call_End` | Call | clear call mailbox |
| POST | `/Call_Start` | Call | mark call active in `janus:calls`; returns deterministic Janus `room` |
| POST | `/Call_Stop` | Call | `HDEL janus:calls` |
| POST | `/Wake_Command` | Device | raw WAV body → `wake:jobs` queue → worker |
| POST | `/Register_Device` | — | device provisioning |
| POST | `/Subscribe_Device` | — | device subscription |
| POST | `/Replace_Subscriber` | — | swap a subscriber |
| POST | `/Unsubscribe_Device` | — | remove subscriber |
| POST | `/Factory_Reset_Device` | — | reset device record |
| POST | `/Fall_Clip_Replace_Device` | — | replace stored fall clip |
| GET  | `/Device_Subscribers` | — | list a device's subscribers |

### Redis keys used by the API
- `wake:jobs` (list) — wake-command queue → worker; `wake:res:{id}` (list, TTL 60s) — result.
- `janus:calls` (hash) `device_id → caregiver` — active call state (call_pending source).
- `imgreq:{req_id}` (str, EX 600) + `imgreq:dev:{device_id}` (str, EX 600) — camera snapshot requests.
- `retention:lock` (str, NX EX 3300) — single-sweeper lock for `_retention_loop`.

### Postgres (asyncpg)
Pool created in `lifespan` (min 2 / max 20). Schema in `schema_postgres.sql`
(`users`, `devices`, `active_devices`, `messages`, `unread_messages`,
`subscriptions` [3-subscriber cap trigger], `subscription_history`,
`pill_schedules`). `database_management.py` is the **legacy SQLite** initializer
(`server_database.db`) — superseded by Postgres; not used by `app_async.py`.

### Media encryption (`media_crypto.py`)
The server is a **relay, not a content store** — it shuttles ciphertext and never
holds the media key. End-to-end scheme: per-device HKDF-SHA256 root key →
per-blob key → XChaCha20-Poly1305 (libsodium/pynacl). The module exists mainly to
prove cross-implementation (Flutter / C++) parity via shared test vectors; the
relay path does not encrypt/decrypt user media.
