#!/usr/bin/env python3
"""Interactive 'eyes' for the Noban OLED (SH1106 128x32).

A persistent daemon that renders animated robot eyes based on a tiny state file
the C++ pipeline writes at each transition (setOled() in main.cpp). Reuses the
proven raw-I2C SH1106 init/blit path from show_noban.py.

States (written by the pipeline to OLED_STATE_FILE):
    none     blank screen — any setup/maintenance: unprovisioned, BLE setup,
             factory reset, reboot, shutdown (no eyes at all)
    idle     calm open eyes, blink now and then (provisioned, listening)
    happy    heard its name "Noban" (normal wake) — happy, focusing
    listen   woke from sleep on "Hey Noban" (guest) — wide awake, listening
    sleep    guest/sleep mode — eyes closed, a little "z"
    worried  post-fall "speak now" recording — concerned brows

Env: OLED_STATE_FILE (default /tmp/noban_oled_state), OLED_ADDR (0x3C),
     OLED_BUS (1), OLED_WIDTH (128), OLED_HEIGHT (32), OLED_DRIVER (sh1106),
     OLED_FPS (15).

The screen is only re-blitted when the rendered frame actually changes, so a
static state costs no I2C traffic; only blinks/animation push bytes.
"""
import os
import signal
import sys
import time

from PIL import Image, ImageDraw

from show_noban import Oled

STATE_FILE = os.environ.get("OLED_STATE_FILE", "/tmp/noban_oled_state")
FPS = int(os.environ.get("OLED_FPS", "15"))

_running = True


def _read_state():
    try:
        with open(STATE_FILE) as f:
            return f.read().strip() or "none"
    except FileNotFoundError:
        return "none"
    except Exception:
        return "none"


class EyeRenderer:
    """Draws each state onto a 1-bit PIL image sized to the panel."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        # Two eyes, centred vertically, split left/right of centre.
        self.lx = w // 2 - w // 5     # ~38 on 128
        self.rx = w // 2 + w // 5     # ~90
        self.cy = h // 2              # 16
        # Eye half-extents (kept inside the 32px height).
        self.ew = max(8, w // 10)     # half-width ~12
        self.eh = max(6, h // 2 - 2)  # half-height ~14

    # ---- per-state drawers -------------------------------------------------
    def _open_eye(self, d, cx, half_w, half_h):
        d.rounded_rectangle(
            [cx - half_w, self.cy - half_h, cx + half_w, self.cy + half_h],
            radius=min(half_w, half_h) // 2 + 2, fill=255)

    def _blink_bar(self, d, cx):
        d.rounded_rectangle(
            [cx - self.ew, self.cy - 2, cx + self.ew, self.cy + 2],
            radius=2, fill=255)

    def idle(self, d, t):
        # Blink ~150ms every ~3.5s.
        blinking = (t % 3.5) < 0.15
        for cx in (self.lx, self.rx):
            if blinking:
                self._blink_bar(d, cx)
            else:
                self._open_eye(d, cx, self.ew, self.eh)

    def happy(self, d, t):
        # Upward smile-arcs (◡ ◡) — happy/squinting eyes.
        for cx in (self.lx, self.rx):
            box = [cx - self.ew, self.cy - self.eh,
                   cx + self.ew, self.cy + self.eh]
            d.arc(box, start=20, end=160, fill=255, width=4)

    def listen(self, d, t):
        # Wide-awake big eyes with a pupil that drifts a little (alert).
        drift = int(2 * __import__("math").sin(t * 3.0))
        r = min(self.ew + 1, self.eh)
        for cx in (self.lx, self.rx):
            d.ellipse([cx - r, self.cy - r, cx + r, self.cy + r], fill=255)
            # black pupil
            pr = max(2, r // 3)
            d.ellipse([cx - pr + drift, self.cy - pr,
                       cx + pr + drift, self.cy + pr], fill=0)

    def sleep(self, d, t):
        # Closed eyes = flat bars, plus a tiny rising "z".
        for cx in (self.lx, self.rx):
            d.rounded_rectangle(
                [cx - self.ew, self.cy - 2, cx + self.ew, self.cy + 2],
                radius=2, fill=255)
        # little "z" near top-right, bobs up/down slowly
        zy = 2 + int(2 * (1 + __import__("math").sin(t * 2.0)))
        d.text((self.w - 12, zy), "z", fill=255)

    def worried(self, d, t):
        # Smaller open eyes + worried brows (inner ends raised), tiny shiver.
        shake = int(1 * __import__("math").sin(t * 18.0))
        hw, hh = self.ew - 2, self.eh - 4
        for i, cx in enumerate((self.lx, self.rx)):
            cxx = cx + shake
            d.ellipse([cxx - hw, self.cy - hh + 3,
                       cxx + hw, self.cy + hh + 3], fill=255)
            # worried brow: inner end up, outer end down
            if i == 0:   # left eye: inner is on the right
                d.line([cxx - hw, self.cy - hh - 4,
                        cxx + hw, self.cy - hh - 9], fill=255, width=2)
            else:        # right eye: inner is on the left
                d.line([cxx - hw, self.cy - hh - 9,
                        cxx + hw, self.cy - hh - 4], fill=255, width=2)

    def render(self, state, t):
        img = Image.new("1", (self.w, self.h))
        if state == "none":
            return img   # blank
        d = ImageDraw.Draw(img)
        fn = getattr(self, state, None)
        if fn is None:
            fn = self.idle
        fn(d, t)
        return img


def _on_signal(signum, frame):   # noqa: ARG001
    global _running
    _running = False


def main():
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)

    oled = Oled(addr=int(os.environ.get("OLED_ADDR", "0x3C"), 16),
                bus=int(os.environ.get("OLED_BUS", "1")),
                w=int(os.environ.get("OLED_WIDTH", "128")),
                h=int(os.environ.get("OLED_HEIGHT", "32")),
                driver=os.environ.get("OLED_DRIVER", "sh1106"))
    eyes = EyeRenderer(oled.w, oled.h)

    period = 1.0 / max(1, FPS)
    start = time.monotonic()
    last_bytes = None
    blank = Image.new("1", (oled.w, oled.h))

    print(f"[oled-eyes] running ({oled.w}x{oled.h} {oled.driver}), "
          f"state file={STATE_FILE}", flush=True)

    while _running:
        state = _read_state()
        t = time.monotonic() - start
        img = eyes.render(state, t)
        b = img.tobytes()
        if b != last_bytes:        # only touch I2C when the frame changes
            oled._blit(img)
            last_bytes = b
        time.sleep(period)

    # Clear the panel on exit (clean shutdown / reboot → no eyes).
    try:
        oled._blit(blank)
    except Exception:
        pass
    print("[oled-eyes] stopped, screen cleared", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
