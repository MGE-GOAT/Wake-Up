#!/bin/bash
# Launch the Phase 5.4 async FastAPI server (PostgreSQL/asyncpg).
# Mirrors the env the Flask server used (SMTP proxy + wake model paths).
# Single uvicorn process — async event loop + GPU-semaphore'd inference.
cd "$(dirname "$0")/.."

# Optional gitignored secrets file (SMTP_SENDER_PASSWORD, DB_PASSWORD, ...).
[ -f "$(dirname "$0")/secrets.env" ] && set -a && . "$(dirname "$0")/secrets.env" && set +a

CONDA_ROOT="${CONDA_ROOT:-/home/mahrad/miniconda3}"
CONDA_ENV="${CONDA_ENV:-app}"
source "$CONDA_ROOT/etc/profile.d/conda.sh" || { echo "ERROR: conda.sh not found under CONDA_ROOT=$CONDA_ROOT" >&2; exit 1; }
conda activate "$CONDA_ENV" || { echo "ERROR: 'conda activate $CONDA_ENV' failed (create/unpack the env first)" >&2; exit 1; }

# CUDA runtime libs (libcudart/libcublas/libcudnn) live in the `agent` env on
# this laptop — point the loader at them so llama-cpp + faster-whisper find CUDA.
CUDA_LIBS=$(ls -d /home/mahrad/miniconda3/envs/agent/lib/python3.11/site-packages/nvidia/*/lib 2>/dev/null | tr '\n' ':')

env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  LD_LIBRARY_PATH="${CUDA_LIBS}${LD_LIBRARY_PATH}" \
  WAKE_NO_AUTO_WARMUP=1 \
  SMTP_PROXY_HOST=127.0.0.1 SMTP_PROXY_PORT=10808 SMTP_PROXY_TYPE=socks5 \
  SMTP_SENDER_PASSWORD="${SMTP_SENDER_PASSWORD:-}" \
  DATABASE_URL="postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care" \
  WAKE_LLM_GGUF="${WAKE_LLM_GGUF:-/home/mahrad/storage/Data/Server/models/qwen2.5-7b-instruct-q4_k_m.gguf}" \
  WAKE_WHISPER_DIR="${WAKE_WHISPER_DIR:-/home/mahrad/storage/Data/Server/models/faster-whisper-large-v3}" \
  WAKE_LLM_GPU_LAYERS="${WAKE_LLM_GPU_LAYERS:-20}" WAKE_WHISPER_DEVICE=cuda WAKE_WHISPER_COMPUTE=int8_float16 \
  uvicorn app_async:app --host 0.0.0.0 --port "${PORT:-5000}" --workers "${WORKERS:-1}"
