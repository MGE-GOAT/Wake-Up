#!/bin/bash
# Noban demo server launcher for the laptop. Auto-detects VRAM → GPU layers.
cd "$(dirname "$0")"

# Optional gitignored secrets file (SMTP_SENDER_PASSWORD, DB_PASSWORD, ...).
[ -f "$(dirname "$0")/secrets.env" ] && set -a && . "$(dirname "$0")/secrets.env" && set +a

# activate the conda-pack'd env (edit path if you unpacked elsewhere):
source ~/miniconda3/envs/app/bin/activate 2>/dev/null || { echo "Activate the unpacked 'app' env first (see SETUP.md)"; exit 1; }

VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1)
if   [ -z "$VRAM" ];        then GL=0;  WDEV=cpu;  echo "No GPU detected → CPU (wake commands will be slow)";
elif [ "$VRAM" -ge 7500 ];  then GL=32; WDEV=cuda;
elif [ "$VRAM" -ge 5500 ];  then GL=20; WDEV=cuda; echo "~6GB GPU → partial offload";
elif [ "$VRAM" -ge 3500 ];  then GL=12; WDEV=cuda; echo "~4GB GPU → heavy CPU offload (wake commands slower); calls/pills fine";
else GL=0; WDEV=cpu; fi
echo "VRAM=${VRAM:-?}MiB  →  WAKE_LLM_GPU_LAYERS=$GL  whisper=$WDEV"

env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  SMTP_PROXY_HOST=127.0.0.1 SMTP_PROXY_PORT=10808 SMTP_PROXY_TYPE=socks5 \
  SMTP_SENDER_PASSWORD="${SMTP_SENDER_PASSWORD:-}" \
  DATABASE_URL="postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care" \
  WAKE_LLM_GGUF="$(pwd)/models/qwen2.5-7b-instruct-q4_k_m.gguf" \
  WAKE_WHISPER_DIR="$(pwd)/models/faster-whisper-large-v3" \
  WAKE_LLM_GPU_LAYERS=$GL WAKE_WHISPER_DEVICE=$WDEV WAKE_WHISPER_COMPUTE=int8_float16 \
  uvicorn app_async:app --host 0.0.0.0 --port "${PORT:-5000}"
