#!/bin/bash
# Launch the GPU wake-command worker (the ONLY process that loads the models).
# Mirrors run_async.sh's CUDA + model env. Run alongside run_async.sh.
# Scale on the real box: run N copies (or swap wake_worker.py for a batched/vLLM
# backend) — the API + Redis queue are unchanged.
cd "$(dirname "$0")/.."

# Optional gitignored secrets file (DB_PASSWORD, etc.).
[ -f "$(dirname "$0")/secrets.env" ] && set -a && . "$(dirname "$0")/secrets.env" && set +a

CONDA_ROOT="${CONDA_ROOT:-/home/mahrad/miniconda3}"
CONDA_ENV="${CONDA_ENV:-app}"
source "$CONDA_ROOT/etc/profile.d/conda.sh" || { echo "ERROR: conda.sh not found under CONDA_ROOT=$CONDA_ROOT" >&2; exit 1; }
conda activate "$CONDA_ENV" || { echo "ERROR: 'conda activate $CONDA_ENV' failed (create/unpack the env first)" >&2; exit 1; }

CUDA_LIBS=$(ls -d /home/mahrad/miniconda3/envs/agent/lib/python3.11/site-packages/nvidia/*/lib 2>/dev/null | tr '\n' ':')

env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  LD_LIBRARY_PATH="${CUDA_LIBS}${LD_LIBRARY_PATH}" \
  REDIS_URL="${REDIS_URL:-redis://127.0.0.1:6379}" \
  WAKE_LLM_GGUF="${WAKE_LLM_GGUF:-/home/mahrad/storage/Data/Server/models/qwen2.5-7b-instruct-q4_k_m.gguf}" \
  WAKE_WHISPER_DIR="${WAKE_WHISPER_DIR:-/home/mahrad/storage/Data/Server/models/faster-whisper-large-v3}" \
  WAKE_LLM_GPU_LAYERS="${WAKE_LLM_GPU_LAYERS:-20}" WAKE_WHISPER_DEVICE=cuda WAKE_WHISPER_COMPUTE=int8_float16 \
  python wake_worker.py
