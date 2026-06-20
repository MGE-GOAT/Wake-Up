# Laptop side of the elderly-care system (simulating the Pi)

This is the **edge device** half of the project, extracted from the PC for
testing on a laptop that has a webcam + microphone instead of a real Pi.

## Architecture

```
PC                                  Laptop (you are here)
─────────────────                    ─────────────────────────
Flutter app (emulator)               C++ pipeline binary
Python server (port 5000)  <─HTTP─   (webcam + mic + uploads)
```

You're the device. You talk to the PC's server over LAN.

## First-time setup

1. Run `./setup.sh` — installs apt deps (OpenCV, ALSA, etc.) and builds
   the bundled TFLite/ONNX/KFR libraries from source. **~20-30 min** the
   first time.
2. Edit `main.cpp` — change `SERVER_BASE_URL` to your PC's LAN IP, e.g.
   `http://192.168.1.50:5000`. Find your PC's IP with `ip a` on the PC.
3. `mkdir build && cd build && cmake .. && make -j$(nproc) && cd ..`
4. `./dev_run.sh` — starts the pipeline. The script also tries to start
   a server but you can pass `SERVER_PID=skip` since the server runs on
   the PC, not here. (Or just run `./pipeline` directly.)

## What the binary does on a laptop (vs Pi)

| Thing | On Pi | On laptop |
|---|---|---|
| Mic | I2S INMP441 (set `USE_S32_CAPTURE=1` at compile) | Built-in mic, S16, "default" ALSA |
| Camera | Pi Camera via libcamera-v4l2 | Webcam at `/dev/video0` |
| Button | GPIO BCM 24 | Send `kill -USR1 $(pgrep -f pipeline)` |
| Wi-Fi setup | Real AP via `nmcli` | Simulated 5 s wait (doesn't touch your laptop's Wi-Fi) |
| LED | GPIO RGB | Print-only stub |

These switches are auto-detected at runtime — same binary works on both.

## Onboarding doc

For a fresh Claude Code session, run:

    https://claude.ai/claude-code/onboard/1r9vI3TQW3zr

(Drop that link in the session's first message.)

## Testing playbook

See TESTING.md in this directory.

## Offline parity / smoke tests

These don't need the camera or mic — they replay fixture files through the
pipeline so you can sanity-check the build before plugging anything in.

- **Fall pipeline on a recorded clip** (verifies the 6-stage fall detector
  end-to-end and dumps `/tmp/fall_cpp.json` with per-batch state):

      ./pipeline --fall-video /path/to/clip.mp4 --fall-loops 3

  Loops the file N times so you exceed the `batch_number >= 7` threshold
  that gates the MLP confirm stage. Compare against the matching Python
  reference in `parity_test/` if you want bit-for-bit verification.

## Build notes

- TFLite is pinned to **r2.13** via `setup.sh`. The wake-word model was
  trained on TF 2.13; newer kernels drift int8 outputs and can flip class
  predictions on borderline samples. Do not bump the branch without
  retraining and re-validating parity.
- The build links a single `libtensorflow-lite.a` (the XNNPACK delegate is
  baked into core as of r2.13 — no separate delegate static lib).
- `setup.sh` also handles the Iran-network workarounds: pre-fetching
  `fft2d`, `neon2sse`, and `psimd` from GitHub instead of the
  `storage.googleapis.com/mirror.tensorflow.org` mirror, which returns 403
  from this region.
