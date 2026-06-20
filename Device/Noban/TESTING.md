# Laptop testing — no Raspberry Pi required

This project has three components. Each can be tested in isolation on a
laptop; the C++ Pi binary is the only one that needs special accommodation
because it normally talks to GPIO + a 4-core Pi-class CPU.

| Component | Laptop testable? | How |
|---|---|---|
| Flutter app | Fully | Emulator + laptop's server IP |
| Python server | Fully | `python myserver.py` |
| C++ Pi pipeline | Partial | Webcam + mic + signal-driven button stub |


## Tier 1 — app + server (zero C++ needed)

Cheapest test loop. Exercises the entire UI, event ingestion, FCM push, and
the Wi-Fi setup screens. **Doesn't** test the actual ML pipelines on device.

```bash
# 1. Start the server (logs to /tmp/dev_server.log; skips heavy model load)
WAKE_NO_AUTO_WARMUP=1 python3 /home/mahrad/storage/Data/App/Server/myserver.py

# 2. Boot the Android emulator and launch the Flutter app
#    (already built — emulator-5554)
adb -s emulator-5554 shell monkey -p com.example.elderly_care_app_2 \
    -c android.intent.category.LAUNCHER 1

# 3. Inject a test fall event from any terminal
./seed_event.sh

# Optionally include a real MP4 clip:
VIDEO=/path/to/clip.mp4 ./seed_event.sh

# Snapshot-only message (the "thumbnail refresh" flow):
TYPE=image-only ./seed_event.sh
```

You should see:
- The event appear in the device hub → "رویدادهای سقوط" with a red banner
- An FCM notification on the emulator (if Firebase is wired)
- The image preview in the card, video banner if you sent one

The Flutter app's `configs.dart` already targets `http://10.0.2.2:5000` (the
Android emulator's alias for the host machine), so no change needed.


## Tier 2 — Wi-Fi setup screen (UI only, no real AP)

Tests the three-step Flutter screen against a mock HTTP server that
pretends to be the Pi.

```bash
# Run a "fake Pi" on 192.168.4.1:8080 emulated as 127.0.0.1:8080
python3 -c "
from wifi_setup_server import Handler
from http.server import ThreadingHTTPServer
Handler.iface = 'lo'   # won't actually scan, returns canned data on errors
ThreadingHTTPServer(('127.0.0.1', 8080), Handler).serve_forever()
"
```

Then in the app, temporarily change `_kSetupBase` in
`lib/wifi_setup_page.dart` from `http://192.168.4.1:8080` to
`http://10.0.2.2:8080`, rebuild and tap "راه‌اندازی وای‌فای دستگاه جدید".

(For a real end-to-end test you need an actual Pi — bringing up an AP on
your laptop would disconnect you mid-test.)


## Tier 3 — C++ pipeline binary on the laptop

Exercises the full Pi pipeline (WUW + camera + fall detection + server
uploads) against your laptop's built-in mic and webcam.

### Prerequisites

1. **Build dependencies** (apt):
   ```bash
   sudo apt install libopencv-dev libasound2-dev cmake build-essential
   ```

2. **Bundled libs** in `deps/` need x86_64 binaries (TFLite + ONNX +
   KFR + RNNoise). If you built on a Pi previously, these are aarch64 and
   won't link. Easiest fix: blow away `deps/` and re-run `setup.sh` on the
   laptop — it will rebuild everything for x86_64. Takes ~15 minutes.

3. **Model files** in the run directory:
   - `4.tflite`, `SVM_rbf.onnx`, `Last_stage_Model.onnx`  (fall detection — from the rar)
   - Wake-word `.tflite` (matches `TFLITE_MODEL` constant in main.cpp)
   - `silero_vad.onnx`
   - YAMNet + pain specialist `.tflite` (from `Pain Detection/models_speech/`)

### Build + run

```bash
cd /home/mahrad/storage/Data/files/kali
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..
./dev_run.sh           # starts server + pipeline together
```

### Simulating a button press (no GPIO on laptop)

The button stub auto-detects: if `/sys/class/gpio` is unavailable, it
switches to signal mode. The pipeline prints its PID and the exact command
to use:

```bash
kill -USR1 $(pgrep -f pipeline)
```

This triggers `runWifiSetup()`. On a laptop with no `/usr/local/bin/wifi_setup.sh`
installed, the binary prints `[wifi-DEV] simulated successful setup (5 s)`
and sleeps — your laptop's actual Wi-Fi is never touched.

### Triggering a fall (without actually falling)

Stand in front of the webcam and squat / lie down quickly. The 16-frame
batch cadence is 1 sec, so it takes a moment to register. Watch the
pipeline output for `!!! doubtful !!!` then `!!! FALL CONFIRMED`.

You can also inject a fall externally with `seed_event.sh` — that bypasses
the C++ binary entirely but exercises the server + app side.

### Triggering wake-word

Speak the trained wake phrase into the laptop mic. Watch the EMA values
in the pipeline output (`w=… m=…`) and the `🔔 WAKE WORD DETECTED!` line.
The 5 s command recording happens immediately after.


## What you cannot test on a laptop

| Thing | Why | Workaround |
|---|---|---|
| Real Wi-Fi AP provisioning | Bringing up the laptop's wlan0 as AP would disconnect you | Test on a Pi |
| INMP441 I2S mic (`USE_S32_CAPTURE=1`) | Laptop has no I2S — use S16 default | Keep `USE_S32_CAPTURE=0` (the default) |
| Real ALSA xrun behavior under load | Laptop has much more CPU + buffer headroom than Pi | Profile on a Pi |
| End-to-end `/Wake_Command` STT+LLM | HuggingFace models are blocked from the Iran network | Download models on a different network or skip this branch |


## Quick smoke test order

If you have ~10 minutes to verify the build is healthy:

1. `./seed_event.sh` → fall event card appears in the app  (15 s)
2. `TYPE=image-only ./seed_event.sh` → snapshot in the app  (15 s)
3. `./dev_run.sh` → pipeline boots, EMAs print  (1 min once built)
4. `kill -USR1 $(pgrep -f pipeline)` → "[wifi-DEV] simulated…"  (10 s)
5. Speak wake phrase → wake fire  (variable)
6. Squat in front of webcam → "doubtful" / "FALL CONFIRMED"  (variable)

If 1–4 work, the app/server/binary integration is fine. 5–6 depend on
your models being accurate enough for the laptop's audio/visual conditions.
