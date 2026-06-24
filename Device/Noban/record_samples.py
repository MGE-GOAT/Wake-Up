#!/usr/bin/env python3
"""Noban sample recorder.

Records audio in the SAME format the wake-word pipeline reads from the mic
(`micleft`, 16 kHz, S32_LE, mono), with LED + OLED feedback.

Keys (single keypress, no Enter):
  n = signal       (general: stop+save current clip; wake: mark one utterance)
  w = switch mode  (general <-> wake)
  q = quit

GENERAL mode:
  Asks for a clip name -> OLED "recording <name>", LED red, records ->
  press n -> saves the wav up to that point -> asks for the next name -> repeat.

WAKE mode:
  For each of 5 speakers: records one continuous wav while you collect 20
  utterances of the wake word. Say the word, press n -> 1.4 s timer ->
  OLED shows "continue" for the next one. After 20, the speaker's wav is
  saved, a beep plays, and it advances to the next speaker.

IMPORTANT: the wake-word pipeline holds the mic + GPIO, so this tool stops
`elderly-pipeline` on start and restarts it on exit. Run it over SSH:
    python3 record_samples.py
Samples land in ~/samples/.
"""
import os
import sys
import time
import select
import termios
import tty
import atexit
import signal
import subprocess

# ── config ──────────────────────────────────────────────────────────────────
OUT_DIR       = os.path.expanduser(os.environ.get("SAMPLE_DIR", "~/samples"))
RATE          = int(os.environ.get("SAMPLE_RATE", "16000"))
FMT           = os.environ.get("SAMPLE_FMT", "S32_LE")
CH            = 1
MIC_DEV       = os.environ.get("MIC_DEVICE", "micleft")
AMP_DEV       = os.environ.get("SPEAKER_DEV", "plughw:CARD=sndrpigooglevoi,DEV=0")
WAKE_PER_SPK  = int(os.environ.get("WAKE_SAMPLES", "20"))
WAKE_SPEAKERS = int(os.environ.get("WAKE_SPEAKERS", "5"))
WAKE_GAP_S    = float(os.environ.get("WAKE_GAP", "1.4"))
PIPELINE_SVC  = "elderly-pipeline"

os.makedirs(OUT_DIR, exist_ok=True)


# ── LED (lgpio; falls back to no-op) ─────────────────────────────────────────
try:
    import lgpio
    _gpio_h = lgpio.gpiochip_open(0)
    for _p in (7, 1, 12):
        lgpio.gpio_claim_output(_gpio_h, _p, 0)

    def led(r=0, g=0, b=0):
        lgpio.gpio_write(_gpio_h, 7, 1 if r else 0)
        lgpio.gpio_write(_gpio_h, 1, 1 if g else 0)
        lgpio.gpio_write(_gpio_h, 12, 1 if b else 0)

    def _led_close():
        try:
            led(0, 0, 0)
            lgpio.gpiochip_close(_gpio_h)
        except Exception:
            pass
    atexit.register(_led_close)
    print("[rec] LED ready (BCM 7/1/12)")
except Exception as e:
    print(f"[rec] LED unavailable ({e}) — continuing without LED")

    def led(r=0, g=0, b=0):
        pass


# ── OLED (raw SSD1306 over smbus2 + PIL; falls back to terminal) ─────────────
class _Oled:
    def __init__(self, addr=0x3C, bus=1, w=128, h=64, driver="ssd1306"):
        from smbus2 import SMBus
        from PIL import Image, ImageDraw, ImageFont
        self.addr, self.w, self.h, self.driver = addr, w, h, driver
        self.col_offset = 2 if driver == "sh1106" else 0  # SH1106 has 132 cols
        self.bus = SMBus(bus)
        self._Image, self._Draw = Image, ImageDraw
        fsize = 10 if h <= 32 else 14
        self.dy = 11 if h <= 32 else 16   # line spacing → 3 lines fit on 32px
        try:
            self.font = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", fsize)
        except Exception:
            self.font = ImageFont.load_default()
        self._init_panel()

    def _cmd(self, *cs):
        for c in cs:
            self.bus.write_byte_data(self.addr, 0x00, c)

    def _init_panel(self):
        if self.driver == "sh1106":
            self._cmd(0xAE, 0xD5, 0x80, 0xA8, self.h - 1, 0xD3, 0x00, 0x40,
                      0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F,
                      0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF)
        else:  # ssd1306
            self._cmd(0xAE, 0xD5, 0x80, 0xA8, self.h - 1, 0xD3, 0x00, 0x40,
                      0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
                      0xDA, 0x12 if self.h == 64 else 0x02, 0x81, 0x7F,
                      0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF)

    def show(self, lines):
        img = self._Image.new("1", (self.w, self.h))
        d = self._Draw.Draw(img)
        y = 0
        for ln in lines:
            d.text((0, y), str(ln), font=self.font, fill=255)
            y += self.dy
        px = img.load()
        buf = []
        for page in range(self.h // 8):
            for x in range(self.w):
                byte = 0
                for bit in range(8):
                    if px[x, page * 8 + bit]:
                        byte |= (1 << bit)
                buf.append(byte)
        if self.driver == "sh1106":
            # SH1106: page addressing, per-page column reset (+offset), no 0x20 mode
            off = self.col_offset
            for page in range(self.h // 8):
                self._cmd(0xB0 + page, 0x00 | (off & 0x0F), 0x10 | (off >> 4))
                row = buf[page * self.w:(page + 1) * self.w]
                for i in range(0, len(row), 32):
                    self.bus.write_i2c_block_data(self.addr, 0x40, row[i:i + 32])
        else:  # ssd1306 horizontal addressing
            self._cmd(0x21, 0, self.w - 1, 0x22, 0, self.h // 8 - 1)
            for i in range(0, len(buf), 32):
                self.bus.write_i2c_block_data(self.addr, 0x40, buf[i:i + 32])


def _make_oled(driver=None, h=None):
    return _Oled(addr=int(os.environ.get("OLED_ADDR", "0x3C"), 16),
                 bus=int(os.environ.get("OLED_BUS", "1")),
                 h=int(h if h is not None else os.environ.get("OLED_HEIGHT", "32")),
                 driver=driver or os.environ.get("OLED_DRIVER", "sh1106"))


_oled = None
try:
    _oled = _make_oled()
    print(f"[rec] OLED ready ({_oled.driver} {_oled.w}x{_oled.h})")
except Exception as e:
    print(f"[rec] OLED unavailable ({e}) — status shown in terminal only")


def probe_oled():
    """Cycle through panel variants so we can see which renders correctly."""
    variants = [("ssd1306", 64), ("ssd1306", 32), ("sh1106", 64), ("sh1106", 32)]
    for i, (drv, h) in enumerate(variants, 1):
        print(f"\n[probe] {i}/{len(variants)}: {drv} 128x{h} — watch the panel for 7s")
        try:
            o = _make_oled(driver=drv, h=h)
            o.show([f"#{i} {drv}", f"128x{h}", "readable?"])
        except Exception as e:
            print(f"[probe] {drv} {h} failed: {e}")
        time.sleep(7)
    print("\n[probe] done — tell me which number (#1-4) showed clear text.")


def show(*lines):
    print("  [OLED] " + " | ".join(str(x) for x in lines))
    if _oled is not None:
        try:
            _oled.show(list(lines))
        except Exception as e:
            print(f"  [OLED write failed: {e}]")


# ── beep (next-speaker cue) ──────────────────────────────────────────────────
def beep():
    try:
        # `with` so the wav file descriptor is closed even if ffmpeg raises
        # (the previous bare open() leaked an fd on every exception).
        with open("/tmp/_noban_beep.wav", "wb") as f:
            subprocess.run(
                ["ffmpeg", "-loglevel", "quiet", "-f", "lavfi",
                 "-i", "sine=frequency=880:duration=0.25", "-ar", "48000",
                 "-ac", "2", "-f", "wav", "-"],
                stdout=f, check=False)
        subprocess.run(["aplay", "-q", "-D", AMP_DEV, "/tmp/_noban_beep.wav"],
                       check=False)
    except Exception as e:
        print(f"[rec] beep failed: {e}")


# ── ALSA recording (arecord; SIGINT finalizes the wav header) ────────────────
def start_rec(path):
    led(r=1)
    return subprocess.Popen(
        ["arecord", "-q", "-D", MIC_DEV, "-f", FMT, "-r", str(RATE),
         "-c", str(CH), "-t", "wav", path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_rec(proc):
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGINT)   # arecord finalizes the WAV on SIGINT
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    led(0, 0, 0)


# ── single-key keyboard (raw stdin) ──────────────────────────────────────────
_fd = sys.stdin.fileno()
try:
    _old_term = termios.tcgetattr(_fd)
    _HAS_TTY = True
except termios.error:
    _old_term = None          # no controlling terminal (e.g. non-interactive ssh)
    _HAS_TTY = False


def _restore_term():
    if _HAS_TTY:
        termios.tcsetattr(_fd, termios.TCSADRAIN, _old_term)


atexit.register(_restore_term)


def getkey(timeout=None):
    """Return one keypress (lowercased) or None on timeout."""
    tty.setcbreak(_fd)
    try:
        r, _, _ = select.select([sys.stdin], [], [], timeout)
        return sys.stdin.read(1).lower() if r else None
    finally:
        termios.tcsetattr(_fd, termios.TCSADRAIN, _old_term)


def ask_line(prompt):
    """Read a full line in normal (cooked) terminal mode."""
    termios.tcsetattr(_fd, termios.TCSADRAIN, _old_term)
    try:
        return input(prompt).strip()
    except (EOFError, KeyboardInterrupt):
        return ""


# ── pipeline ownership (it holds the mic + GPIO) ─────────────────────────────
def _sudo(*args):
    subprocess.run(["sudo", "-n", *args], check=False)


def stop_pipeline():
    print(f"[rec] stopping {PIPELINE_SVC} (frees mic + GPIO)…")
    _sudo("systemctl", "stop", PIPELINE_SVC)
    time.sleep(1.5)


def start_pipeline():
    print(f"[rec] restarting {PIPELINE_SVC}…")
    _sudo("systemctl", "start", PIPELINE_SVC)


# ── modes ────────────────────────────────────────────────────────────────────
def general_mode():
    """Returns 'wake' to switch, or 'quit'."""
    n = 0
    while True:
        name = ask_line("\nclip name (Enter = auto, 'w'=wake, 'q'=quit): ")
        if name.lower() == "q":
            return "quit"
        if name.lower() == "w":
            return "wake"
        if not name:
            n += 1
            name = f"clip_{int(time.time())}_{n}"
        if not name.endswith(".wav"):
            name += ".wav"
        path = os.path.join(OUT_DIR, name)
        print(f"[rec] recording → {path}   (press n to save)")
        proc = start_rec(path)
        t0 = time.time()
        last = -1
        while True:
            k = getkey(0.2)
            if k == "n":
                stop_rec(proc)
                show("saved", name[:14])
                print(f"[rec] saved {path}")
                break
            if k in ("w", "q"):
                stop_rec(proc)
                show("saved", name[:14])
                print(f"[rec] saved {path}")
                return "wake" if k == "w" else "quit"
            el = int(time.time() - t0)          # live elapsed timer, refreshed ~1/s
            if el != last:
                show("REC " + name[:10], f"{el // 60:02d}:{el % 60:02d}", "n=save")
                last = el


def wake_mode():
    """Returns 'general' to switch, or 'quit'."""
    show("WAKE MODE", f"{WAKE_SPEAKERS} speakers", f"{WAKE_PER_SPK} each")
    print(f"[rec] WAKE mode: {WAKE_SPEAKERS} speakers x {WAKE_PER_SPK} utterances")
    total = 0  # cumulative wake-word utterances across all speakers
    for spk in range(1, WAKE_SPEAKERS + 1):
        path = os.path.join(OUT_DIR, f"wake_speaker{spk}.wav")
        show(f"Speaker {spk}", "get ready…", f"0/{WAKE_PER_SPK}")
        print(f"\n[rec] Speaker {spk} → {path}")
        time.sleep(1.0)
        proc = start_rec(path)
        i = 0
        while i < WAKE_PER_SPK:
            show(f"Spk{spk}  {i}/{WAKE_PER_SPK}", "SAY IT + n", f"total {total}")
            k = getkey(0.2)
            if k is None:
                continue
            if k == "n":
                i += 1
                total += 1
                show(f"Spk{spk}  {i}/{WAKE_PER_SPK}", "wait...", f"total {total}")
                time.sleep(WAKE_GAP_S)
                show(f"Spk{spk}  {i}/{WAKE_PER_SPK}", "CONTINUE", f"total {total}")
            elif k == "q":
                stop_rec(proc)
                return "quit"
            elif k == "w":
                stop_rec(proc)
                return "general"
        stop_rec(proc)
        print(f"[rec] saved {path}")
        show(f"Speaker {spk}", "DONE", "next →")
        beep()
        time.sleep(0.5)
    show("WAKE DONE", f"{WAKE_SPEAKERS} speakers")
    print("[rec] all speakers done.")
    return "general"


def main():
    if not _HAS_TTY:
        print("[rec] ERROR: needs an interactive terminal. Run it directly over "
              "SSH (ssh pi@pi.local, then `python3 record_samples.py`), not piped.")
        return
    print(f"[rec] samples → {OUT_DIR}")
    print(f"[rec] format: {MIC_DEV} {FMT} {RATE}Hz mono (wake-word format)")
    stop_pipeline()
    atexit.register(start_pipeline)
    show("Noban recorder", "n=signal", "w=mode q=quit")
    mode = "general"
    try:
        while True:
            mode = wake_mode() if mode == "wake" else general_mode()
            if mode == "quit":
                break
    finally:
        led(0, 0, 0)
        show("bye")
    print("[rec] done.")


if __name__ == "__main__":
    try:
        if "--probe" in sys.argv:
            probe_oled()
        else:
            main()
    except KeyboardInterrupt:
        print("\n[rec] interrupted.")
