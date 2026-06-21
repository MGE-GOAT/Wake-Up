#!/usr/bin/env bash
# =============================================================================
# Noban SERVER — one-shot provisioner for a BARE Debian/Ubuntu machine (LAN path).
#
# Brings a fresh box from nothing to a runnable Noban server: installs + starts
# the four system services (PostgreSQL, Redis, mosquitto, Janus), creates the DB
# + applies the schema, builds a Python venv with the API (and, if a GPU is
# present, the worker) deps, fetches the models, and scaffolds secrets.
#
# Idempotent: safe to re-run. Mirrors local-lan/LOCAL_LAN_SETUP.md.
#
# Usage:   ./setup_server.sh
# Override:  DB_PASSWORD=... SERVER_PORT=5000 ./setup_server.sh
#
# After it finishes you still: (1) put your secrets in secrets.env, (2) provide
# the Firebase key (see REQUIRED_FILES.md), (3) start the worker then the API.
# GPU/CUDA for the worker is a PREREQUISITE this script can't install reliably —
# it detects nvidia-smi and warns if absent.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"

DB_NAME="${DB_NAME:-elderly_care}"
DB_USER="${DB_USER:-elderly}"
DB_PASSWORD="${DB_PASSWORD:-elderly_local_pw}"   # override in prod; matches run_async.sh default
VENV_DIR="${VENV_DIR:-${HERE}/.venv}"

say(){ printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok(){  printf '    \033[0;32m✓ %s\033[0m\n' "$*"; }
warn(){ printf '    \033[1;33m! %s\033[0m\n' "$*"; }

SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

# -----------------------------------------------------------------------------
say "[1/7] System packages (services + build tools)"
$SUDO apt-get update -y
$SUDO apt-get install -y \
    postgresql postgresql-client \
    redis-server \
    mosquitto mosquitto-clients \
    ffmpeg curl git \
    python3 python3-venv python3-pip \
    || warn "some apt packages failed — re-check the names for your distro"
# Janus is packaged as 'janus' on Debian/Ubuntu; not on every release.
$SUDO apt-get install -y janus 2>/dev/null \
    || warn "'janus' apt package unavailable — install Janus manually (calls need it; the rest of the server runs without it)"
# coturn = STUN/TURN relay. The app's WebRTC ICE points at stun/turn :3478 on the
# server, so calls need it. On a flaky LAN, TURN-over-TCP also keeps call media
# alive when direct UDP gets dropped.
$SUDO apt-get install -y coturn 2>/dev/null \
    || warn "'coturn' apt package unavailable — install it manually (calls fall back to host candidates without it)"
ok "packages installed"

# -----------------------------------------------------------------------------
say "[2/7] Enable + start services"
for svc in postgresql redis-server mosquitto; do
    $SUDO systemctl enable --now "$svc" 2>/dev/null && ok "$svc up" \
        || warn "could not enable/start $svc"
done
$SUDO systemctl enable --now janus 2>/dev/null && ok "janus up" \
    || warn "janus service not started (manual install?) — calls won't work until it is"

# -----------------------------------------------------------------------------
say "[3/7] mosquitto listener (anonymous, LAN-only)"
# The API publishes device/app push over MQTT on 1883. LAN deployment = anonymous.
$SUDO tee /etc/mosquitto/conf.d/noban.conf >/dev/null <<'MQ'
listener 1883 0.0.0.0
allow_anonymous true
MQ
$SUDO systemctl restart mosquitto 2>/dev/null || true
ok "mosquitto on 1883 (anonymous)"

# -----------------------------------------------------------------------------
say "[4/7] Janus transports (HTTP 8088 + WS 8188)"
# The app talks Janus over WebSocket (8188); the device over HTTP (8088). Install
# the repo's jcfg if the system dir exists (LAN-safe; coturn/public-IP in the
# configs only matter for remote calls — see remote-docker/REMOTE_DEPLOYMENT.md).
if [ -d /etc/janus ] && [ -d remote-docker/janus ]; then
    for f in remote-docker/janus/*.jcfg; do
        $SUDO cp "$f" /etc/janus/ && ok "installed $(basename "$f")"
    done
    $SUDO systemctl restart janus 2>/dev/null || true
else
    warn "/etc/janus or remote-docker/janus missing — verify Janus has HTTP+WS transports enabled"
fi

# -----------------------------------------------------------------------------
say "[4b/7] coturn (STUN/TURN on :3478 — the app's WebRTC ICE points here)"
# Detect this host's LAN IP for the relay/external addresses.
LAN_IP="${TURN_IP:-$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[0-9.]+' | head -1)}"
TURN_USER="${TURN_USER:-noban}"; TURN_PASS="${TURN_PASS:-nobanpass}"
if command -v turnserver >/dev/null 2>&1 && [ -n "$LAN_IP" ]; then
    $SUDO tee /etc/turnserver.conf >/dev/null <<TURN
listening-port=3478
fingerprint
lt-cred-mech
user=${TURN_USER}:${TURN_PASS}
realm=noban.local
listening-ip=0.0.0.0
relay-ip=${LAN_IP}
external-ip=${LAN_IP}
min-port=49160
max-port=49200
no-tls
no-dtls
TURN
    # the coturn .deb sometimes fails to create its run-user; ensure it exists
    id turnserver >/dev/null 2>&1 || $SUDO useradd -r -s /usr/sbin/nologin turnserver 2>/dev/null || true
    $SUDO sed -i 's/^#*TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn 2>/dev/null \
        || echo "TURNSERVER_ENABLED=1" | $SUDO tee -a /etc/default/coturn >/dev/null
    $SUDO systemctl enable --now coturn 2>/dev/null && ok "coturn up on ${LAN_IP}:3478 (user ${TURN_USER})" \
        || warn "coturn failed to start — check 'journalctl -u coturn'"
else
    warn "coturn not installed or LAN IP undetected — calls use host candidates only (fine on a clean LAN)"
fi

# -----------------------------------------------------------------------------
say "[5/7] PostgreSQL: role + database + schema"
# Create role + db idempotently (run as the postgres superuser).
$SUDO -u postgres psql -tAc "SELECT 1 FROM pg_roles WHERE rolname='${DB_USER}'" | grep -q 1 \
    || $SUDO -u postgres psql -c "CREATE ROLE ${DB_USER} LOGIN PASSWORD '${DB_PASSWORD}';"
$SUDO -u postgres psql -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" | grep -q 1 \
    || $SUDO -u postgres createdb -O "${DB_USER}" "${DB_NAME}"
ok "role '${DB_USER}' + db '${DB_NAME}' ready"
# Schema is idempotent (every table is CREATE TABLE IF NOT EXISTS).
PGPASSWORD="${DB_PASSWORD}" psql -h 127.0.0.1 -U "${DB_USER}" -d "${DB_NAME}" -f schema_postgres.sql >/dev/null
ok "schema applied (schema_postgres.sql)"

# -----------------------------------------------------------------------------
say "[6/7] Python venv + dependencies"
[ -d "${VENV_DIR}" ] || python3 -m venv "${VENV_DIR}"
# shellcheck disable=SC1091
"${VENV_DIR}/bin/pip" install --upgrade pip >/dev/null
"${VENV_DIR}/bin/pip" install -r requirements-server.txt
ok "API deps installed (${VENV_DIR})"
if command -v nvidia-smi >/dev/null 2>&1; then
    ok "GPU detected — installing worker deps (faster-whisper + llama-cpp-python)"
    "${VENV_DIR}/bin/pip" install -r requirements-wake.txt \
        || warn "worker deps failed — llama-cpp-python may need CUDA toolkit/build flags; see requirements-wake.txt"
else
    warn "no nvidia-smi — the GPU worker (whisper+LLM) needs a CUDA GPU. API works without it,"
    warn "but /Wake_Command will hang with no worker. Install worker deps on a GPU box."
fi

# -----------------------------------------------------------------------------
say "[7/7] Models + secrets scaffold"
if [ -f models/faster-whisper-large-v3/model.bin ] && [ -f models/qwen2.5-7b-instruct-q4_k_m.gguf ]; then
    ok "models already present"
else
    warn "fetching models (~8 GB) via fetch_models.sh — Ctrl-C to skip + run later"
    bash fetch_models.sh || warn "model fetch incomplete — re-run ./fetch_models.sh (export HTTPS_PROXY on restricted nets)"
fi
if [ ! -f secrets.env ]; then
    cat > secrets.env <<'SE'
# Noban server secrets — gitignored. Sourced by run_async.sh / run_worker.sh.
SMTP_SENDER_EMAIL=
SMTP_SENDER_PASSWORD=
# DATABASE_URL is built from the defaults in run_async.sh; override here if needed.
SE
    ok "secrets.env scaffold written (fill it in)"
else
    ok "secrets.env already present"
fi

cat <<DONE

──────────────────────────────────────────────────────────────────────────────
 Noban server provisioned. Remaining manual steps:
   1. Fill in secrets.env (SMTP for verification emails).
   2. Provide the Firebase key (see REQUIRED_FILES.md) for push notifications.
   3. Start it (the run scripts auto-use the .venv created above — no extra setup).
      Two terminals, worker FIRST so the models warm up:
        bash local-lan/run_worker.sh   # wait for "models ready"  (needs a GPU)
        bash local-lan/run_async.sh    # wait for "asyncpg pool ready"
   4. Health check:  curl -s -o /dev/null -w '%{http_code}\\n' \\
        -X POST http://localhost:${SERVER_PORT:-5000}/Pill_Status_App \\
        -H 'Username: x' -H 'Password: x'   # → 403 = healthy
──────────────────────────────────────────────────────────────────────────────
DONE
