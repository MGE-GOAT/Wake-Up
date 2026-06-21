#!/bin/bash
# Noban all-in-one demo launcher: ONE uvicorn process that also loads the models
# (no separate worker). Good for a single-box demo. Auto-detects VRAM → GPU layers.
# For the scalable split (model-free API + separate GPU worker) use
# local-lan/run_async.sh + local-lan/run_worker.sh instead.
#
# Python env: prefers the .venv created by ./setup_server.sh; falls back to a
# conda env (override with CONDA_ROOT / CONDA_ENV).
cd "$(dirname "$0")" || exit 1
ROOT="$(pwd)"

# Optional gitignored secrets (SMTP_SENDER_PASSWORD, DB_PASSWORD, SMTP_PROXY_*, ...).
[ -f secrets.env ] && set -a && . secrets.env && set +a

# --- pick the Python interpreter -------------------------------------------------
if [ -x "$ROOT/.venv/bin/python" ]; then
    PY="$ROOT/.venv/bin/python"
else
    CONDA_ROOT="${CONDA_ROOT:-$HOME/miniconda3}"
    CONDA_ENV="${CONDA_ENV:-app}"
    if [ -f "$CONDA_ROOT/etc/profile.d/conda.sh" ]; then
        # shellcheck disable=SC1091
        source "$CONDA_ROOT/etc/profile.d/conda.sh"
        conda activate "$CONDA_ENV" || { echo "ERROR: 'conda activate $CONDA_ENV' failed." >&2; exit 1; }
        PY="$(command -v python)"
    else
        echo "ERROR: no .venv (run ./setup_server.sh) and no conda at $CONDA_ROOT." >&2
        exit 1
    fi
fi

# --- CUDA runtime libs from pip 'nvidia-*' packages (venv or conda), best-effort -
CUDA_LIBS=""
for d in "$ROOT/.venv"/lib/python*/site-packages/nvidia/*/lib \
         "$HOME"/miniconda3/envs/*/lib/python*/site-packages/nvidia/*/lib; do
    [ -d "$d" ] && CUDA_LIBS="${CUDA_LIBS}${d}:"
done

VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1)
if   [ -z "$VRAM" ];        then GL=0;  WDEV=cpu;  echo "No GPU detected → CPU (wake commands will be slow)";
elif [ "$VRAM" -ge 7500 ];  then GL=32; WDEV=cuda;
elif [ "$VRAM" -ge 5500 ];  then GL=20; WDEV=cuda; echo "~6GB GPU → partial offload";
elif [ "$VRAM" -ge 3500 ];  then GL=12; WDEV=cuda; echo "~4GB GPU → heavy CPU offload (wake commands slower); calls/pills fine";
else GL=0; WDEV=cpu; fi
echo "VRAM=${VRAM:-?}MiB  →  WAKE_LLM_GPU_LAYERS=$GL  whisper=$WDEV"

# SMTP proxy is OPTIONAL — set SMTP_PROXY_HOST/PORT only on networks where direct
# SMTP is blocked (e.g. via secrets.env). Empty = send email directly.
env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  LD_LIBRARY_PATH="${CUDA_LIBS}${LD_LIBRARY_PATH:-}" \
  SMTP_SENDER_PASSWORD="${SMTP_SENDER_PASSWORD:-}" \
  SMTP_PROXY_HOST="${SMTP_PROXY_HOST:-}" SMTP_PROXY_PORT="${SMTP_PROXY_PORT:-}" SMTP_PROXY_TYPE="${SMTP_PROXY_TYPE:-socks5}" \
  DATABASE_URL="${DATABASE_URL:-postgresql://${DB_USER:-elderly}:${DB_PASSWORD:-elderly_local_pw}@127.0.0.1:5432/${DB_NAME:-elderly_care}}" \
  WAKE_LLM_GGUF="${WAKE_LLM_GGUF:-$ROOT/models/qwen2.5-7b-instruct-q4_k_m.gguf}" \
  WAKE_WHISPER_DIR="${WAKE_WHISPER_DIR:-$ROOT/models/faster-whisper-large-v3}" \
  WAKE_LLM_GPU_LAYERS=$GL WAKE_WHISPER_DEVICE=$WDEV WAKE_WHISPER_COMPUTE=int8_float16 \
  "$PY" -m uvicorn app_async:app --host 0.0.0.0 --port "${PORT:-5000}"
