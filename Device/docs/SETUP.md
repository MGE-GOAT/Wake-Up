# Noban Device — Setup & Operations

The Noban device is the edge half of the elderly-care system: a Raspberry Pi
running a C++ pipeline that does wake-word detection (WUW), camera-based fall
detection, post-fall pain assessment, plays pill reminders, and handles
WebRTC calls. It talks to the Noban server (`Data/Server`) over HTTP + MQTT and
relays calls through a Janus SFU.

This document covers wiring, OS/boot configuration, building the native binary,
installing the systemd services, the provisioning flow, and day-to-day
operations.

> Source of truth: everything here is grounded in the code under
> `Data/Device/Noban/` and `Data/Device/systemd/`. Where a fact lives only in
> code, the `file:line` reference is given. Items the repo does not contain
> (e.g. the exact `/etc/asound.conf`, the boot overlay file, the
> `noban-watchdog.sh` body) are flagged as **MUST PROVIDE ON DEVICE**.

---

## 0. ⚠ OS requirement — Raspberry Pi OS **Bookworm**, not Trixie

Flash **Raspberry Pi OS Bookworm (Debian 12), 64-bit / arm64** (last known-good:
`2025-05-13-raspios-bookworm-arm64`, Desktop). **Do not use Trixie (Debian 13).**

Trixie's **BlueZ 5.82** uses BLE **extended advertising** and rejects the device's
connectable provisioning advertisement (`Failed to add advertisement: Invalid Parameters
(0x0d)`), so the button→BLE/app onboarding (`ble_provisioning.py`) never advertises and a
fresh unit can't be onboarded. **Bookworm's BlueZ 5.66** uses legacy advertising and the same
code works. There is no config knob to force legacy advertising on 5.82; the bug is
acknowledged upstream (RPi `linux#7098`). Wake word / fall / calls / pills work on either OS —
only BLE onboarding is affected. (Root-caused + verified June 2026 on a Pi 4 / BCM4345C0.)

Recovery if a device is already on Trixie: register it server-side directly (insert into the
server's `devices` table with `key_hash = sha256(<32-char id.txt secret>)` + a `subscriptions`
row) instead of onboarding over BLE.

---

## 1. Hardware & wiring

| Peripheral | Part | Bus / pins | Notes |
|---|---|---|---|
| Microphone | INMP441 I2S MEMS | I2S (googlevoicehat overlay) | 24-bit, delivered as S32_LE; LEFT channel only (`micleft` ALSA device) |
| Speaker amp | MAX98357A I2S amp | I2S | Plays alarms, pill audio, voice notes, call far-end |
| Camera | Pi CSI camera | CSI → `/dev/video9` via v4l2loopback | Fed by `rpicam-vid` (`run_camera.sh`) |
| OLED | SH1106/SSD1306 128×32 | I2C `0x3C`, bus 1 | Shows "Noban" brand at startup (`show_noban.py`) |
| Button | momentary push button | GPIO BCM **17** (physical pin 11), pull-up to 3V3 | Active-low (pressed = line LOW) |
| RGB LED | common-cathode RGB | GPIO BCM **R=7, G=1, B=12** | Status indicator |

Pin facts verified in code:
- Button line + LED lines: `gpio_io.cpp:15-17` (`BTN_LINE=17`, `LED_R=7, LED_G=1, LED_B=12`, chip `/dev/gpiochip0`).
- Button BCM17 also referenced in `main.cpp:1532` (`BUTTON_BCM_PIN = 17`).
- OLED I2C `0x3C` / bus 1: `show_noban.py`, `record_samples.py`.

> Note: `LAPTOP_README.md` and some `main.cpp` comments mention BCM 24 for the
> button — that is stale. The live wiring is **BCM17** per `gpio_io.cpp`.

### LED color meaning (`main.cpp`, `setLed` / `idleColor` / `recColor` `main.cpp:1404-1405`)
The idle and record-active colors depend on the mode (`g_guest_mode`):

| State | Normal (awake) | Sleep / guest |
|---|---|---|
| Idle / listening for wake word | **GREEN** | **RED** |
| Recording a command (post-wake) | **RED** | **BLUE** |

- **RED** also marks a fall/pain session in progress.
- **BLUE** also marks an active call, or BLE/Wi-Fi setup mode in progress.
- Blue blink (6×) — factory reset aborted (server unreachable).

---

## 2. OS / boot configuration

The pipeline opens the I2S devices by **name**, not `default`, because the
`type asym` default fails to resolve its capture slave under systemd
(`main.cpp:65-73`):

- Capture device: `micleft` (`ALSA_CAPTURE_DEV`, `main.cpp:72`)
- Playback device: `plughw:CARD=sndrpigooglevoi,DEV=0` (`ALSA_PLAYBACK_DEV`, `main.cpp:73`)

You must configure the following on the device (**MUST PROVIDE ON DEVICE** — not
in this repo):

1. **`/boot/firmware/config.txt`** — enable the I2S audio overlay and I2C:
   ```ini
   # I2S MEMS mic (INMP441) + I2S amp (MAX98357A) via the googlevoicehat overlay
   dtoverlay=googlevoicehat-soundcard
   # OLED on I2C
   dtparam=i2c_arm=on
   ```
   (Overlay name confirmed by the card name `sndrpigooglevoi` used throughout
   the code; exact overlay file lives on the SD card, not in the repo.)

2. **`/etc/asound.conf`** — define `micleft` as the pure LEFT channel of the
   I2S card downmixed to mono @ 16 kHz, and route playback to the amp. The mic
   scaling in `main.cpp` (`MIC_SCALE = 1/2^27`, `main.cpp:271-278`) is
   calibrated for the **pure-LEFT** `micleft` path; a different downmix changes
   the level and breaks WUW.

3. **v4l2loopback** — `run_camera.sh` feeds the CSI camera through
   `/dev/video9`:
   ```bash
   rpicam-vid ... --codec yuv420 -o - | ffmpeg ... -f v4l2 ... /dev/video9
   ```
   The loopback module must be loaded at boot (`elderly-camera.service` runs
   `After=systemd-modules-load.service`). The pipeline reads the camera via
   `CAMERA_DEV=/dev/video9` (`elderly-pipeline.service`).

### Mic capture format
`USE_S32_CAPTURE=1` is hard-set in `CMakeLists.txt:38`. The INMP441 delivers
24-bit samples inside S32_LE frames; opening S16 would destroy the upper bits.
`MIC_SCALE_POW` env overrides the default `1/2^27` for empirical sweeps
(`main.cpp:271-277`).

---

## 3. Building the binary

### Native dependencies
The CMake build (`CMakeLists.txt`) links these, all but the system libs built
from source into `Noban/deps/`:

| Dep | Where | Role |
|---|---|---|
| **TensorFlow Lite r2.13** | `deps/tflite` (built from `deps/tensorflow-src`) | WUW DS-CNN, YAMNet, pain, MoVeNet models. **Pin to r2.13** — newer kernels drift int8 outputs and flip class predictions (`CMakeLists.txt:150-160`, `setup.sh:92-94`). |
| **ONNX Runtime 1.17.x** | `deps/onnxruntime` | Silero VAD, fall SVM + MLP (`CMakeLists.txt:223-226`) |
| **RNNoise** | system (apt/make install) | Optional denoise (`CMakeLists.txt:220-221`); not used on the hot wake path |
| **pocketfft** (header-only) | `deps/pocketfft` | Bit-exact STFT/YIN FFT vs numpy/scipy (`CMakeLists.txt:94-97`) |
| **OpenCV** (core/imgproc/imgcodecs/videoio) | system | All fall-detection frame ops (`CMakeLists.txt:99-102`) |
| **libcurl** | system | Server HTTP (`CMakeLists.txt:108-111`) |
| **OpenSSL (libcrypto)** | system | SHA-256 of device password for `Key-Hash` (`CMakeLists.txt:115-117`) |
| **libgpiod v2** | system (`libgpiod-dev`) | Button + RGB LED on Pi OS Trixie (sysfs GPIO removed). Absent → button falls back to SIGUSR1, LED to print-only (`CMakeLists.txt:50-61`) |
| **libsodium** | system (`libsodium-dev`) | E2E media crypto. Absent → plaintext media + warning (`CMakeLists.txt:63-74`) |
| **paho-mqtt-cpp** | system (`libpaho-mqttpp-dev`) | MQTT transport. Absent → compiles as no-op stub, `HAVE_MQTT=0` (`CMakeLists.txt:76-92`) |
| **ALSA / pthread / dl / m** | system | Audio + runtime (`CMakeLists.txt:228-229`) |

### Setup script
`setup.sh` is the **x86_64 / Kali laptop** variant (`setup.sh:3`). It:
- apt-installs build deps, OpenCV, paho-mqtt, libsodium, NetworkManager
  (`setup.sh:14-33`), and `pip install bless` for BLE provisioning
  (`setup.sh:37-43`);
- builds RNNoise (`setup.sh:45-58`), clones pocketfft (`setup.sh:60-70`),
  downloads ONNX Runtime **x86_64** (`setup.sh:77-89`), builds TFLite r2.13
  (`setup.sh:91-161`) with Iran-network workarounds (pre-fetch fft2d / neon2sse
  / psimd from GitHub instead of the blocked Google mirror);
- installs `wifi_setup.sh` + `wifi_setup_server.py` to `/usr/local/bin` and
  grants passwordless `nmcli` sudo (`setup.sh:163-184`).

> **Pi vs laptop — use `setup_new_device.sh` on the Pi.** `setup.sh` targets the
> x86_64 Kali laptop (wrong ONNX Runtime arch for the Pi). For a real device,
> **`../setup_new_device.sh`** (repo root, a sibling to `Noban/`) is the
> **complete, from-scratch, idempotent** provisioner: it takes a fresh
> Raspberry Pi OS (Trixie, 64-bit / arm64) install on a Pi 4/5 all the way to a
> running device. It is the real setup path — the manual steps in this section
> are the underlying detail it automates. The script:
> - apt-installs all build + runtime deps (incl. `libgpiod-dev`, `libssl-dev`,
>   `libcurl4-openssl-dev`, `libflatbuffers-dev`, OpenCV, paho-mqtt, libsodium,
>   NetworkManager, ffmpeg/rpicam, bluez, alsa-utils, Python deps);
> - lays down the boot/audio config from `system-config/`
>   (`config.txt` overlays, `asound.conf` `micleft` path, the two v4l2loopback
>   configs) so the mic and `/dev/video9` come up correctly;
> - builds the native deps **from source for aarch64** (RNNoise, pocketfft,
>   ONNX Runtime `1.17.3`, TFLite `r2.13` — selecting the aarch64 ORT build, not
>   the x86_64 one `setup.sh` downloads);
> - CMake-builds the pipeline into `/home/pi/device_bundle/Noban` (the
>   `WorkingDirectory` the units hard-code);
> - installs and enables the four systemd units + the watchdog timer, and wires
>   the `SERVER_URL` / `MQTT_HOST` / `MQTT_PORT` / `CAMERA_DEV` env (config block
>   at the top of the script);
> - optionally writes `id.txt` if `DEVICE_ID`/`DEVICE_PASSWORD` are set, else
>   leaves the device unprovisioned for app/BLE onboarding.
>
> Run it as the `pi` user with sudo, then reboot. The manual steps below are
> useful for understanding or debugging individual stages.

### Compile
Models load by relative path from the working directory, so build under
`Noban/` and run from there:
```bash
cd Noban
./setup.sh                       # first time only (laptop); ~20–30 min
mkdir -p build && cd build
cmake ..
make -j"$(nproc)"
cd ..
```
The binary is `Noban/build/pipeline`.

Offline build/parity checks (no mic/camera needed):
```bash
./build/pipeline --wuw-wav capture.wav            # stream a WAV through the live WUW path
./build/pipeline --fall-video clip.mp4 --fall-loops 3   # fall detector → /tmp/fall_cpp.json
./build/pipeline --parity-fixtures parity_test/fixtures # tensor-dump parity (DUMP_TENSORS build)
```

---

## 4. Models

All device models ship **in the bundle** — there is no download step; STT/LLM
live on the server (`setup.sh:170-173`).

| File | Loaded as | Loaded at | Role |
|---|---|---|---|
| `models/noban_int8.tflite` | `TFLITE_MODEL` (`main.cpp:107`) | `WakeWordDetector::init` (`main.cpp:642`) | **Primary** "Noban" wake-word DS-CNN-QAT, int8, 3-class (wake/other/noise); fires in both modes |
| `models/hey_int8.tflite` | `HEY_MODEL` (`main.cpp:108`) | `hey_wwd.init` (`main.cpp:2613`) | "Hey" pre-gate DS-CNN, int8, 3-class; opens the sleep-mode double-gate window (see §7) |
| `models/silero_vad.onnx` | `SILERO_MODEL` (`main.cpp:109`) | `SileroVAD::init` (`main.cpp:713`) | Voice-activity trimming (helper; not on the live fire path) |
| `models/yamnet.tflite` | `YAMNET_MODEL` (`main.cpp:110`) | `YamnetRouter::init` (`pain_pipeline.cpp:83`) | Pain router: scream vs speech (521-class AudioSet) |
| `models/pain_logmel5.tflite` | `PAIN_MODEL` (`main.cpp:111`) | `SpeechSpecialist::init` (`pain_pipeline.cpp:119`) | Pain speech specialist, int8, sigmoid prob |
| `4.tflite` | CWD-relative | `FallDetection` ctor | MoVeNet keypoints, input `[1,192,192,3]` uint8 → `[1,1,17,3]` (`fall_detection.hpp:17`) |
| `SVM_rbf.onnx` | CWD-relative | `FallDetection` ctor | Fall stage-5 SVM, input `X[16,80]` (`fall_detection.hpp:15`) |
| `Last_stage_Model.onnx` | CWD-relative | `FallDetection` ctor | Fall MLP confirmer, input `[1,15,16,2]` (`fall_detection.hpp:16`) |

Run the binary from `Noban/` so the relative paths resolve.

---

## 5. systemd services

Four units + a watchdog timer live in `Data/Device/systemd/`. They reference an
installed bundle at `/home/pi/device_bundle/Noban`. Install with:
```bash
sudo cp systemd/*.service systemd/*.timer /etc/systemd/system/
sudo mkdir -p /etc/systemd/system/elderly-pipeline.service.d
sudo cp systemd/elderly-pipeline.service.d/oled.conf \
        /etc/systemd/system/elderly-pipeline.service.d/
sudo systemctl daemon-reload
sudo systemctl enable --now elderly-camera elderly-pipeline \
        elderly-call-daemon noban-watchdog.timer
```

| Unit | Purpose | Key settings |
|---|---|---|
| **elderly-camera.service** | CSI camera → `/dev/video9` loopback feeder (`run_camera.sh`) | `After=systemd-modules-load`; `Restart=always`; `StartLimitIntervalSec=0` |
| **elderly-pipeline.service** | The main C++ binary (WUW + fall + pain + heartbeat) | `After/Wants=elderly-camera`; env `SERVER_URL=http://noban.local:5000`, `MQTT_HOST=noban.local`, `CAMERA_DEV=/dev/video9`, `LD_LIBRARY_PATH=.../deps/onnxruntime/lib`; `ExecStartPre=/bin/sleep 8` (let i2s + camera settle); `Restart=always RestartSec=5`; `StartLimitIntervalSec=0`; `TimeoutStopSec=15`, `KillMode=mixed` (clean SIGTERM shutdown — see §8) |
| → **oled.conf** drop-in | `ExecStartPost` runs `show_noban.py` to display the brand | leading `-` = failure is non-fatal |
| **elderly-call-daemon.service** | Warm call daemon — `wake_call_helper.py --serve` pre-imports aiortc so calls start fast | env `AEC_MODE=off`, `GATE_THRESHOLD=200`, `GATE_HANGOVER=0.8`, `SERVER_URL`, `XDG_RUNTIME_DIR=/run/user/1000`; `Restart=always` |
| **noban-watchdog.service** + **.timer** | Self-heal: reboot once if the pipeline never reaches "Listening". `Type=oneshot` runs `/usr/local/bin/noban-watchdog.sh` | Timer: `OnBootSec=120`, `OnUnitActiveSec=60` |

> **MUST PROVIDE ON DEVICE:** `/usr/local/bin/noban-watchdog.sh` — referenced
> by `noban-watchdog.service:7` but **not present in this repo**. It is expected
> to grep `journalctl -u elderly-pipeline` for the "Listening for wake word"
> line and reboot once if absent past boot+120s.

Service topology: `elderly-camera` produces `/dev/video9`; `elderly-pipeline`
consumes it and owns the mic/GPIO/LED; `elderly-call-daemon` is a separate
process the pipeline shells into for calls (it needs the mic/camera released —
the pipeline does that via `g_in_call`).

---

## 6. Device identity & provisioning

### Identity file
The binary reads `id.txt` at startup (`loadDeviceIdentity`, `main.cpp:206`):
two lines — `device_id` then a 32-char shared `password`. Lookup order
(`main.cpp:207-220`):
1. `$ELDERLY_ID_FILE` (test override)
2. `/var/lib/elderly-care/id.txt` (Pi production)
3. `~/.config/elderly-care/id.txt` (laptop dev)

The SHA-256 of the password is sent as the `Key-Hash` header on every server
call (`main.cpp:180-190`). The stable wlan MAC is sent as `Device-Mac` so the
server can dedup a re-provisioned device (`readDeviceMac`, `main.cpp:168`).

If **no `id.txt`** exists the device boots **unprovisioned** as
`FACTORY_ID = "Noban"` (`main.cpp:150, 226`) and the `main()` guard refuses all
server endpoints — it runs **only** BLE provisioning until creds are written
(`main.cpp:2547-2558`).

### BLE provisioning (primary, Phase 7)
Triggered by a **3–10 s button hold** (`ButtonGesture::ENTER_SETUP`) or
automatically on first boot. `runWifiSetup` (`main.cpp:1775`) spawns
`ble_provisioning.py` (looked up at `/usr/local/bin/ble_provisioning.py` then
`./ble_provisioning.py`). The companion Flutter app (flutter_blue_plus):

- Connects to GATT service `11111111-2222-3333-4444-555555555555`
  (device advertises as `Noban-Setup-<serial>`).
- Reads **WIFI_SCAN** (`...5001`) → JSON list of `{ssid, signal, security}`
  (via `nmcli dev wifi list`).
- Writes **WIFI_JOIN** (`...5002`) `{ssid, psk}` → `nmcli connection add/up`.
- Reads/notifies **STATUS** (`...5003`).
- Writes **COMMIT** (`...5004`) `{device_id, password}` → writes `id.txt`
  atomically to `/var/lib/elderly-care/id.txt` (fallback
  `~/.config/elderly-care/id.txt`).
- Reads/notifies **DONE** (`...5005`).

On success `runWifiSetup` restarts `elderly-pipeline.service` so a fresh process
re-reads the new identity (`main.cpp:1815-1822`).

### Legacy AP provisioning (fallback)
If the BLE sidecar is missing, `runWifiSetup` falls back to `wifi_setup.sh` +
`wifi_setup_server.py` (`main.cpp:1795-1800`): the Pi raises an AP and serves
`GET /scan` + `POST /connect` (`{ssid,password}`) on port 8080; on connect it
touches a done-flag and exits.

### Pointing the device at the server
The build-time defaults are `SERVER_BASE_URL = http://192.168.1.104:5000`
(`main.cpp:126`) and MQTT host `192.168.1.104:1883` (`main.cpp:2568`). **The
`SERVER_URL` env (set by systemd) overrides the URL for all server comms**
(`main.cpp:2601`); production uses `http://noban.local:5000`. MQTT host/port are
overridable via `MQTT_HOST` / `MQTT_PORT` (`main.cpp:2565-2571`).

---

## 7. Operations

### Restarting safely — NEVER run the binary by hand
The device is **systemd-managed**. The pipeline is the **sole owner** of the
mic (`micleft`) and GPIO/LED. Launching `./build/pipeline` manually while the
service is up creates a **second instance that steals the mic and GPIO lines**
(gpiod line requests fail; capture races), which silently breaks WUW and the
button. Always deploy/restart via systemd:
```bash
sudo systemctl restart elderly-pipeline
```
After editing source you **must rebuild and restart** — a stale binary is the
classic "WUW broken" cause. Build, then:
```bash
cd Noban && (cd build && make -j"$(nproc)") && sudo systemctl restart elderly-pipeline
```

To kill a stray manual instance without touching the service, use the bracket
trick so `pkill` doesn't match its own command line:
```bash
pkill -f '[b]uild/pipeline'
```

### Logs
```bash
journalctl -u elderly-pipeline -f          # live pipeline log
journalctl -u elderly-camera -e            # camera feeder
journalctl -u elderly-call-daemon -e       # warm call daemon
```
Healthy boot shows model-load lines then `👂 Listening for wake word...`.
stdout is line-buffered under systemd so timestamps are accurate
(`main.cpp:2521`).

### Postgres / server dependency
The device needs the server reachable at `SERVER_URL`. The heartbeat logs
throttled failures if the server is down (`main.cpp:940-954`); the device keeps
running and self-heals when the server returns. (Server-side Postgres can need a
manual start after a reboot — see the server docs.)

### Guest / sleep (do-not-disturb)
Entered three ways: **3 rapid button taps** (sleep) / **1 tap** (wake)
(`g_guest_mode`, `main.cpp:163`; button handling `main.cpp:2051-2061`); the
**app moon button** via the heartbeat `guest_set` (`main.cpp:2094-2099`); or
**voice** — «غیرفعالش کن» (SLEEP) / «فعالش کن» (WAKE), routed by the server's
wake-intent classifier.

Sleep mode pauses **only fall detection + camera + snapshots** (privacy) — it is
**not** a full pause:
- **Pills, calls, and caregiver voice notes keep working** — they still play /
  connect in sleep (`main.cpp:2119, 2164, 2184`).
- **The wake word keeps running but is double-gated** — the elder must say
  **"Hey … Noban"**: a smoothed "Hey" hit (`hey_int8.tflite`, low EMA threshold
  ~0.16) opens an 8 s window, then the full strict "Noban" gate must fire inside
  it (`main.cpp:2891-2899`). Awake mode is a single strict "Noban" — unchanged.
  This suppresses false fires from background chatter while guests are over.
- **LED**: RED while idle (asleep) and BLUE while recording a command — vs GREEN
  idle / RED recording when awake (`idleColor`/`recColor`, `main.cpp:1404-1405`).

Sleep is **RAM-only** — a reboot comes back ACTIVE so a power blip can't leave
the elder unmonitored. The device reports its sleep state to the server
(`Guest-Mode` heartbeat header, `main.cpp:925`) so the app can show a "not
monitoring" banner.

### Button gestures (summary)
- 1 tap while asleep → wake.
- 3 taps → sleep / guest mode.
- 5 taps → factory reset (wipes `id.txt` + Wi-Fi; confirms purge with server
  first, aborts if unreachable — `doFactoryReset`, `main.cpp:1690`).
- Hold 3 s → BLE/Wi-Fi setup.
- Hold 10 s → reboot (a Pi has no soft power-on).

### Clean shutdown — REQUIRED
An unclean power-off can wedge the i2s subsystem (it has killed a demo).
**Always** shut the Pi down cleanly before unplugging and wait for the green
activity LED to go dark:
```bash
ssh pi@<device-ip> sudo shutdown -h now
```
systemd sends SIGTERM on stop; the binary handles it (`on_term`,
`main.cpp:1547`) by clearing the run flag so all threads join and ALSA/GPIO are
released cleanly (`TimeoutStopSec=15`, `KillMode=mixed`).

---

## 8. Wake-word retraining data capture
`record_samples.py` collects INMP441 samples in the pipeline's native format
(`micleft`, 16 kHz, S32_LE, mono) to `~/samples/`. It **stops
`elderly-pipeline` on start** (to free the mic + GPIO) and restarts it on exit —
the same "no second mic owner" rule. WAKE mode records 5 speakers × 20
utterances; GENERAL mode is free recording. (Background: WUW false triggers on
the INMP441 are a mic domain-shift problem; the fix is recording on the INMP441
and retraining.)
