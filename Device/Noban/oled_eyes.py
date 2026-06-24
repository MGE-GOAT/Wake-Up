#!/usr/bin/env python3
"""Interactive 'eyes' for the Noban OLED (SH1106 128x32).

A persistent daemon that renders animated robot eyes based on a tiny state file
the C++ pipeline writes at each transition (setOled() in main.cpp). Reuses the
proven raw-I2C SH1106 init/blit path from show_noban.py.

Each eye is a white "sclera" with a black pupil + sparkle. The WHOLE eye drifts
around the screen (plus the pupil leads), so the eyes look alive. States:

    none     blank screen — any setup/maintenance: unprovisioned, BLE setup,
             factory reset, reboot, shutdown (no eyes at all)
    idle     calm — whole eyes wander slowly, occasional blink
    happy    warm closed-eye smile (◡ ◡) — used for greetings
    excited  big eyes, bounce, sparkles — heard its name "Noban"
    listen   woke from sleep on "Hey Noban" (guest) — curious side-to-side scan
    sleep    guest/sleep mode — closed lids + a little rising "z"
    worried  post-fall "speak now" — small eyes look down, worried brows, shiver
    greet      meta: happy for ~30s then idle (first-ever setup)
    wakegreet  meta: idle 2s, happy 10s, then idle (guest→active)

Env: OLED_STATE_FILE (default /tmp/noban_oled_state), OLED_EYE_DY (vertical
     nudge in px if the panel sits low/high; negative = up; default 0),
     OLED_FPS (default 18), OLED_ADDR/OLED_BUS/OLED_WIDTH/OLED_HEIGHT/OLED_DRIVER.
"""
import math
import os
import random
import signal
import sys
import time

from PIL import Image, ImageDraw

from show_noban import Oled

STATE_FILE = os.environ.get("OLED_STATE_FILE", "/tmp/noban_oled_state")
FPS = int(os.environ.get("OLED_FPS", "18"))
EYE_DY = int(os.environ.get("OLED_EYE_DY", "0"))

_running = True


def _read_state():
    try:
        with open(STATE_FILE) as f:
            return f.read().strip() or "none"
    except Exception:
        return "none"


class EyeRenderer:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.cy = h // 2 + EYE_DY            # vertical centre (+ optional nudge)
        gap = 44                             # distance between the two eyes
        self.lx = w // 2 - gap // 2
        self.rx = w // 2 + gap // 2
        self.ew = 12                         # eye half-width
        self.eh = 9                          # eye half-height (open)
        self.EX = 13                         # whole-eye horizontal travel
        self.EY = 3                          # whole-eye vertical travel (keep small: 32px tall)
        # wandering-gaze state (normalised -1..1), eased toward a moving target
        self.gx = self.gy = 0.0
        self.tx = self.ty = 0.0
        self.next_move = 0.0
        self.next_blink = 2.0
        self.blink_until = -1.0
        self.last_t = 0.0

    # ---- gaze + blink animation state -------------------------------------
    def _gaze(self, t, dwell):
        dt = max(0.0, min(0.1, t - self.last_t))
        self.last_t = t
        if t >= self.next_move:
            self.tx = random.uniform(-1.0, 1.0)
            self.ty = random.uniform(-1.0, 1.0)
            self.next_move = t + random.uniform(dwell * 0.5, dwell * 1.5)
        k = min(1.0, dt * 9.0)
        self.gx += (self.tx - self.gx) * k
        self.gy += (self.ty - self.gy) * k

    def _blinking(self, t):
        if self.blink_until < 0 and t >= self.next_blink:
            self.blink_until = t + 0.11
            self.next_blink = t + random.uniform(2.5, 5.5)
        if self.blink_until > 0 and t >= self.blink_until:
            self.blink_until = -1.0
        return self.blink_until > 0

    # ---- primitives -------------------------------------------------------
    def _eyeball(self, d, cx, cy, hw, hh, look_x, look_y, pr, sparkle=True):
        d.rounded_rectangle([cx - hw, cy - hh, cx + hw, cy + hh],
                            radius=min(hw, hh), fill=255)
        mx, my = hw - pr - 1, hh - pr - 1
        pcx = cx + int(round(look_x * mx))
        pcy = cy + int(round(look_y * my))
        d.ellipse([pcx - pr, pcy - pr, pcx + pr, pcy + pr], fill=0)
        if sparkle:
            sr = max(1, pr // 2)
            d.ellipse([pcx - pr, pcy - pr, pcx - pr + sr, pcy - pr + sr], fill=255)

    def _lid(self, d, cx, cy, hw):
        d.rounded_rectangle([cx - hw, cy - 2, cx + hw, cy + 2], radius=2, fill=255)

    # ---- states -----------------------------------------------------------
    def idle(self, d, t):
        self._gaze(t, dwell=1.6)
        blink = self._blinking(t)
        ex = self.gx * self.EX
        ey = self.gy * self.EY
        for cx in (self.lx, self.rx):
            ccx, ccy = cx + ex, self.cy + ey
            if blink:
                self._lid(d, ccx, ccy, self.ew)
            else:
                self._eyeball(d, ccx, ccy, self.ew, self.eh,
                              look_x=self.gx * 0.4, look_y=self.gy * 0.4, pr=4)

    def happy(self, d, t):
        # HAPPY: warm closed-eye smile arcs (◡ ◡) with a gentle bob.
        bob = int(round(1.5 * math.sin(t * 4.0)))
        r = self.ew
        for cx in (self.lx, self.rx):
            cy = self.cy + bob
            box = [cx - r, cy - 16, cx + r, cy + 5]
            d.arc(box, start=0, end=180, fill=255, width=4)   # ◡ upward smile

    def excited(self, d, t):
        # EXCITED: big eyes looking up, lively bounce, twinkling sparkles +
        # a shine dot. Used when it hears its name "Noban".
        bounce = int(round(2.0 * math.sin(t * 9.0)))
        for cx in (self.lx, self.rx):
            self._eyeball(d, cx, self.cy + bounce, self.ew + 1, self.eh + 1,
                          look_x=0.0, look_y=-0.6, pr=5, sparkle=False)
            d.ellipse([cx - 4, self.cy + bounce - 6, cx - 1, self.cy + bounce - 3],
                      fill=255)
        if int(t * 7) % 2 == 0:
            for sx in (self.lx - self.ew - 3, self.rx + self.ew + 2):
                sy = self.cy - 6
                d.line([sx - 2, sy, sx + 2, sy], fill=255)
                d.line([sx, sy - 2, sx, sy + 2], fill=255)

    def listen(self, d, t):
        # CURIOUS: whole eyes sweep smoothly side-to-side (searching for the
        # speaker), pupils lead the sweep. A calm, deliberate scan.
        sweep = math.sin(t * 1.9)
        ex = sweep * self.EX
        blink = self._blinking(t)
        for cx in (self.lx, self.rx):
            ccx = cx + ex
            if blink:
                self._lid(d, ccx, self.cy, self.ew)
            else:
                self._eyeball(d, ccx, self.cy, self.ew, self.eh,
                              look_x=sweep, look_y=-0.15, pr=4)

    def sleep(self, d, t):
        for cx in (self.lx, self.rx):
            self._lid(d, cx, self.cy, self.ew)
        zy = self.cy - 8 + int(2 * (1 + math.sin(t * 2.0)))
        d.text((self.w - 12, zy), "z", fill=255)

    def worried(self, d, t):
        # WORRIED: small downcast eyes (pupils low), a slight shiver, and
        # prominent sad/worried brows (inner ends raised). Brows are drawn just
        # above each eye (small offset) so they stay on-screen even after the
        # vertical nudge.
        shake = int(round(math.sin(t * 16.0)))
        hw, hh = self.ew - 1, self.eh - 2
        eyecy = self.cy + 2                       # eyes sit a touch low (downcast)
        top = eyecy - hh
        for i, cx in enumerate((self.lx, self.rx)):
            cxx = cx + shake
            self._eyeball(d, cxx, eyecy, hw, hh, look_x=0.0, look_y=0.9,
                          pr=3, sparkle=False)
            by_out, by_in = max(0, top - 1), max(0, top - 5)   # inner end raised
            if i == 0:        # left eye: inner edge is on the right
                d.line([cxx - hw, by_out, cxx + hw, by_in], fill=255, width=2)
            else:             # right eye: inner edge is on the left
                d.line([cxx - hw, by_in, cxx + hw, by_out], fill=255, width=2)

    def render(self, state, t):
        img = Image.new("1", (self.w, self.h))
        if state == "none":
            return img
        d = ImageDraw.Draw(img)
        getattr(self, state, self.idle)(d, t)
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

    print(f"[oled-eyes] running ({oled.w}x{oled.h} {oled.driver}, dy={EYE_DY}), "
          f"state file={STATE_FILE}", flush=True)

    cur_state = None
    state_since = 0.0
    while _running:
        raw = _read_state()
        t = time.monotonic() - start
        if raw != cur_state:           # remember when this state was first seen
            cur_state = raw
            state_since = t
        elapsed = t - state_since

        # Timed "greeting" meta-states resolve to a happy→idle sequence; a real
        # event (the pipeline writing a different state) interrupts them at once.
        state = raw
        if raw == "greet":             # first-ever setup: happy 30s, then idle
            state = "happy" if elapsed < 30.0 else "idle"
        elif raw == "wakegreet":       # guest→active: idle 2s, happy 10s, idle
            state = "idle" if elapsed < 2.0 else ("happy" if elapsed < 12.0 else "idle")

        try:
            img = eyes.render(state, t)
            b = img.tobytes()
            if b != last_bytes:
                oled._blit(img)        # I2C write — can transiently fail
                last_bytes = b
        except Exception as e:
            # A single I2C hiccup must NOT kill a daemon meant to run for weeks.
            # Log, drop the cached frame so the next good frame is re-blitted,
            # and keep looping.
            print(f"[oled-eyes] render/blit error (continuing): {e}", flush=True)
            last_bytes = None
        time.sleep(period)

    try:
        oled._blit(blank)
    except Exception:
        pass
    print("[oled-eyes] stopped, screen cleared", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
