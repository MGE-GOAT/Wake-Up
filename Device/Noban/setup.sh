#!/bin/bash
# =========================================================
# Setup script for Kali Linux Rolling (x86_64)
# Run once: chmod +x setup_kali.sh && ./setup_kali.sh
# =========================================================
set -e

echo "╔══════════════════════════════════════════╗"
echo "║   Farsi Pipeline — Kali Linux Setup      ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── System dependencies ───────────────────────────────────
echo "=== [1/6] Installing system dependencies ==="
sudo apt update
sudo apt install -y \
    cmake \
    build-essential \
    git \
    wget \
    curl \
    libasound2-dev \
    autoconf \
    automake \
    libtool \
    pkg-config \
    python3 \
    python3-pip \
    libgomp1 \
    libopencv-dev \
    libpaho-mqtt-dev libpaho-mqttpp-dev \
    libsodium-dev \
    network-manager   # OpenCV for fall_detection; paho for MQTT (Phase 5.1); libsodium for E2E media crypto (Phase 3); NetworkManager for wifi_setup.sh AP mode

mkdir -p deps models

# ── BLE provisioning sidecar (Phase 7) — `bless` is pip-only ──────────────
# Spawned by main.cpp when the button is held 3-10s. Replaces the legacy
# AP-mode wifi_setup.sh flow.
pip3 install --break-system-packages bless 2>&1 | tail -3 || \
  pip3 install --user bless 2>&1 | tail -3
# Bluez should already be present on Pi OS, but ensure the daemon is up.
sudo systemctl enable --now bluetooth 2>&1 | head -2

# ── RNNoise ───────────────────────────────────────────────
echo ""
echo "=== [2/6] Building RNNoise ==="
if [ ! -d "deps/rnnoise" ]; then
    git clone https://github.com/xiph/rnnoise deps/rnnoise
fi
cd deps/rnnoise
./autogen.sh
./configure
make -j$(nproc)
sudo make install
sudo ldconfig
cd ../..
echo "    ✅ RNNoise done"

# ── pocketfft (header-only) ───────────────────────────────
# Replaced KFR with pocketfft so the C++ STFT/YIN FFT are bit-exact vs
# numpy.fft / scipy.fft (which use pocketfft internally since numpy 1.17).
# The header lives at deps/pocketfft/pocketfft_hdronly.h and CMakeLists
# adds that dir to the include path.
echo ""
echo "=== [3/6] Cloning pocketfft ==="
if [ ! -d "deps/pocketfft" ]; then
    git clone https://github.com/mreineck/pocketfft deps/pocketfft
fi
echo "    ✅ pocketfft done"

# whisper.cpp and llama.cpp builds were dropped — the device doesn't run
# STT or LLM locally. /Wake_Command POSTs the audio to the PC server which
# does faster-whisper STT + Qwen intent classification and returns
# PILL/CALL/MESSAGE.

# ── ONNX Runtime x86_64 ───────────────────────────────────
echo ""
echo "=== [4/6] Downloading ONNX Runtime ==="
if [ ! -d "deps/onnxruntime" ]; then
    ORT_VERSION="1.17.3"
    wget --show-progress \
        "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" \
        -O /tmp/ort.tgz
    mkdir -p deps/onnxruntime
    tar -xf /tmp/ort.tgz -C deps/onnxruntime --strip-components=1
    rm /tmp/ort.tgz
fi
echo "    ✅ ONNX Runtime done"

# ── TFLite ────────────────────────────────────────────────
# Must pin to r2.13 — the wake-word model (Ds-CNN-QAT) was trained on TF 2.13.
# Newer TFLite kernels (r2.16+) drift int8 outputs by 1-12 LSB and can flip
# class predictions on borderline samples. See memory: kali-tflite-version.
echo ""
echo "=== [5/6] Building TFLite (r2.13) ==="
if [ ! -d "deps/tflite" ]; then
    sudo apt install -y libflatbuffers-dev

    git clone --depth 1 -b r2.13 \
        https://github.com/tensorflow/tensorflow \
        deps/tensorflow-src

    # GCC 13+ no longer pulls <cstdint> transitively. r2.13's spectrogram.cc
    # uses uint32_t directly — patch the include.
    SPECTROGRAM="deps/tensorflow-src/tensorflow/lite/kernels/internal/spectrogram.cc"
    if ! grep -q "include <cstdint>" "$SPECTROGRAM"; then
        sed -i '/^#include <math.h>$/a\\n#include <cstdint>' "$SPECTROGRAM"
    fi

    mkdir -p deps/tensorflow-src/tflite_build
    cd deps/tensorflow-src/tflite_build

    # First configure run will fail on the Iran network — Google mirror returns
    # 403 for fft2d/neon2sse. Run it once just to lay down the FetchContent
    # download stubs, then pre-fetch from github before retrying.
    cmake ../tensorflow/lite \
        -DCMAKE_BUILD_TYPE=Release \
        -DTFLITE_ENABLE_XNNPACK=ON \
        -DTFLITE_ENABLE_GPU=OFF \
        -DFETCHCONTENT_SOURCE_DIR_TENSORFLOW=.. \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || true

    # Pre-fetch deps that fail behind shecan/Iran proxy. Hash-verified by cmake.
    FFT2D_DEST="_deps/fft2d-subbuild/fft2d-populate-prefix/src/v1.0.tar.gz"
    NEON_DEST="_deps/neon2sse-subbuild/neon2sse-populate-prefix/src/a15b489e1222b2087007546b4912e21293ea86ff.tar.gz"
    [ -s "$FFT2D_DEST" ] || curl -L --max-time 120 -o "$FFT2D_DEST" \
        https://github.com/petewarden/OouraFFT/archive/v1.0.tar.gz
    [ -s "$NEON_DEST" ]  || curl -L --max-time 120 -o "$NEON_DEST" \
        https://github.com/intel/ARM_NEON_2_x86_SSE/archive/a15b489e1222b2087007546b4912e21293ea86ff.tar.gz
    # psimd uses ExternalProject_Add(GIT_REPOSITORY) — XNNPACK invokes the
    # subbuild during FP16's configure. If the auto-fetch silently fails it
    # leaves psimd-source empty, breaking the FP16 ADD_SUBDIRECTORY. Pre-clone.
    if [ ! -d "psimd-source/.git" ]; then
        rm -rf psimd-source
        git clone --depth 1 https://github.com/Maratyszcza/psimd.git psimd-source
    fi

    cmake ../tensorflow/lite \
        -DCMAKE_BUILD_TYPE=Release \
        -DTFLITE_ENABLE_XNNPACK=ON \
        -DTFLITE_ENABLE_GPU=OFF \
        -DFETCHCONTENT_SOURCE_DIR_TENSORFLOW=.. \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5

    cmake --build . -j$(nproc)

    # Stage headers and the single static lib (r2.13 bakes the XNNPACK
    # delegate directly into libtensorflow-lite.a — no separate
    # libxnnpack-delegate.a or libxnnpack-microkernels-prod.a target).
    mkdir -p ../../tflite/lib ../../tflite/include/tensorflow/lite
    cp libtensorflow-lite.a ../../tflite/lib/
    cd ../../tensorflow-src
    find tensorflow/lite -name "*.h" | while read f; do
        dest="../../tflite/include/$f"
        mkdir -p "$(dirname "$dest")"
        cp "$f" "$dest"
    done
    cd ../..
fi
echo "    ✅ TFLite r2.13 done"

# ── Wi-Fi provisioning script installation ────────────────────────────────
# wifi_setup.sh + wifi_setup_server.py must live somewhere on PATH that's
# stable across reboots. /usr/local/bin is the conventional spot. We also
# grant the invoking user passwordless sudo for nmcli (required by the
# script's AP up/down). Comment out the sudoers line if your distro
# already grants this via a group.
#
# Note on models: the device-side models (dscnn_int8.tflite, silero_vad.onnx,
# yamnet.tflite, pain_logmel5.tflite + the three fall models) are SHIPPED
# IN THE BUNDLE alongside this script. No model download step is needed —
# STT and LLM live on the PC server, not the device.
echo ""
echo "=== [6/6] Installing Wi-Fi provisioning helper ==="
sudo install -m 755 wifi_setup.sh         /usr/local/bin/wifi_setup.sh
sudo install -m 755 wifi_setup_server.py  /usr/local/bin/wifi_setup_server.py
# passwordless nmcli for the current user (idempotent insert)
SUDOERS_LINE="$USER ALL=(root) NOPASSWD: /usr/bin/nmcli"
if ! sudo grep -qsxF "$SUDOERS_LINE" /etc/sudoers.d/wifi-setup; then
    echo "$SUDOERS_LINE" | sudo tee /etc/sudoers.d/wifi-setup > /dev/null
    sudo chmod 440 /etc/sudoers.d/wifi-setup
fi
echo "Wi-Fi provisioning helper installed."

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║   ✅ Setup complete!                      ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "Next steps:"
echo ""
echo "  1. Build the pipeline:"
echo "     mkdir build && cd build && cmake .. && make -j\$(nproc) && cd .."
echo ""
echo "  2. Run:"
echo "     ./dev_run.sh        # or ./build/pipeline"
echo ""
echo "  Tip: if mic not found, run 'arecord -l' to find your device name"
echo "  and update main.cpp. Models are already in ./models/ and the fall"
echo "  triad (4.tflite, Last_stage_Model.onnx, SVM_rbf.onnx) is in the"
echo "  CWD — the binary loads them by relative path so run from this dir."
