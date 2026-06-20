# Noban — Device

On-device firmware for the Noban elderly-care unit: a **Raspberry Pi** C++ pipeline that
runs always-on, fully offline edge AI and talks to the Noban server over HTTP + MQTT.

What the device does:

- **Wake-word detection** — always-on detection of the Persian wake word **"Noban"**
  (`noban_int8.tflite`). On fire it records ~5 s and posts it to the server's
  `/Wake_Command` for STT + intent (PILL / CALL / MESSAGE / SLEEP / WAKE).
- **Fall detection + pain assessment** — a camera pipeline (MoVeNet + SVM + MLP) detects
  falls, then runs a post-fall pain cascade (YAMNet scream/speech router + a log-mel
  specialist) and sends one encrypted A/V fall event stamped with a danger color.
- **Calls** — two-way audio+video to the caregiver app via the server's **Janus SFU**
  (`wake_call_helper.py`, warm daemon).
- **Pills / voice / snapshots** — plays pill-reminder and caregiver voice audio on the amp,
  serves camera snapshots on request, all driven off the `/Live_Device` heartbeat.
- **Sleep / guest mode** — a privacy mode that pauses fall+camera (see
  [Sleep mode](#sleep--guest-mode)).
- **UX** — OLED brand splash, RGB status LED, and a single physical button (gestures).

Runs as systemd services (`elderly-pipeline`, `elderly-camera`, `elderly-call-daemon`,
`noban-watchdog`).

---

## Repository layout

| Path | What it is |
|------|------------|
| `Noban/` | The pipeline source (`main.cpp`, `pain_pipeline.*`, `fall_detection.*`, `mqtt_client.*`, `gpio_io.*`, `media_crypto.*`, `aec_wrap.cpp`), the Python sidecars (`wake_call_helper.py`, `ble_provisioning.py`, `wifi_setup_server.py`, `show_noban.py`, `record_samples.py`), `CMakeLists.txt`, `run_camera.sh`, and the on-device models. |
| `Noban/models/` | The bundled models — **no download step** (STT/LLM live on the server). |
| `system-config/` | The real device `/etc` + `/boot` files: `config.txt`, `asound.conf`, the two `v4l2loopback` configs, and `noban-watchdog.sh`. |
| `systemd/` | The service units: `elderly-pipeline`, `elderly-camera`, `elderly-call-daemon`, `noban-watchdog.{service,timer}`, and the `oled.conf` drop-in. |
| `setup_new_device.sh` | **One-shot provisioner** — fresh Pi OS → fully running device. |
| `docs/` | `SETUP.md`, `ARCHITECTURE.md`. |

### On-device models

| File | Role |
|------|------|
| `models/noban_int8.tflite` | **Primary** "Noban" wake-word model (DS-CNN, int8). |
| `models/hey_int8.tflite` | "Hey" pre-gate model — the second gate for the double-gated wake in sleep mode. |
| `models/yamnet.tflite` | Pain router (scream vs speech, 521-class AudioSet). |
| `models/pain_logmel5.tflite` | Pain speech specialist (int8 log-mel). |
| `models/silero_vad.onnx` | VAD helper (off the live wake path). |
| `4.tflite` / `SVM_rbf.onnx` / `Last_stage_Model.onnx` | Fall detection: MoVeNet keypoints / SVM / MLP confirmer. |

TFLite is pinned to **r2.13** (newer kernels drift int8 outputs and flip predictions).

---

## From-scratch setup (`setup_new_device.sh`)

Takes a fresh **Raspberry Pi OS (Trixie, 64-bit / arm64)** install on a Pi 4 or 5 all the
way to a running device. It is **idempotent** — safe to re-run.

```bash
git clone https://github.com/rayanobinorg/Device.git && cd Device
```

### 1. Edit the config block at the top of `setup_new_device.sh`

```bash
SERVER_HOST="noban.local"                 # server hostname or IP (no mDNS → use an IP)
SERVER_URL="http://${SERVER_HOST}:5000"   # must include scheme + :5000
MQTT_HOST="${SERVER_HOST}"                # broker host (default = server)
MQTT_PORT="1883"
CAMERA_DEV="/dev/video9"                  # v4l2loopback device fed by run_camera.sh

# Identity — leave BOTH empty to onboard via the app over BLE (recommended),
# or set both to write /var/lib/elderly-care/id.txt now:
DEVICE_ID=""                              # e.g. ID_12233945
DEVICE_PASSWORD=""                        # 32-char shared secret (matches server key_hash)
```

> These override the values compiled into `main.cpp`; systemd passes `SERVER_URL` /
> `MQTT_HOST` / `MQTT_PORT` / `CAMERA_DEV` as environment. If you leave the identity empty,
> the device boots **unprovisioned** as `Noban` and runs **only** BLE provisioning until
> the app writes `id.txt` — that is the normal onboarding flow.

### 2. Run it (as the `pi` user, with sudo)

```bash
sudo ./setup_new_device.sh
sudo reboot
```

### What the script installs / configures

- **apt dependencies** — build tools + CMake, OpenCV, libcurl, OpenSSL, ALSA,
  `libgpiod-dev` (button/LED on Trixie), `libsodium-dev` (E2E media crypto),
  `paho-mqtt-cpp`, NetworkManager, ffmpeg + rpicam (camera), bluez (BLE), Python deps.
- **Boot / audio config** (from `system-config/`) — `config.txt`
  (`dtoverlay=googlevoicehat-soundcard` for the INMP441 mic + MAX98357A amp,
  `dtparam=i2c_arm=on` for the OLED), `asound.conf` (the `micleft` pure-LEFT mono @16 kHz
  capture path the mic scaling is calibrated for), and the two `v4l2loopback` configs so
  the camera surfaces at `/dev/video9`.
- **Native deps built/fetched from source** (~1.5 GB, **not** in the repo) — ONNX Runtime
  (aarch64), TensorFlow Lite r2.13, RNNoise, pocketfft.
- **The pipeline** — CMake build of `Noban/build/pipeline` into the bundle at
  `/home/pi/device_bundle/Noban` (the `WorkingDirectory` the units expect).
- **systemd units** — installs and enables the four services + the watchdog timer.

---

## Sleep / guest mode

A privacy / do-not-disturb mode, entered three ways:

- **App moon button** — the caregiver app toggles it via the server (`guest_set` on the
  heartbeat).
- **Device button: 3 rapid taps** to sleep; **1 tap** to wake.
- **Voice** — say «غیرفعالش کن» (SLEEP) / «فعالش کن» (WAKE); the server's wake intent
  classifier maps these to SLEEP/WAKE.

In sleep mode:

- **Fall detection + camera + snapshots are paused** (privacy). The app shows a "not
  monitoring" banner so a caregiver is never misled.
- **Pills, calls, and voice keep working** — they still play / connect.
- **Wake-word becomes double-gated** — a raw "Noban" hit must also clear the "Hey" gate
  (`hey_int8.tflite`), so the device only acts on a deliberate **"Hey Noban"** and won't
  fire from background chatter.
- **LED:** RED while idle (asleep), BLUE while recording a command (vs GREEN idle / RED
  recording in normal mode).

Sleep is **RAM-only** — a reboot comes back **ACTIVE**, so a power blip can't silently
leave the elder unmonitored.

### Other button gestures
1 tap (while asleep) → wake · 3 taps → sleep/guest toggle · 5 taps → factory reset
(confirms purge with the server first) · hold 3 s → BLE/Wi-Fi setup · hold 10 s → reboot.

---

## Operations

### NEVER manual-launch the pipeline
The device is **systemd-managed** and the pipeline is the **sole owner** of the mic
(`micleft`) and the GPIO/LED lines. Running `./build/pipeline` by hand while the service is
up spawns a **second instance that steals the mic + GPIO** — WUW and the button silently
break. Always go through systemd:

```bash
sudo systemctl restart elderly-pipeline      # deploy/restart the pipeline
```

After editing source you **must rebuild and restart** (a stale binary is the classic "WUW
broken" cause):

```bash
cd /home/pi/device_bundle/Noban && (cd build && make -j"$(nproc)") \
  && sudo systemctl restart elderly-pipeline
```

To kill a stray manual instance without touching the service, use the bracket trick:
```bash
pkill -f '[b]uild/pipeline'
```

### Logs
```bash
journalctl -u elderly-pipeline -f      # live pipeline log (healthy boot ends with "👂 Listening for wake word...")
journalctl -u elderly-camera -e        # camera feeder (/dev/video9)
journalctl -u elderly-call-daemon -e   # warm call daemon
```

### Clean shutdown — REQUIRED
An unclean power-off can wedge the i2s subsystem (it has killed a demo). Always shut down
cleanly and wait for the green activity LED to go dark before unplugging:

```bash
ssh pi@<device-ip> sudo shutdown -h now
```
systemd sends SIGTERM on stop; the binary handles it (clears the run flag so all threads
join and ALSA/GPIO release cleanly).

---

## Docs

- [`docs/SETUP.md`](docs/SETUP.md) — hardware wiring (mic/amp/camera/OLED/button/LED), boot
  config, native deps + build, models, systemd install, device identity & BLE provisioning,
  and operations.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — thread topology, the WUW and
  fall→pain pipelines, per-file responsibilities, the model table, systemd topology, and
  the MQTT/HTTP/call wire protocols.

---

## Related repos

- **Server** (backend): https://github.com/rayanobinorg/Server
- **App** (caregiver phone app): https://github.com/rayanobinorg/App
