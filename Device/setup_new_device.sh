#!/usr/bin/env bash
# =============================================================================
# setup_new_device.sh — provision a BRAND-NEW Raspberry Pi into a running
#                        Noban elderly-care device.
#
# ⚠ OS REQUIREMENT — USE RASPBERRY PI OS *BOOKWORM* (Debian 12), 64-bit / arm64.
#   Do NOT use Trixie (Debian 13). Trixie ships BlueZ 5.82, which uses LE
#   *extended* advertising and REJECTS the device's connectable provisioning
#   advert ("Failed to add advertisement: Invalid Parameters (0x0d)"), so the
#   button→BLE/app onboarding (ble_provisioning.py) silently never advertises.
#   Bookworm ships BlueZ 5.66 (legacy advertising), where onboarding works.
#   (Root-caused + verified June 2026 on a Pi 4 / BCM4345C0; see docs/SETUP.md.)
#
# WHAT THIS DOES
#   Takes a fresh Raspberry Pi OS (Bookworm, 64-bit / arm64) install and brings
#   it all the way to a fully running Noban device: apt deps, i2s/i2c boot
#   config + ALSA routing for the INMP441 mic and MAX98357A amp, the native
#   deps (rnnoise, pocketfft, ONNX Runtime, TFLite r2.13), the CMake build of
#   the C++ pipeline, the systemd units, and the server/MQTT pointing.
#
# GROUNDED IN (source of truth — do not invent beyond these):
#   - Noban/setup.sh                (apt list, rnnoise/pocketfft/ort/tflite builds)
#   - setup.log                     (real apt run: Trixie arm64, OpenCV 4.10, etc.)
#   - Noban/CMakeLists.txt          (libs linked, USE_S32_CAPTURE, ORT/TFLite layout)
#   - systemd/*                     (ExecStart, WorkingDirectory, Environment, User)
#   - Noban/run_camera.sh           (rpicam-vid -> ffmpeg -> /dev/video9)
#   - Noban/main.cpp, mqtt_client.cpp (SERVER_URL/MQTT_HOST/CAMERA_DEV env, id.txt)
#   - Noban/wake_call_helper.py, ble_provisioning.py, show_noban.py (python deps)
#
# ASSUMPTIONS
#   * Raspberry Pi 4 or 5, 64-bit Raspberry Pi OS *BOOKWORM* (see OS REQUIREMENT
#     above — Trixie's BlueZ 5.82 breaks BLE onboarding). (setup.sh's header says
#     "Kali x86_64" and downloads the x64 ONNX Runtime — that is wrong for the Pi;
#     this script fixes it by selecting the aarch64 ORT build.)
#   * Run as the 'pi' user (the systemd units hard-code User=pi and
#     /home/pi/device_bundle/Noban). Has sudo. Network reachable (deps are
#     cloned/downloaded from github at build time).
#   * The Noban/ source tree (this repo's Noban dir, incl. models/ and the 3
#     fall models) is present next to this script. It gets copied into
#     ${DEVICE_BUNDLE}/Noban (= the WorkingDirectory the systemd units expect).
#
# IDEMPOTENT: re-runnable. Every step guards against re-doing completed work
# (dir-exists checks, grep-before-append for config files, install -m, etc.).
#
# USAGE:  chmod +x setup_new_device.sh && ./setup_new_device.sh
# =============================================================================
set -euo pipefail

# =============================================================================
# 0. CONFIG — edit these for each deployment
# =============================================================================
# Where the device is told to find the Noban server. main.cpp reads SERVER_URL
# from the environment (set by systemd) and falls back to the compiled-in
# SERVER_BASE_URL only if unset. MQTT_HOST/MQTT_PORT likewise override the
# compiled defaults (mqtt_client via main.cpp builds tcp://$MQTT_HOST:$MQTT_PORT).
# The shipped systemd units use "noban.local"; change to an IP if you have no
# mDNS. SERVER_URL must include the scheme + :5000 port.
SERVER_HOST="${SERVER_HOST:-noban.local}"          # server hostname or IP
SERVER_URL="${SERVER_URL:-http://${SERVER_HOST}:5000}"
# OPTIONAL static IP for the server. If set, we pin "<SERVER_IP> noban.local" in
# /etc/hosts so the device never depends on flaky mDNS to resolve the server
# (mDNS resolving a stale IP was a real cause of dropped heartbeats). Leave empty
# to rely on mDNS/DNS. Example: SERVER_IP=192.168.0.154
SERVER_IP="${SERVER_IP:-}"
MQTT_HOST="${MQTT_HOST:-${SERVER_HOST}}"           # broker host (default = server)
MQTT_PORT="${MQTT_PORT:-1883}"                      # paho default; mqtt_client default
CAMERA_DEV="${CAMERA_DEV:-/dev/video9}"            # v4l2loopback fed by run_camera.sh

# Device identity. main.cpp reads /var/lib/elderly-care/id.txt (line1=device_id,
# line2=32-char shared password). If absent the device boots UNPROVISIONED as
# FACTORY_ID "Noban" and only runs BLE provisioning (no server traffic). Leave
# DEVICE_ID empty to leave the device unprovisioned for app/BLE onboarding, or
# set both to write id.txt now.
DEVICE_ID="${DEVICE_ID:-}"                          # e.g. ID_12233945  (empty = BLE onboard)
DEVICE_PASSWORD="${DEVICE_PASSWORD:-}"             # 32-char shared secret matching server key_hash

# Filesystem layout. The systemd units hard-code /home/pi/device_bundle/Noban
# as WorkingDirectory + binary path, so this is the install target. SRC_DIR is
# where this script + the Noban/ source currently live.
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_BUNDLE="${DEVICE_BUNDLE:-/home/pi/device_bundle}"
NOBAN_DIR="${DEVICE_BUNDLE}/Noban"                  # = WorkingDirectory in systemd units
SYSTEMD_SRC="${SRC_DIR}/systemd"

# Native dep versions (from setup.sh).
ORT_VERSION="1.17.3"
TFLITE_BRANCH="r2.13"

say() { echo -e "\n\033[1;36m=== $* ===\033[0m"; }
ok()  { echo -e "    \033[1;32m✓\033[0m $*"; }

say "Noban device provisioning — server=${SERVER_URL} mqtt=${MQTT_HOST}:${MQTT_PORT} bundle=${NOBAN_DIR}"

# -----------------------------------------------------------------------------
# Keep sudo alive for the WHOLE run. This script needs root (apt / systemd / /etc),
# but the TFLite build (stage 3d) takes ~30 min with no sudo calls in between —
# long enough for the sudo timestamp to expire, after which a later step would
# re-prompt (and hang if you walked away) or fail outright. So authenticate ONCE
# up front, then refresh the timestamp in the background until the script exits.
# => One password prompt for the entire run; safe to start it and walk away.
if ! sudo -n true 2>/dev/null; then
    echo "This setup needs administrator rights. Enter your password once —"
    echo "it stays valid for the whole install (you won't be asked again):"
    sudo -v
fi
( while true; do sudo -n true 2>/dev/null || true; sleep 50; kill -0 "$$" 2>/dev/null || exit 0; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true' EXIT

# =============================================================================
# 1. APT DEPENDENCIES
#    Build/runtime deps from setup.sh (verified against setup.log: cmake,
#    build-essential, libasound2-dev, libopencv-dev, paho, libsodium,
#    network-manager all installed on Trixie arm64). Added below the setup.sh
#    set: deps the CODE/units need that setup.sh assumed pre-present on Pi OS —
#    libgpiod-dev (CMake find_library gpiod), libssl-dev (OpenSSL::Crypto),
#    libcurl4-openssl-dev (CURL REQUIRED), libflatbuffers-dev (TFLite, was
#    installed inline in setup.sh), alsa-utils (aplay/arecord used by main.cpp +
#    wake_call_helper), ffmpeg + rpicam (run_camera.sh), bluez (BLE), python3
#    helpers' libs are pip'd in step 1b.
# =============================================================================
say "[1/8] apt dependencies"
sudo apt-get update
sudo apt-get install -y \
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
    network-manager \
    libflatbuffers-dev \
    libgpiod-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    alsa-utils \
    ffmpeg \
    bluez
# rpicam-apps provides rpicam-vid (run_camera.sh). On Pi OS it is usually
# pre-installed; install if available, don't fail the run if the name differs.
sudo apt-get install -y rpicam-apps 2>/dev/null || \
    sudo apt-get install -y libcamera-apps 2>/dev/null || \
    echo "    # VERIFY: rpicam-apps not found by either name — confirm 'rpicam-vid' exists on the Pi"
ok "apt deps installed"

# ── 1b. Python helper deps ───────────────────────────────────────────────────
# bless          -> ble_provisioning.py (BLE GATT onboarding)
# aiortc         -> wake_call_helper.py (Janus WebRTC call daemon)
#   numpy, requests   (aiortc pulls a COMPATIBLE 'av' wheel itself — do NOT list
#   'av' explicitly; a source build of PyAV on arm64 frequently fails.)
# smbus2, Pillow -> show_noban.py (OLED splash, raw I2C SSD1306/SH1106)
say "[1/8b] python helper deps (bless / aiortc / smbus2 / Pillow ...)"
PIP="pip3 install --break-system-packages"
PKGS="bless aiortc numpy requests smbus2 Pillow aiohttp"
$PIP $PKGS 2>&1 | tail -5 || \
  pip3 install --break-system-packages --user $PKGS 2>&1 | tail -5
sudo systemctl enable --now bluetooth 2>&1 | head -2 || true
ok "python helper deps installed"

# =============================================================================
# 2. BOOT CONFIG — i2s mic + i2s amp + i2c OLED + /etc/asound.conf
#    INMP441 (capture) + MAX98357A (playback) both ride the googlevoicehat
#    overlay (ALSA card name "sndrpigooglevoi" — confirmed by main.cpp /
#    pain_pipeline.cpp playback device "plughw:CARD=sndrpigooglevoi,DEV=0").
#    OLED is on I2C 0x3C, needs dtparam=i2c_arm=on. Capture goes through a
#    "micleft" ALSA plug (ALSA_CAPTURE_DEV="micleft" in main.cpp) that picks
#    the INMP441 left channel, S32, and resamples 48k->16k.
# =============================================================================
say "[2/8] boot config (i2s / i2c overlay + asound.conf)"
# Pi OS Trixie/Bookworm uses /boot/firmware/config.txt; older images use /boot/config.txt
CONFIG_TXT=/boot/firmware/config.txt
[ -f "$CONFIG_TXT" ] || CONFIG_TXT=/boot/config.txt
ok "using $CONFIG_TXT"

append_cfg() {  # idempotent line append to config.txt
    local line="$1"
    if ! grep -qxF "$line" "$CONFIG_TXT" 2>/dev/null; then
        echo "$line" | sudo tee -a "$CONFIG_TXT" >/dev/null
        ok "added: $line"
    fi
}
# Marker block so re-runs are obvious in the file.
if ! grep -q "# --- Noban device ---" "$CONFIG_TXT" 2>/dev/null; then
    echo "# --- Noban device ---" | sudo tee -a "$CONFIG_TXT" >/dev/null
fi
# Ensure exactly the lines present on the live Pi's /boot/firmware/config.txt
# (system-config/config.txt). I2C OLED (0x3C @ 400kHz) + i2s + on-board audio
# enabled + the googlevoicehat soundcard overlay (INMP441 mic + MAX98357A amp)
# + audremap for the amp pins, vc4-kms-v3d for display, dwc2 host for USB.
append_cfg "dtparam=i2c_arm=on"
append_cfg "dtparam=i2c_arm_baudrate=400000"
append_cfg "dtparam=i2s=on"
append_cfg "dtparam=audio=on"
append_cfg "dtoverlay=vc4-kms-v3d"
append_cfg "dtoverlay=dwc2,dr_mode=host"
append_cfg "dtoverlay=googlevoicehat-soundcard"
append_cfg "dtoverlay=audremap,pins_12_13"

# ── /etc/asound.conf ─────────────────────────────────────────────────────────
# Defines the "micleft" capture plug main.cpp opens (INMP441 LEFT channel, mono)
# and routes "default" playback to the MAX98357A amp. This is the REAL file
# pulled off the live Pi (system-config/asound.conf): "default" is asym
# capture=micleft / playback=plughw:CARD=sndrpigooglevoi, plus the callmic +
# micleft route plugs (ttable.0.0 1).
sudo install -m 644 "${SRC_DIR}/system-config/asound.conf" /etc/asound.conf
ok "installed /etc/asound.conf"

# ── v4l2loopback for /dev/video9 (camera feeder target) ──────────────────────
# run_camera.sh pipes rpicam-vid -> ffmpeg -> /dev/video9. /dev/video9 is the
# v4l2loopback device created by the REAL module config pulled off the live Pi:
#   /etc/modules-load.d/v4l2loopback.conf  -> "v4l2loopback" (load at boot)
#   /etc/modprobe.d/v4l2loopback.conf      -> options video_nr=9 card_label=picam
#                                             exclusive_caps=1
say "[2/8b] v4l2loopback (/dev/video9)"
if ! lsmod 2>/dev/null | grep -q v4l2loopback; then
    sudo apt-get install -y v4l2loopback-dkms 2>/dev/null || \
        echo "    # VERIFY: v4l2loopback-dkms not installable here — confirm how /dev/video9 is created on the Pi"
fi
# Install the live-Pi module configs verbatim (load at boot + fixed video_nr=9).
sudo install -m 644 "${SRC_DIR}/system-config/modules-load.d-v4l2loopback.conf" \
    /etc/modules-load.d/v4l2loopback.conf
sudo install -m 644 "${SRC_DIR}/system-config/modprobe.d-v4l2loopback.conf" \
    /etc/modprobe.d/v4l2loopback.conf
sudo modprobe v4l2loopback video_nr=9 card_label=picam exclusive_caps=1 2>/dev/null || \
    echo "    # VERIFY: modprobe v4l2loopback failed — /dev/video9 may need a reboot or a different module"
ok "v4l2loopback configured for video_nr=9 (card_label=picam)"

# =============================================================================
# 2c. NETWORK ROBUSTNESS — the two biggest causes of intermittent stalls on a Pi
#     elderly-care device: Wi-Fi power-save (radio sleeps → 1-5s latency spikes /
#     dropped packets) and mDNS resolving a stale server IP. Fix both here.
# =============================================================================
say "[2/8c] network robustness (Wi-Fi power-save off + server host pin)"

# --- Disable Wi-Fi power-save persistently (survives reboot) + immediately. ---
# A tiny boot service is the most driver-agnostic way (works regardless of
# NetworkManager/dhcpcd/wpa_supplicant). iw applies to whatever wlan iface exists.
sudo tee /etc/systemd/system/wifi-powersave-off.service >/dev/null <<'UNIT'
[Unit]
Description=Disable Wi-Fi power save (avoids latency spikes / dropped packets)
After=network.target

[Service]
Type=oneshot
# Apply to every wireless iface; never fail the boot if none exists.
ExecStart=/bin/sh -c 'for i in $(ls /sys/class/net | grep -E "^wlan|^wlp"); do /sbin/iw dev "$i" set power_save off || true; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl enable --now wifi-powersave-off.service 2>&1 | head -1 || \
    echo "    # VERIFY: could not enable wifi-powersave-off.service"
# One-time immediate apply (the service handles reboots).
for i in $(ls /sys/class/net 2>/dev/null | grep -E "^wlan|^wlp"); do
    sudo iw dev "$i" set power_save off 2>/dev/null \
        && ok "Wi-Fi power-save off on $i" \
        || echo "    # VERIFY: 'iw dev $i set power_save off' failed (no iw / no wlan?)"
done

# --- Pin the server's static IP in /etc/hosts (skip mDNS) if SERVER_IP given. ---
if [ -n "${SERVER_IP}" ]; then
    # Idempotent: drop any prior noban.local line, then add the pinned one.
    sudo sed -i '/[[:space:]]noban\.local$/d' /etc/hosts 2>/dev/null || true
    echo "${SERVER_IP} noban.local" | sudo tee -a /etc/hosts >/dev/null
    ok "/etc/hosts pinned: ${SERVER_IP} noban.local"
else
    echo "    # NOTE: SERVER_IP unset — relying on mDNS/DNS for ${SERVER_HOST}."
    echo "    #       If heartbeats drop intermittently, re-run with SERVER_IP=<ip>."
fi

# =============================================================================
# 3. STAGE THE SOURCE + BUILD NATIVE DEPS
#    Copy the Noban source into the bundle path the systemd units expect, then
#    build/fetch the vendored deps under Noban/deps exactly as setup.sh did
#    (the 1.5GB deps/ tree is NOT shipped — we rebuild it here).
# =============================================================================
say "[3/8] stage source into ${NOBAN_DIR}"
sudo mkdir -p "$DEVICE_BUNDLE"
sudo chown "$(id -un):$(id -gn)" "$DEVICE_BUNDLE"
# rsync so re-runs only copy changes; preserve any existing deps/ build.
if command -v rsync >/dev/null; then
    rsync -a --exclude 'build/' --exclude 'deps/' "${SRC_DIR}/Noban/" "${NOBAN_DIR}/"
else
    mkdir -p "$NOBAN_DIR"
    cp -ru "${SRC_DIR}/Noban/." "${NOBAN_DIR}/"
fi
ok "source staged"

cd "$NOBAN_DIR"
mkdir -p deps models

# ── 3a. RNNoise (from setup.sh) ──────────────────────────────────────────────
# NOTE: setup.log shows the rnnoise autogen tries to wget a model from
# media.xiph.org and FAILS behind a restricted network. autogen.sh fetches the
# model; if that host is unreachable, pre-place the model or run with a working
# DNS. We retry and continue — the build still links if the model lands.
say "[3a] build RNNoise"
if [ ! -f /usr/local/lib/librnnoise.so ] && [ ! -f deps/rnnoise/.libs/librnnoise.so ]; then
    [ -d deps/rnnoise ] || git clone https://github.com/xiph/rnnoise deps/rnnoise
    # autogen.sh wgets the model from media.xiph.org. Under `set -e` a failure here
    # would abort the whole script with no explanation — guard it and give a clear
    # error + recovery hint instead of dying mid-run.
    if ! ( cd deps/rnnoise
           ./autogen.sh        # downloads the rnnoise model — needs network/DNS
           ./configure
           make -j"$(nproc)"
           sudo make install
           sudo ldconfig ); then
        echo "" >&2
        echo "ERROR: RNNoise build failed (commonly autogen.sh cannot reach media.xiph.org" >&2
        echo "       to fetch the model — DNS/network restricted)." >&2
        echo "  Fix one of:" >&2
        echo "   - retry on a working network / with a proxy (export HTTPS_PROXY), then re-run this script; or" >&2
        echo "   - pre-place the rnnoise model so autogen.sh skips the download, then re-run." >&2
        echo "  This step is required — the C++ pipeline links against librnnoise.so." >&2
        exit 1
    fi
    ok "RNNoise built + installed"
else
    ok "RNNoise already installed"
fi

# ── 3b. pocketfft (header-only, from setup.sh) ───────────────────────────────
say "[3b] fetch pocketfft"
[ -d deps/pocketfft ] || git clone https://github.com/mreineck/pocketfft deps/pocketfft
ok "pocketfft present"

# ── 3c. ONNX Runtime — ARM64 build (setup.sh fetched x64; that is wrong for
#       the Pi). CMakeLists links deps/onnxruntime/lib/libonnxruntime.so and
#       the systemd override sets LD_LIBRARY_PATH to deps/onnxruntime/lib.
say "[3c] ONNX Runtime ${ORT_VERSION} (aarch64)"
if [ ! -f deps/onnxruntime/lib/libonnxruntime.so ]; then
    arch="$(uname -m)"
    case "$arch" in
        aarch64|arm64) ORT_PKG="onnxruntime-linux-aarch64-${ORT_VERSION}" ;;
        x86_64)        ORT_PKG="onnxruntime-linux-x64-${ORT_VERSION}" ;;
        *) echo "    # VERIFY: unknown arch '$arch' — pick the right ORT package"; ORT_PKG="onnxruntime-linux-aarch64-${ORT_VERSION}" ;;
    esac
    wget --show-progress \
        "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_PKG}.tgz" \
        -O /tmp/ort.tgz
    mkdir -p deps/onnxruntime
    tar -xf /tmp/ort.tgz -C deps/onnxruntime --strip-components=1
    rm -f /tmp/ort.tgz
    ok "ONNX Runtime ${ORT_PKG} installed"
else
    ok "ONNX Runtime already present"
fi

# ── 3d. TFLite r2.13 (from setup.sh, incl. the Iran-network pre-fetch dance &
#       the GCC13 <cstdint> patch). Must be r2.13 — newer kernels drift the
#       int8 wake-word outputs. CMakeLists links the static libs out of
#       deps/tensorflow-src/tflite_build and deps/tflite.
say "[3d] build TFLite ${TFLITE_BRANCH} (this is the long one)"
if [ ! -f deps/tflite/lib/libtensorflow-lite.a ]; then
    [ -d deps/tensorflow-src ] || \
        git clone --depth 1 -b "$TFLITE_BRANCH" \
            https://github.com/tensorflow/tensorflow deps/tensorflow-src

    # GCC 13+ dropped the transitive <cstdint>; r2.13 spectrogram.cc needs it.
    SPECTROGRAM="deps/tensorflow-src/tensorflow/lite/kernels/internal/spectrogram.cc"
    if ! grep -q "include <cstdint>" "$SPECTROGRAM"; then
        sed -i '/^#include <math.h>$/a\\n#include <cstdint>' "$SPECTROGRAM"
    fi

    mkdir -p deps/tensorflow-src/tflite_build
    ( cd deps/tensorflow-src/tflite_build
      # First cmake just lays down the FetchContent stubs (may 403 on some
      # networks — ignore failure).
      cmake ../tensorflow/lite \
          -DCMAKE_BUILD_TYPE=Release \
          -DTFLITE_ENABLE_XNNPACK=ON \
          -DTFLITE_ENABLE_GPU=OFF \
          -DFETCHCONTENT_SOURCE_DIR_TENSORFLOW=.. \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || true

      # Pre-fetch the deps that fail behind restricted networks (hash-verified).
      FFT2D_DEST="_deps/fft2d-subbuild/fft2d-populate-prefix/src/v1.0.tar.gz"
      NEON_DEST="_deps/neon2sse-subbuild/neon2sse-populate-prefix/src/a15b489e1222b2087007546b4912e21293ea86ff.tar.gz"
      mkdir -p "$(dirname "$FFT2D_DEST")" "$(dirname "$NEON_DEST")" 2>/dev/null || true
      [ -s "$FFT2D_DEST" ] || curl -L --max-time 120 -o "$FFT2D_DEST" \
          https://github.com/petewarden/OouraFFT/archive/v1.0.tar.gz || true
      [ -s "$NEON_DEST" ]  || curl -L --max-time 120 -o "$NEON_DEST" \
          https://github.com/intel/ARM_NEON_2_x86_SSE/archive/a15b489e1222b2087007546b4912e21293ea86ff.tar.gz || true
      if [ ! -d psimd-source/.git ]; then
          rm -rf psimd-source
          git clone --depth 1 https://github.com/Maratyszcza/psimd.git psimd-source || true
      fi

      cmake ../tensorflow/lite \
          -DCMAKE_BUILD_TYPE=Release \
          -DTFLITE_ENABLE_XNNPACK=ON \
          -DTFLITE_ENABLE_GPU=OFF \
          -DFETCHCONTENT_SOURCE_DIR_TENSORFLOW=.. \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5

      cmake --build . -j"$(nproc)"

      # Stage the single static lib where CMakeLists expects it (deps/tflite/lib).
      mkdir -p ../../tflite/lib
      cp libtensorflow-lite.a ../../tflite/lib/ )
    ok "TFLite ${TFLITE_BRANCH} built"
else
    ok "TFLite lib already built"
fi

# Stage the TFLite HEADERS into deps/tflite/include (CMakeLists adds this dir to
# the include path; tensorflow/lite/interpreter.h et al MUST land here or the
# pipeline fails to compile). Run this OUTSIDE the build guard + only-if-missing,
# so a re-run that already has the prebuilt lib still heals a missing-headers tree.
# CAREFUL: this subshell runs in deps/tensorflow-src, so the include dir is ONE
# '..' up (../tflite/include = deps/tflite/include). The build subshell above runs
# one level deeper (deps/tensorflow-src/tflite_build) where the same dir is ../../
# — using ../../ here (the old bug) staged headers to Noban/tflite/include and the
# compile then could not find interpreter.h.
if [ ! -f deps/tflite/include/tensorflow/lite/interpreter.h ]; then
    echo "    staging TFLite headers -> deps/tflite/include"
    mkdir -p deps/tflite/include
    ( cd deps/tensorflow-src 2>/dev/null && find tensorflow/lite -name "*.h" | while read -r f; do
          dest="../tflite/include/$f"
          mkdir -p "$(dirname "$dest")"
          cp "$f" "$dest"
      done )
fi
[ -f deps/tflite/include/tensorflow/lite/interpreter.h ] \
    || { echo "ERROR: TFLite headers missing from deps/tflite/include (staging failed)." >&2; \
         echo "       Make sure deps/tensorflow-src exists, then re-run this script." >&2; exit 1; }
ok "TFLite headers staged (deps/tflite/include)"

# =============================================================================
# 4. BUILD THE PIPELINE (CMake)
#    CMakeLists auto-detects ARM64 and applies the Cortex-A72 flags. It finds
#    libgpiod / libsodium / paho / OpenCV / curl / OpenSSL, includes pocketfft,
#    and links the TFLite + ORT trees built above. USE_S32_CAPTURE=1 is baked in.
# =============================================================================
say "[4/8] cmake build"
mkdir -p build
( cd build
  cmake ..
  make -j"$(nproc)" )
[ -x build/pipeline ] && ok "build/pipeline built" || { echo "BUILD FAILED"; exit 1; }

# =============================================================================
# 5. SYSTEMD UNITS
#    Copy from ./systemd, enable elderly-camera, elderly-pipeline,
#    elderly-call-daemon, and the watchdog timer. The pipeline drop-in override
#    (Environment=, ExecStartPost OLED) is copied with the unit.
# =============================================================================
say "[5/8] install systemd units"
sudo cp "${SYSTEMD_SRC}/elderly-pipeline.service"     /etc/systemd/system/
sudo cp "${SYSTEMD_SRC}/elderly-camera.service"       /etc/systemd/system/
sudo cp "${SYSTEMD_SRC}/elderly-call-daemon.service"  /etc/systemd/system/
sudo cp "${SYSTEMD_SRC}/noban-watchdog.service"       /etc/systemd/system/
sudo cp "${SYSTEMD_SRC}/noban-watchdog.timer"         /etc/systemd/system/
sudo mkdir -p /etc/systemd/system/elderly-pipeline.service.d
sudo cp "${SYSTEMD_SRC}/elderly-pipeline.service.d/"*.conf \
        /etc/systemd/system/elderly-pipeline.service.d/

# ── noban-watchdog.sh — the REAL self-heal watchdog pulled off the live Pi
#    (system-config/noban-watchdog.sh), called by noban-watchdog.service every
#    60s: (A) restarts the call-daemon on sustained "failed to open mic" (stuck
#    call holding the mic), (B) reboots once (capped at 2) if the pipeline never
#    reaches "Listening for wake word" (i2s wedge).
sudo install -m 755 "${SRC_DIR}/system-config/noban-watchdog.sh" \
    /usr/local/bin/noban-watchdog.sh
ok "installed /usr/local/bin/noban-watchdog.sh"

# Wi-Fi provisioning helpers (from setup.sh) onto PATH + passwordless nmcli.
sudo install -m 755 "${NOBAN_DIR}/wifi_setup.sh"        /usr/local/bin/wifi_setup.sh
sudo install -m 755 "${NOBAN_DIR}/wifi_setup_server.py" /usr/local/bin/wifi_setup_server.py
SUDOERS_LINE="pi ALL=(root) NOPASSWD: /usr/bin/nmcli"
if ! sudo grep -qsxF "$SUDOERS_LINE" /etc/sudoers.d/wifi-setup 2>/dev/null; then
    echo "$SUDOERS_LINE" | sudo tee /etc/sudoers.d/wifi-setup >/dev/null
    sudo chmod 440 /etc/sudoers.d/wifi-setup
fi
# run_camera.sh does `sudo chmod 666 /dev/video9` — allow it without a password.
CAM_SUDO="pi ALL=(root) NOPASSWD: /usr/bin/chmod 666 /dev/video9"
if ! sudo grep -qsxF "$CAM_SUDO" /etc/sudoers.d/noban-camera 2>/dev/null; then
    echo "$CAM_SUDO" | sudo tee /etc/sudoers.d/noban-camera >/dev/null
    sudo chmod 440 /etc/sudoers.d/noban-camera
fi

sudo systemctl daemon-reload
sudo systemctl enable elderly-camera.service elderly-pipeline.service \
                      elderly-call-daemon.service noban-watchdog.timer
ok "units installed + enabled"

# =============================================================================
# 6. POINT THE DEVICE AT THE SERVER
#    main.cpp reads SERVER_URL / MQTT_HOST / MQTT_PORT / CAMERA_DEV from the
#    environment (systemd sets them). The shipped units already carry these as
#    Environment= lines using noban.local; we rewrite them to the configured
#    values via systemd drop-ins so the units themselves stay pristine and
#    re-runs are clean.
# =============================================================================
say "[6/8] point device at server (${SERVER_URL} / ${MQTT_HOST}:${MQTT_PORT})"
sudo mkdir -p /etc/systemd/system/elderly-pipeline.service.d \
              /etc/systemd/system/elderly-call-daemon.service.d
sudo tee /etc/systemd/system/elderly-pipeline.service.d/zz-server.conf >/dev/null <<EOF
[Service]
Environment=SERVER_URL=${SERVER_URL}
Environment=MQTT_HOST=${MQTT_HOST}
Environment=MQTT_PORT=${MQTT_PORT}
Environment=CAMERA_DEV=${CAMERA_DEV}
EOF
sudo tee /etc/systemd/system/elderly-call-daemon.service.d/zz-server.conf >/dev/null <<EOF
[Service]
Environment=SERVER_URL=${SERVER_URL}
EOF
sudo systemctl daemon-reload
ok "server/MQTT env written via drop-ins"

# ── Device identity (optional) ───────────────────────────────────────────────
# Written only if DEVICE_ID + DEVICE_PASSWORD given; otherwise the device boots
# UNPROVISIONED and waits for BLE/app onboarding to write this file.
if [ -n "$DEVICE_ID" ] && [ -n "$DEVICE_PASSWORD" ]; then
    sudo mkdir -p /var/lib/elderly-care
    printf '%s\n%s\n' "$DEVICE_ID" "$DEVICE_PASSWORD" \
        | sudo tee /var/lib/elderly-care/id.txt >/dev/null
    sudo chmod 600 /var/lib/elderly-care/id.txt
    sudo chown pi:pi /var/lib/elderly-care/id.txt
    ok "wrote /var/lib/elderly-care/id.txt (device_id=${DEVICE_ID})"
else
    echo "    note: no DEVICE_ID/DEVICE_PASSWORD — device boots UNPROVISIONED (BLE/app onboarding)"
fi

# =============================================================================
# 7. FINAL NOTES
# =============================================================================
say "[7/8] done"
cat <<EOF

Provisioning complete.

  Bundle:     ${NOBAN_DIR}
  Binary:     ${NOBAN_DIR}/build/pipeline
  Server:     ${SERVER_URL}
  MQTT:       ${MQTT_HOST}:${MQTT_PORT}
  Camera:     ${CAMERA_DEV}
  Identity:   $( [ -n "$DEVICE_ID" ] && echo "${DEVICE_ID} (id.txt written)" || echo "UNPROVISIONED — BLE/app onboarding" )

NEXT:
  1. REBOOT so the i2s/i2c overlays + v4l2loopback take effect:
         sudo reboot
  2. After reboot, the device is systemd-managed (never launch ./build/pipeline
     by hand — a duplicate instance steals the mic/GPIO):
         sudo systemctl status elderly-pipeline elderly-camera elderly-call-daemon
  3. Watch it reach the wake-word loop + heartbeat the server:
         journalctl -u elderly-pipeline -f          # expect "Listening for wake word"
         journalctl -u elderly-pipeline | grep mqtt # expect "[mqtt] client started -> tcp://${MQTT_HOST}:${MQTT_PORT}"
  4. Verify audio hardware if WUW never fires:
         arecord -l                                  # expect card 'sndrpigooglevoi'
         aplay -D plughw:CARD=sndrpigooglevoi,DEV=0 /usr/share/sounds/alsa/Front_Center.wav
  5. Verify camera feed:
         ls -l ${CAMERA_DEV} ; journalctl -u elderly-camera -f

EOF
