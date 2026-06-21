#!/bin/bash
# Launch the GPU wake-command worker (the ONLY process that loads the models).
# Run alongside run_async.sh. Needs a CUDA GPU + the worker deps
# (requirements-wake.txt). Scale by running N copies on a bigger GPU.
#
# Python env: prefers the .venv created by ./setup_server.sh; if there is no
# .venv it falls back to a conda env (override with CONDA_ROOT / CONDA_ENV).
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"

# Optional gitignored secrets (DB_PASSWORD, REDIS_URL, model path overrides, ...).
[ -f local-lan/secrets.env ] && set -a && . local-lan/secrets.env && set +a

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
        echo "ERROR: no .venv found — run ./setup_server.sh first (it creates .venv)," >&2
        echo "       or point CONDA_ROOT/CONDA_ENV at an existing conda env." >&2
        exit 1
    fi
fi

# --- CUDA runtime libs from pip 'nvidia-*' packages (venv or conda), best-effort -
CUDA_LIBS=""
for d in "$ROOT/.venv"/lib/python*/site-packages/nvidia/*/lib \
         "$HOME"/miniconda3/envs/*/lib/python*/site-packages/nvidia/*/lib; do
    [ -d "$d" ] && CUDA_LIBS="${CUDA_LIBS}${d}:"
done

env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  LD_LIBRARY_PATH="${CUDA_LIBS}${LD_LIBRARY_PATH:-}" \
  REDIS_URL="${REDIS_URL:-redis://127.0.0.1:6379}" \
  WAKE_LLM_GGUF="${WAKE_LLM_GGUF:-$ROOT/models/qwen2.5-7b-instruct-q4_k_m.gguf}" \
  WAKE_WHISPER_DIR="${WAKE_WHISPER_DIR:-$ROOT/models/faster-whisper-large-v3}" \
  WAKE_LLM_GPU_LAYERS="${WAKE_LLM_GPU_LAYERS:-20}" \
  WAKE_WHISPER_DEVICE="${WAKE_WHISPER_DEVICE:-cuda}" WAKE_WHISPER_COMPUTE="${WAKE_WHISPER_COMPUTE:-int8_float16}" \
  "$PY" wake_worker.py
