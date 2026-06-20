#!/usr/bin/env bash
# Download the models the Noban server needs (NOT tracked in git — ~8 GB total):
#   - faster-whisper-large-v3  (STT, FULL large-v3 — non-negotiable; turbo loses far-field accuracy)
#   - Qwen2.5-7B-Instruct q4_K_M GGUF  (intent LLM)
# On a restricted network, export HTTPS_PROXY / HTTP_PROXY before running.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p models

WHISPER_DIR="models/faster-whisper-large-v3"
LLM_GGUF="models/qwen2.5-7b-instruct-q4_k_m.gguf"

echo "==> faster-whisper-large-v3"
if [ ! -f "$WHISPER_DIR/model.bin" ]; then
  if command -v huggingface-cli >/dev/null 2>&1; then
    huggingface-cli download Systran/faster-whisper-large-v3 --local-dir "$WHISPER_DIR"
  else
    mkdir -p "$WHISPER_DIR"
    base="https://huggingface.co/Systran/faster-whisper-large-v3/resolve/main"
    # vocabulary is published as .json OR .txt depending on the snapshot; we
    # require AT LEAST ONE of them (checked below). The rest are mandatory.
    for f in model.bin config.json tokenizer.json preprocessor_config.json vocabulary.json vocabulary.txt; do
      echo "   $f"; curl -fL "$base/$f" -o "$WHISPER_DIR/$f" 2>/dev/null || true
    done
  fi
else
  echo "   already present"
fi

# Verify ALL required whisper files exist — a partial download must NOT report
# success (faster-whisper crashes at load time otherwise, not at download time).
missing=()
for f in model.bin config.json tokenizer.json preprocessor_config.json; do
  [ -s "$WHISPER_DIR/$f" ] || missing+=("$f")
done
# vocabulary.json OR vocabulary.txt is acceptable.
if [ ! -s "$WHISPER_DIR/vocabulary.json" ] && [ ! -s "$WHISPER_DIR/vocabulary.txt" ]; then
  missing+=("vocabulary.json|vocabulary.txt")
fi
if [ "${#missing[@]}" -ne 0 ]; then
  echo "!! faster-whisper-large-v3 is INCOMPLETE — missing: ${missing[*]}" >&2
  echo "!! Delete '$WHISPER_DIR' and re-run (export HTTPS_PROXY first on restricted nets)," >&2
  echo "!! or install huggingface-cli (pip install -U 'huggingface_hub[cli]') for a robust download." >&2
  exit 1
fi
echo "   whisper files OK"

echo "==> Qwen2.5-7B-Instruct q4_K_M GGUF"
if [ ! -f "$LLM_GGUF" ]; then
  # Official Qwen GGUF repo. If the filename/casing differs, adjust the URL.
  curl -fL "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf" \
    -o "$LLM_GGUF" \
    || echo "   !! download failed — grab a Qwen2.5-7B-Instruct Q4_K_M .gguf manually into $LLM_GGUF"
else
  echo "   already present"
fi

echo "Done. Models in ./models/"
