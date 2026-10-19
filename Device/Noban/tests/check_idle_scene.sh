#!/bin/bash
# Guard for the fall detector build: an empty, static scene must cost the pose model nothing.
# History: when the fall_cpp sources were compiled with -ffast-math the detector found a "person" in an empty
# scene and ran the pose model on every batch (about 1.2 s of compute per second of video), so batches were
# dropped constantly. Run this on the Pi after every build of the pipeline binary. Exit 0 = pass.
#   usage: tests/check_idle_scene.sh [path to pipeline binary, default ./build/pipeline]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./build/pipeline}
VIDEO=$(mktemp /tmp/idle_scene_XXXX.mp4)
trap 'rm -f "$VIDEO"' EXIT
ffmpeg -loglevel error -y -f lavfi -i "color=c=gray:s=640x480:r=16" -t 40 -pix_fmt yuv420p "$VIDEO"
export OMP_WAIT_POLICY=PASSIVE FALL_STATS=1 FALL_NICE=0
export FALL_CPP_CONFIG=${FALL_CPP_CONFIG:-fall_cpp/noban.yaml}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-$PWD/deps/onnxruntime/lib}
OUT=$("$BIN" --fall-offline "$VIDEO" 2>&1)
# Timing windows are printed every 10 batches. After the 3 warm-up batches every window must show deep 0.
WINDOWS=$(echo "$OUT" | grep -c "mean ms per batch" || true)
BAD=$(echo "$OUT" | grep "mean ms per batch" | sed 's/.*deep //; s/ .*//' | awk '$1 > 1 {n++} END {print n+0}')
if [ "$WINDOWS" -lt 2 ]; then echo "FAIL: expected at least 2 timing windows, got $WINDOWS"; exit 2; fi
if [ "$BAD" -gt 0 ]; then
  echo "FAIL: the pose model ran on an empty scene in $BAD of $WINDOWS windows (check the fall_cpp compile flags, no -ffast-math)"
  echo "$OUT" | grep "mean ms per batch"
  exit 1
fi
echo "PASS: empty scene costs the pose model nothing ($WINDOWS windows, deep 0 ms)"
