# Files NOT in this repo — provide before running

These are git-ignored (too large, or secrets). Get them, then the server runs.

## 1. Models (~8 GB) — run the fetch script
```bash
./fetch_models.sh        # downloads into ./models/
```
Produces:
- `models/faster-whisper-large-v3/`  — STT (FULL large-v3; **do not** swap for turbo — it loses far-field accuracy)
- `models/qwen2.5-7b-instruct-q4_k_m.gguf`  — intent LLM

## 2. Firebase service-account key (FCM push notifications)
Provide your own — it is a private credential and must stay out of git (gitignored
**and** `.dockerignore`'d, so it is never baked into the Docker image):
- Get it: Firebase Console → Project settings → Service accounts → **Generate new private key**
- **Local / laptop:** place it in the Server root. The code default filename is
  `elderly-care-assistant-1bab66e453cb.json`; override with `FCM_KEY_FILE=/path/to/key.json`.
- **Docker (remote-docker):** name it `elderly-care-assistant.json` at the repo root —
  `docker-compose.yml` mounts it read-only into the `api` container and sets
  `FCM_KEY_FILE=/app/elderly-care-assistant.json` (in-container path). Override the
  host path with `FCM_KEY_FILE_HOST=/abs/path/key.json` in your `.env`.
- Without it the API runs, but FCM push notifications fail.

## 3. Secrets / config (set via env or the run scripts)
- `SMTP_SENDER_PASSWORD` — Gmail app password for validation emails.
- `DB_PASSWORD` / `DATABASE_URL` — Postgres credentials for your deployment.
- For remote/Docker calls: `PUBLIC_IP` + a TURN secret in `remote-docker/coturn.conf`, and
  `nat_1_1_mapping` in `remote-docker/janus/janus.jcfg`.

See `local-lan/LOCAL_LAN_SETUP.md` and `remote-docker/REMOTE_DEPLOYMENT.md`.
