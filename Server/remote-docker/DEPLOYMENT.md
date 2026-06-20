# Noban — production deployment checklist (200 users)

The 5 gaps between "works on the laptop" and "serves 200 remote users," in order.
Each phase has commands + a **done-when** check. Artifacts referenced live in this
`remote-docker/` folder. Target box: 1 server, 1 GPU (≥24 GB), ≥16 vCPU, ≥64 GB RAM, gigabit.

---
## Phase 1 — Stand up the stack in Docker  (gap #3)
**Goal:** the whole stack running + supervised on the real server.
```bash
# on the server (Docker + docker compose + nvidia-container-toolkit installed)
git clone <repo> && cd .../Server/deploy
cp -r /path/to/models ./models            # qwen gguf + faster-whisper-large-v3
export DB_PASSWORD='<strong>'
docker compose build                       # ← validates Dockerfile.api + Dockerfile.worker
docker compose up -d
docker compose ps                          # all services Up
curl -s -o /dev/null -w '%{http_code}' -X POST localhost:5000/Pill_Status_App \
     -H 'Username: x' -H 'Password: x'      # → 403 = API healthy
docker compose logs worker | grep "models ready"
```
**Done when:** `docker compose ps` all healthy, API returns 403, worker logged "models ready".
*If `Dockerfile.worker` fails to build llama-cpp w/ CUDA → use a prebuilt CUDA wheel
(`pip install llama-cpp-python --extra-index-url <cu124 wheel index>`) instead of compiling.*

---
## Phase 2 — Maximize transcription throughput on the 6 GB GPU  (gap #1)
**Hard reality:** the server has **one 6 GB GPU** (≈ this dev laptop). whisper-large-v3 (FULL)
+ Qwen-7B barely fit *one* instance (partial offload). So: **no vLLM, no batching headroom,
no second model copy.** The GPU is a **serial** resource. **True "200 parallel" is physically
impossible on 6 GB** — and no code change changes that. What we DO have:
- The **queue + 1 worker** (already built) = bursts **queue gracefully, nothing is dropped**.
- Measured on this hardware: **~0.9 s per warm wake-command → ~66 commands/minute**.
- For 200 users this is fine on **average** (commands are occasional). A **simultaneous
  burst** just queues: 30 at once → ~27 s for the last; 200 at once → ~3 min for the last.

**whisper-large-v3 (FULL, NOT turbo) is FIXED — non-negotiable** (turbo loses far-field accuracy on
the device's distance mic). Do NOT downsize whisper.
**The only tunable on 6 GB = the intent LLM.** Intent is a 3-class job (PILL/CALL/MESSAGE) — a 7B
is overkill. Swap to **Qwen2.5-3B (or 1.5B) q4**: large-v3 int8 (~1.5 GB) + a 3B (~2 GB) fits 6 GB
with **full GPU offload** for the LLM (no CPU penalty) and trims the LLM share of each command:
```bash
# download a 3B/1.5B gguf into ./models, then point the worker at it + full offload:
#   WAKE_LLM_GGUF=/models/qwen2.5-3b-instruct-q4_k_m.gguf  WAKE_LLM_GPU_LAYERS=99
```
**Validate accuracy before keeping it** (Persian intent on real clips), then benchmark:
```bash
python wake_bench.py 30 <clip.wav> http://localhost:5000 <dev> <kh>   # baseline (7B)
# swap to 3B, restart worker, repeat — expect ~2× throughput, same intents
```
**Done when:** the smaller LLM holds intent accuracy on your clips AND ~doubles
commands/min. Keep `GPU_WORKERS=1` (only one model fits). Accept that big simultaneous
bursts queue — size user expectations around ~60–120 cmds/min, not "instant for 200 at once."

---
## Phase 3 — Public / remote calls  (gap #2)
**Goal:** calls work for caregivers on other networks (not just LAN).
```bash
# 1) Janus: edit remote-docker/janus/janus.jcfg
#      nat_1_1_mapping = "<SERVER_PUBLIC_IP>"
#      full_trickle = true
# 2) coturn: edit remote-docker/coturn.conf  → external-ip=<PUBLIC_IP>, set a strong secret
# 3) Point app + device ICE at TURN:
#      {"urls":"turn:<PUBLIC_IP>:3478","username":"noban","credential":"<secret>"}
#    (app: call_page.dart iceServers; device: STUN_URLS/TURN_URL env in wake_call_helper)
# 4) Firewall: open tcp 8088,8188 + udp 3478 + udp 49152-65535
docker compose restart janus coturn
```
**Test (the real one):** put the **phone on cellular** (not the server's Wi-Fi),
call the device. Verify two-way audio + video.
**Done when:** a call connects + has media with the phone off-network.

---
## Phase 4 — Load test on the real hardware  (gap #4)
**Goal:** confirm capacity numbers on the actual box, not extrapolation.
```bash
# Transcription burst (queue + GPU worker):
python wake_bench.py 200 <clip.wav> http://localhost:5000 <dev> <kh>
# SFU concurrent calls (Janus CPU + bandwidth):
for N in 10 25 50; do python sfu_loadtest.py $N; done   # watch: ps -o %cpu,rss -C janus ; ifstat
```
**Watch:** GPU util (`nvidia-smi dmon`), Janus CPU/RSS, NIC throughput (`ifstat`/`nload`),
Postgres connections (`SELECT count(*) FROM pg_stat_activity`).
**Done when:** at target concurrency, p95 latency OK, NIC < ~70% of line rate, no errors.
*Note: 200 literal real users can't be simulated locally — these synthetic tests +
per-unit numbers are how you size it; provision bandwidth for ~70–150 Mbps of calls.*

---
## Phase 5 — Production hygiene  (gap #5)
**Goal:** safe to expose on the internet.
- **TLS:** terminate HTTPS/WSS at a reverse proxy (Caddy = auto-LetsEncrypt):
  ```
  app.example.com { reverse_proxy localhost:5000 }
  janus.example.com { reverse_proxy localhost:8188 }   # wss for Janus
  ```
  Then point the app/device at `https://`/`wss://` URLs.
- **Secrets:** move DB_PASSWORD, SMTP_SENDER_PASSWORD, TURN secret to a `.env` /
  Docker secrets — out of `run_*.sh` and compose literals. Rotate them.
- **Postgres pooling:** add **PgBouncer** (transaction mode) in front of Postgres;
  point `DATABASE_URL` at it so N API workers don't exhaust connections.
- **Backups:** nightly `pg_dump` + the `uploads/` volume.
**Done when:** all traffic is TLS, no plaintext secrets in the repo, PgBouncer in path.

---
### Quick status map
| # | Gap | Phase | Needs |
|---|-----|-------|-------|
| 3 | Containers untested | 1 | the server |
| 1 | 200-parallel transcription | 2 | the GPU + vLLM swap |
| 2 | Remote calls | 3 | public IP + firewall |
| 4 | Real load test | 4 | the server |
| 5 | TLS / secrets / pooling | 5 | domain + reverse proxy |

Everything before this (functionality, calls on LAN, self-healing, the queue/worker
architecture, multi-worker, Janus scaling) is **done + tested**.
