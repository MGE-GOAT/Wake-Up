#!/usr/bin/env python3
"""Display a single word (default 'Noban') centered on the SH1106 128x32 OLED.

Run once — the panel holds the image in its own GDDRAM, so no refresh loop is
needed. Wired into elderly-pipeline.service as an ExecStartPost so the brand
shows on screen whenever the pipeline is running. Raw I2C (smbus2 + PIL); same
SH1106 init/blit path proven by record_samples.py.

Env: OLED_TEXT (default Noban), OLED_ADDR (0x3C), OLED_BUS (1),
     OLED_HEIGHT (32), OLED_WIDTH (128), OLED_DRIVER (sh1106).
"""
import os

_FONTS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


class Oled:
    def __init__(self, addr=0x3C, bus=1, w=128, h=32, driver="sh1106"):
        from smbus2 import SMBus
        from PIL import Image, ImageDraw
        self.addr, self.w, self.h, self.driver = addr, w, h, driver
        self.col_offset = 2 if driver == "sh1106" else 0  # SH1106 = 132 cols
        self.bus = SMBus(bus)
        self._Image, self._Draw = Image, ImageDraw
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

    def _best_font(self, text):
        from PIL import ImageFont
        probe = self._Draw.Draw(self._Image.new("1", (self.w, self.h)))
        for size in range(self.h + 4, 7, -1):
            font = None
            for path in _FONTS:
                try:
                    font = ImageFont.truetype(path, size)
                    break
                except Exception:
                    continue
            if font is None:
                return ImageFont.load_default()
            l, t, r, b = probe.textbbox((0, 0), text, font=font)
            if (r - l) <= self.w - 2 and (b - t) <= self.h:
                return font
        return ImageFont.load_default()

    def show_centered(self, text):
        img = self._Image.new("1", (self.w, self.h))
        d = self._Draw.Draw(img)
        font = self._best_font(text)
        l, t, r, b = d.textbbox((0, 0), text, font=font)
        x = (self.w - (r - l)) // 2 - l
        y = (self.h - (b - t)) // 2 - t
        d.text((x, y), text, font=font, fill=255)
        self._blit(img)

    def _blit(self, img):
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
            off = self.col_offset
            for page in range(self.h // 8):
                self._cmd(0xB0 + page, 0x00 | (off & 0x0F), 0x10 | (off >> 4))
                row = buf[page * self.w:(page + 1) * self.w]
                for i in range(0, len(row), 32):
                    self.bus.write_i2c_block_data(self.addr, 0x40, row[i:i + 32])
        else:
            self._cmd(0x21, 0, self.w - 1, 0x22, 0, self.h // 8 - 1)
            for i in range(0, len(buf), 32):
                self.bus.write_i2c_block_data(self.addr, 0x40, buf[i:i + 32])


if __name__ == "__main__":
    text = os.environ.get("OLED_TEXT", "Noban")
    o = Oled(addr=int(os.environ.get("OLED_ADDR", "0x3C"), 16),
             bus=int(os.environ.get("OLED_BUS", "1")),
             w=int(os.environ.get("OLED_WIDTH", "128")),
             h=int(os.environ.get("OLED_HEIGHT", "32")),
             driver=os.environ.get("OLED_DRIVER", "sh1106"))
    o.show_centered(text)
    print(f"OLED: '{text}' centered ({o.w}x{o.h} {o.driver})")
