#!/usr/bin/env bash
# Launcher for the C++ pipeline binary. Two modes, auto-detected:
#
#   • PC / dev mode — if $SERVER_DIR (default: /home/mahrad/storage/Data/App/Server)
#     contains myserver.py, this script starts the server alongside the
#     pipeline. Useful when the same machine is hosting both halves.
#
#   • Laptop-as-device mode — if myserver.py is NOT found locally, the script
#     skips the server entirely. The pipeline talks to whatever SERVER_BASE_URL
#     is baked into main.cpp (or runtime override via env). This is the
#     correct mode for testing the device half from a laptop while the
#     server runs on a different PC on the LAN.
#
# Assumptions in either mode:
#   • Pipeline binary at ./build/pipeline (or PIPELINE_BIN env override)
#   • The 3 fall models (4.tflite, SVM_rbf.onnx, Last_stage_Model.onnx) live
#     in the CWD — the binary loads them relative to where it's invoked
#   • models/ dir alongside CWD with dscnn_int8.tflite, silero_vad.onnx,
#     yamnet.tflite, pain_logmel5.tflite
#   • Working webcam (cv::VideoCapture(0)) and microphone

set -u

PIPELINE_BIN="${PIPELINE_BIN:-./build/pipeline}"
SERVER_DIR="${SERVER_DIR:-/home/mahrad/storage/Data/App/Server}"
SERVER_PORT="${SERVER_PORT:-5000}"

# Skip the heavy whisper + Qwen model loads at server start when this script
# spawns the server itself — fine for any test that isn't actually exercising
# /Wake_Command end-to-end. (Has no effect in laptop-as-device mode.)
export WAKE_NO_AUTO_WARMUP=1

log() { echo "[dev_run] $*" >&2; }

SERVER_PID=
PIPELINE_PID=

cleanup() {
    log "Stopping background processes..."
    [ -n "${SERVER_PID}" ] && kill "${SERVER_PID}" 2>/dev/null
    [ -n "${PIPELINE_PID}" ] && kill "${PIPELINE_PID}" 2>/dev/null
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── Server (PC / dev mode only) ───────────────────────────────────────────
if [ -f "${SERVER_DIR}/myserver.py" ]; then
    if ss -ltn 2>/dev/null | grep -q ":${SERVER_PORT}\b"; then
        log "Server already on :${SERVER_PORT} — reusing"
    else
        log "Starting Python server on :${SERVER_PORT} (WAKE_NO_AUTO_WARMUP=1)..."
        (cd "${SERVER_DIR}" && python3 myserver.py) >/tmp/dev_server.log 2>&1 &
        SERVER_PID=$!
        for _ in $(seq 1 30); do
            if ss -ltn 2>/dev/null | grep -q ":${SERVER_PORT}\b"; then
                log "Server listening on :${SERVER_PORT} (logs: /tmp/dev_server.log)"
                break
            fi
            sleep 1
        done
        # Give up cleanly if the server didn't bind — likely a missing dep or
        # already-bound socket on another interface.
        if ! ss -ltn 2>/dev/null | grep -q ":${SERVER_PORT}\b"; then
            log "Server failed to bind. Check /tmp/dev_server.log."
            log "Aborting — the pipeline alone wouldn't be useful from here."
            exit 1
        fi
    fi
else
    log "No myserver.py at ${SERVER_DIR} — running in laptop-as-device mode."
    log "Pipeline will talk to the SERVER_BASE_URL baked into the binary."
    log "(Set SERVER_DIR=/path/to/server if you do have a local server.)"
fi

# ── Pipeline binary ────────────────────────────────────────────────────────
if [ ! -x "${PIPELINE_BIN}" ]; then
    log "Pipeline binary not found at ${PIPELINE_BIN}"
    log "Build it first:  mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    log "Then re-run this script (or set PIPELINE_BIN=...) "
    if [ -n "${SERVER_PID}" ]; then
        log "Server is left running on :${SERVER_PORT} — Ctrl-C to stop."
        wait "${SERVER_PID}" 2>/dev/null
    fi
    exit 1
fi

LAPTOP_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
log "Starting pipeline (${PIPELINE_BIN})..."
log "To simulate button press:"
log "    kill -USR1 \$(pgrep -f pipeline)"
log ""
if [ -n "${SERVER_PID}" ]; then
    log "Flutter app should point at:"
    log "    Android emulator host: http://10.0.2.2:${SERVER_PORT}"
    log "    LAN device:            http://${LAPTOP_IP}:${SERVER_PORT}"
    log ""
fi
log "Watch the pipeline output in this terminal; Ctrl-C to stop."
echo "──────────────────────────────────────────────────────────────────"

"${PIPELINE_BIN}" &
PIPELINE_PID=$!
log "Pipeline PID = ${PIPELINE_PID}"

# Block until the relevant process exits.
#   • Both mode: wait on both, exit when either dies.
#   • Laptop-as-device mode: only the pipeline exists, just wait on it.
if [ -n "${SERVER_PID}" ]; then
    wait -n "${SERVER_PID}" "${PIPELINE_PID}" 2>/dev/null || true
else
    wait "${PIPELINE_PID}" 2>/dev/null || true
fi
