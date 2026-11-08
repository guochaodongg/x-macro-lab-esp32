#!/usr/bin/env python3
"""Generate a Doubao-style animated gradient-orb avatar collection.

Each emotion becomes a looping 128x128 GIF (twice the previous 64x64 pack).
The firmware detects GIFs by magic bytes and plays them with LvglGif/gifdec,
so no C++ changes are needed. Pure standard library, no Pillow.

Firmware constraints respected here:
  - GIF89a with a global color table and NETSCAPE2.0 loop=0 (otherwise gifdec
    stops after one pass).
  - One transparent palette index, disposal=1, binary alpha only.
  - A fixed local frame rectangle is fully redrawn every frame, so there is no
    ghosting and no reliance on disposal=2/3 (disposal=3 is unimplemented).
  - Small frame count and a flat 32-color palette keep assets.bin small.

Usage:
    python scripts/generate_dynamic_ball_emoji.py
    python scripts/generate_dynamic_ball_emoji.py --size 64 --output main/assets/emoji/dynamic_ball_64
"""

import argparse
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "main" / "assets" / "emoji" / "dynamic_ball_128"

TRANSPARENT = 0
EYE = 17
WHITE = 18
HIGHLIGHT = 19
MOUTH = 20
PINK = 21
TEAR = 22
RED = 23
YELLOW = 24
SHADES = 25
GLOW = 26
BLUSH = 27

PALETTE = [
    (0, 0, 0),          # 0 transparent
    (232, 246, 255),    # 1-16 radial orb gradient, bright centre -> violet rim
    (206, 232, 255),
    (180, 214, 255),
    (156, 194, 252),
    (134, 172, 248),
    (116, 150, 242),
    (102, 128, 234),
    (96, 110, 224),
    (100, 96, 214),
    (110, 84, 206),
    (124, 78, 200),
    (140, 76, 196),
    (158, 80, 196),
    (178, 88, 200),
    (198, 100, 208),
    (214, 116, 216),
    (36, 30, 66),       # 17 eye / outline
    (255, 255, 255),    # 18 white
    (240, 250, 255),    # 19 highlight
    (58, 40, 96),       # 20 mouth
    (246, 142, 166),    # 21 tongue
    (126, 196, 240),    # 22 tear / sweat
    (236, 88, 92),      # 23 red
    (255, 216, 96),     # 24 yellow
    (22, 18, 38),       # 25 shades
    (206, 178, 255),    # 26 outer glow
    (244, 150, 190),    # 27 blush
    (170, 244, 226),    # 28 spare
    (0, 0, 0),
    (0, 0, 0),
    (0, 0, 0),
]

FRAME_COUNT = 6
FRAME_DELAY = 13          # 1/100 s -> 130 ms per frame, ~0.78 s loop
MIN_CODE_SIZE = 5         # 32-color GCT
BALL_RADIUS_RATIO = 43 / 64
RECT_RATIO = (20 / 128, 20 / 128, 88 / 128, 88 / 128)

STYLES = {
    "neutral":     dict(eyes="open",                mouth="smile"),
    "happy":       dict(eyes="happy",               mouth="open",    extras=("sparkle",)),
    "laughing":    dict(eyes="happy",               mouth="open",    extras=("tear", "sparkle")),
    "funny":       dict(eyes=("wink", "open"),      mouth="tongue",  extras=("sparkle",)),
    "sad":         dict(eyes="half",                mouth="frown",   extras=("tear",)),
    "angry":       dict(eyes="narrow",              mouth="frown",   extras=("anger",)),
    "crying":      dict(eyes="closed",              mouth="wave",    extras=("tear", "sweat")),
    "loving":      dict(eyes="heart",               mouth="smile",   extras=("heart", "blush")),
    "embarrassed": dict(eyes="happy",               mouth="small",   extras=("blush", "sweat")),
    "surprised":   dict(eyes="wide",                mouth="o",       extras=("sparkle",)),
    "shocked":     dict(eyes="wide",                mouth="open",    extras=("sweat", "sparkle")),
    "thinking":    dict(eyes=("open", "squint"),    mouth="flat",    extras=("question",)),
    "winking":     dict(eyes=("wink", "open"),      mouth="smile",   extras=("sparkle",)),
    "cool":        dict(eyes="shades",              mouth="grin",    extras=("sparkle",)),
    "relaxed":     dict(eyes="closed",              mouth="smile"),
    "delicious":   dict(eyes="happy",               mouth="lick",    extras=("sparkle",)),
    "kissy":       dict(eyes="happy",               mouth="kiss",    extras=("heart", "blush")),
    "confident":   dict(eyes="squint",              mouth="grin",    extras=("sparkle",)),
    "sleepy":      dict(eyes="sleepy",              mouth="small",   extras=("zzz",)),
    "silly":       dict(eyes=("derp_l", "derp_r"),  mouth="tongue",  extras=("wobble",)),
    "confused":    dict(eyes=("open", "squint"),    mouth="wave",    extras=("question", "sweat")),
    "idle":        dict(eyes="closed",              mouth="smile"),
    "robot_2":     dict(eyes="open",                mouth="smile"),
}


class Frame:
    """Indexed 128-unit logical canvas; coordinates scale with the output size."""

    def __init__(self, size):
        self.n = size
        self.k = size / 128.0
        self.buf = bytearray(size * size)

    def set(self, x, y, index):
        ix, iy = int(x * self.k), int(y * self.k)
        if 0 <= ix < self.n and 0 <= iy < self.n:
            self.buf[iy * self.n + ix] = index

    def _paint(self, box, test, index):
        k, n = self.k, self.n
        x0, y0, x1, y1 = (v * k for v in box)
        for iy in range(max(0, int(y0)), min(n, int(y1) + 1)):
            cy = (iy + 0.5) / k
            for ix in range(max(0, int(x0)), min(n, int(x1) + 1)):
                if test((ix + 0.5) / k, cy):
                    self.buf[iy * n + ix] = index

    def ellipse(self, cx, cy, rx, ry, index, angle=0.0):
        cos, sin = math.cos(angle), math.sin(angle)

        def test(x, y):
            dx, dy = x - cx, y - cy
            u = (dx * cos + dy * sin) / rx
            v = (-dx * sin + dy * cos) / ry
            return u * u + v * v <= 1.0

        r = max(rx, ry)
        self._paint((cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1), test, index)

    def line(self, p0, p1, width, index):
        x0, y0 = p0
        x1, y1 = p1
        dx, dy = x1 - x0, y1 - y0
        length = dx * dx + dy * dy
        r = width / 2.0

        def test(x, y):
            t = 0.0 if length == 0 else max(
                0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / length))
            return (x - x0 - t * dx) ** 2 + (y - y0 - t * dy) ** 2 <= r * r

        self._paint((min(x0, x1) - r - 1, min(y0, y1) - r - 1,
                     max(x0, x1) + r + 1, max(y0, y1) + r + 1), test, index)

    def polyline(self, points, width, index):
        for start, end in zip(points, points[1:]):
            self.line(start, end, width, index)

    def arc(self, cx, cy, rx, ry, a0, a1, width, index):
        tol = width / (rx + ry)
        r = max(rx, ry)

        def test(x, y):
            u, v = (x - cx) / rx, (y - cy) / ry
            if abs(math.hypot(u, v) - 1.0) > tol:
                return False
            angle = math.degrees(math.atan2(v, u))
            while angle < a0:
                angle += 360
            return angle <= a1

        self._paint((cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1), test, index)

    def polygon(self, points, index):
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        count = len(points)

        def test(x, y):
            inside = False
            j = count - 1
            for i in range(count):
                xi, yi = points[i]
                xj, yj = points[j]
                if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                    inside = not inside
                j = i
            return inside

        self._paint((min(xs) - 1, min(ys) - 1, max(xs) + 1, max(ys) + 1), test, index)

    def crop(self, rect):
        left, top, w, h = rect
        return bytes(v for y in range(top, top + h)
                     for v in self.buf[(y * self.n + left):(y * self.n + left + w)])


def heart(f, cx, cy, r, index):
    f.ellipse(cx - r * 0.48, cy - r * 0.36, r * 0.6, r * 0.6, index)
    f.ellipse(cx + r * 0.48, cy - r * 0.36, r * 0.6, r * 0.6, index)
    f.polygon([(cx - r, cy - r * 0.1), (cx + r, cy - r * 0.1),
               (cx, cy + r * 1.2)], index)


def star4(f, cx, cy, r, index):
    f.polygon([(cx, cy - r), (cx + r * 0.24, cy - r * 0.24),
               (cx + r, cy), (cx + r * 0.24, cy + r * 0.24),
               (cx, cy + r), (cx - r * 0.24, cy + r * 0.24),
               (cx - r, cy), (cx - r * 0.24, cy - r * 0.24)], index)


def letter_z(f, x, y, w, index):
    f.polyline([(x, y), (x + w, y), (x, y + w * 1.4), (x + w, y + w * 1.4)],
               2.0, index)


def orb(f, size, phase):
    """Radial gradient orb plus a travelling light band and a moving highlight."""
    scale = size / 128.0
    radius = 43 * scale
    cx = cy = size / 2.0
    shifts = (-1, 0, 1, 1, 0, -1)
    offsets = (-26, -13, 0, 13, 26, 13)
    shift = shifts[phase % FRAME_COUNT]
    offset = offsets[phase % FRAME_COUNT] * scale
    glow = 45 * scale

    for iy in range(size):
        y = iy + 0.5
        for ix in range(size):
            x = ix + 0.5
            dx, dy = x - cx, y - cy
            d = math.hypot(dx, dy)
            if d <= glow:
                if d > radius:
                    f.buf[iy * size + ix] = GLOW
                    continue
                level = 1 + int(d / radius * 15 + shift)
                level = max(1, min(16, level))
                band = dx * 0.55 + dy * 0.83
                if abs(band - offset) < 9 * scale:
                    level = max(1, level - 3)
                f.buf[iy * size + ix] = level

    hx = 50 + (phase % FRAME_COUNT - 2.5) * 1.5
    hy = 44 + (2.5 - phase % FRAME_COUNT) * 0.8
    f.ellipse(hx, hy, 9, 5, HIGHLIGHT, angle=-0.5)


def one_eye(f, x, y, kind, blink):
    if blink:
        f.line((x - 9, y + 1), (x + 9, y + 1), 3.0, EYE)
        return
    if kind in ("open", "wide", "narrow", "derp_l", "derp_r", "half", "sleepy"):
        rx = 9.0 if kind == "wide" else 8.0
        ry = 10.0 if kind == "wide" else (4.0 if kind == "narrow" else 8.0)
        f.ellipse(x, y, rx, ry, EYE)
        if kind in ("half", "sleepy"):
            f.line((x - rx, y + 1), (x + rx, y + 1), 2.2, EYE)
        else:
            dx = 3.0 if kind == "derp_l" else (-3.0 if kind == "derp_r" else 0)
            f.ellipse(x + dx - 2, y - 2, 2.0, 2.2, WHITE)
    elif kind in ("happy", "wink"):
        f.polyline([(x - 9, y + 3), (x - 4.5, y - 3), (x, y - 4),
                    (x + 4.5, y - 3), (x + 9, y + 3)], 3.0, EYE)
    elif kind == "closed":
        f.polyline([(x - 9, y - 2), (x - 4.5, y + 2), (x, y + 3),
                    (x + 4.5, y + 2), (x + 9, y - 2)], 2.8, EYE)
    elif kind == "squint":
        f.line((x - 9, y - 1), (x + 9, y - 1), 2.6, EYE)
        f.ellipse(x + 1, y + 2, 2.4, 2.4, EYE)
    elif kind == "heart":
        heart(f, x, y, 7.0, RED)
    elif kind == "x":
        f.line((x - 7, y - 6), (x + 7, y + 6), 2.8, EYE)
        f.line((x + 7, y - 6), (x - 7, y + 6), 2.8, EYE)


def eyes(f, spec, blink):
    if spec == "shades":
        f.line((30, 54), (98, 54), 3.0, SHADES)
        f.ellipse(46, 60, 13, 9, SHADES, angle=-0.12)
        f.ellipse(82, 60, 13, 9, SHADES, angle=0.12)
        f.line((37, 56), (47, 55), 2.0, WHITE)
        f.line((77, 55), (87, 56), 2.0, WHITE)
        return
    left, right = spec if isinstance(spec, tuple) else (spec, spec)
    one_eye(f, 46, 60, left, blink)
    one_eye(f, 82, 60, right, blink)


def mouth(f, kind, phase):
    open_wide = kind == "open" and phase in (1, 2, 3)
    if kind == "smile":
        f.arc(64, 87, 9, 6, 0, 180, 2.4, MOUTH)
    elif kind == "frown":
        f.arc(64, 98, 9, 6, 180, 360, 2.4, MOUTH)
    elif kind == "open":
        ry = 7 if open_wide else 5
        f.ellipse(64, 92, 8, ry, MOUTH)
        f.ellipse(64, 95, 5, 3, PINK)
    elif kind == "o":
        f.ellipse(64, 92, 4, 5, MOUTH)
    elif kind == "flat":
        f.line((56, 92), (72, 92), 2.4, MOUTH)
    elif kind == "tongue":
        f.arc(64, 87, 9, 5, 0, 180, 2.4, MOUTH)
        f.ellipse(66, 97, 6, 4, PINK)
        f.line((66, 94), (66, 100), 1.6, RED)
    elif kind == "lick":
        f.arc(64, 87, 9, 5, 0, 180, 2.4, MOUTH)
        f.ellipse(75, 91, 5, 3, PINK)
    elif kind == "kiss":
        f.ellipse(64, 90, 4, 3, RED)
        f.ellipse(63, 89, 1.2, 0.8, WHITE)
    elif kind == "grin":
        f.arc(64, 87, 9, 5, 0, 180, 2.4, MOUTH)
        f.line((55, 87), (73, 87), 1.8, MOUTH)
    elif kind == "small":
        f.ellipse(64, 91, 2.5, 2, MOUTH)
    elif kind == "wave":
        f.polyline([(54, 92), (59, 95), (64, 90), (69, 95), (74, 92)],
                   2.4, MOUTH)


def extras(f, items, phase):
    bob = (phase % FRAME_COUNT - 2.5) * 0.8
    for item in items:
        if item == "blush":
            f.ellipse(34, 78, 6, 3.5, BLUSH)
            f.ellipse(94, 78, 6, 3.5, BLUSH)
        elif item == "tear":
            for x in (36, 92):
                f.ellipse(x, 78 + bob, 3, 4.5, TEAR)
                f.polygon([(x - 3, 77 + bob), (x + 3, 77 + bob),
                           (x, 69 + bob)], TEAR)
        elif item == "sweat":
            f.ellipse(106, 34, 2.8, 4, TEAR)
            f.polygon([(103.5, 33), (108.5, 33), (106, 27)], TEAR)
        elif item == "heart":
            heart(f, 106 + bob, 26, 5, RED)
            heart(f, 20, 22, 3.4, PINK)
        elif item == "sparkle":
            star4(f, 106 + bob, 26, 5, YELLOW)
            star4(f, 20, 34, 3.4, YELLOW)
        elif item == "zzz":
            letter_z(f, 98, 18 + bob, 5, TEAR)
            letter_z(f, 108, 8 + bob, 3.6, TEAR)
        elif item == "anger":
            f.line((96, 20), (106, 30), 2.0, RED)
            f.line((106, 20), (96, 30), 2.0, RED)
        elif item == "question":
            f.arc(106, 30, 4.5, 4.5, 180, 350, 2.0, TEAR)
            f.line((106, 33), (106, 39), 2.0, TEAR)
            f.ellipse(106, 42, 1.6, 1.6, TEAR)
        elif item == "wobble":
            f.polyline([(14, 60), (18, 56), (22, 60), (26, 56)], 1.8, YELLOW)
            f.polyline([(102, 60), (106, 56), (110, 60), (114, 56)], 1.8, YELLOW)


def render(style, size, phase, rect):
    f = Frame(size)
    orb(f, size, phase)
    blink = phase in (3,) and style["eyes"] not in ("closed", "shades")
    eyes(f, style["eyes"], blink)
    mouth(f, style["mouth"], phase)
    extras(f, style.get("extras", ()), phase)
    return f.crop(rect)


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.bits = 0

    def write(self, code, size):
        self.acc |= code << self.bits
        self.bits += size
        while self.bits >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.bits -= 8

    def finish(self):
        if self.bits:
            self.buf.append(self.acc & 0xFF)
            self.acc = self.bits = 0
        return bytes(self.buf)


def lzw_encode(pixels, min_code_size):
    clear = 1 << min_code_size
    eoi = clear + 1
    code_size = min_code_size + 1
    next_code = eoi + 1
    table = {}
    out = BitWriter()
    out.write(clear, code_size)
    prefix = pixels[0]
    for value in pixels[1:]:
        key = (prefix, value)
        if key in table:
            prefix = table[key]
            continue
        out.write(prefix, code_size)
        if next_code < 4096:
            table[key] = next_code
            next_code += 1
            # Classic GIF encoders widen codes one write after the dictionary
            # grows. gifdec's decoder increments its width after reading the
            # next key, so widening eagerly corrupts every later code.
            if next_code == (1 << code_size) + 1 and code_size < 12:
                code_size += 1
        else:
            out.write(clear, code_size)
            table = {}
            next_code = eoi + 1
            code_size = min_code_size + 1
        prefix = value
    out.write(prefix, code_size)
    out.write(eoi, code_size)
    return out.finish()


def gifdec_decode(stream, min_code_size, expected, frame_width, frame_height):
    """Port of gifdec.c read_image_data() LZW logic, used to verify playback.

    The firmware decoder grows its dictionary one code later than the encoder
    and increments the code width after reading the next key, so a generic
    round-trip check would not prove compatibility with the device.
    """
    clear = 1 << min_code_size
    stop = clear + 1
    big = int.from_bytes(stream, "little")
    bitpos = 0

    def get_key(size):
        nonlocal bitpos
        value = (big >> bitpos) & ((1 << size) - 1)
        bitpos += size
        return value

    def new_table():
        return [(1, 0xFFF, i) for i in range(clear)] + [(0, 0, 0), (0, 0, 0)]

    key_size = min_code_size + 1
    init_key_size = key_size
    entries = new_table()
    table_is_full = False
    ret = 0
    str_len = 0
    entry = (0, 0, 0)
    frm_off = 0
    frm_size = frame_width * frame_height
    out = bytearray()
    key = get_key(key_size)
    while frm_off < frm_size:
        if key == clear:
            key_size = init_key_size
            entries = new_table()
            table_is_full = False
            ret = 0
        elif not table_is_full:
            entries.append((str_len + 1, key, entry[2]))
            n = len(entries)
            ret = 1 if (n & (n - 1)) == 0 else 0
            if n == 0x1000:
                ret = 0
                table_is_full = True
        key = get_key(key_size)
        if key == clear:
            continue
        if key == stop or key == 0x1000:
            break
        if ret == 1:
            key_size += 1
        entry = entries[key]
        str_len = entry[0]
        if frm_off + str_len > frm_size:
            raise ValueError("LZW token overflows the frame buffer")
        cur = entry
        chunk = bytearray(str_len)
        for i in range(str_len):
            chunk[str_len - 1 - i] = cur[2]
            if cur[1] == 0xFFF:
                break
            cur = entries[cur[1]]
        out.extend(chunk)
        frm_off += str_len
        if key < len(entries) - 1 and not table_is_full:
            length, prefix, _ = entries[-1]
            entries[-1] = (length, prefix, cur[2])
        entry = cur
    return bytes(out[:expected])


def pack_gif(frames, size, rect, delay, palette):
    left, top, width, height = rect
    data = bytearray(b"GIF89a")
    data += struct.pack("<HHBBB", size, size, 0xF4, 0, 0)
    for r, g, b in palette:
        data += bytes((r, g, b))
    data += (b"\x21\xFF\x0B" + b"NETSCAPE2.0" +
             b"\x03\x01\x00\x00" + b"\x00")
    for frame in frames:
        data += b"\x21\xF9\x04" + bytes((0x05,)) + struct.pack("<H", delay)
        data += bytes((TRANSPARENT, 0x00))
        data += b"\x2C" + struct.pack("<HHHH", left, top, width, height)
        data += b"\x00" + bytes((MIN_CODE_SIZE,))
        compressed = lzw_encode(frame, MIN_CODE_SIZE)
        for i in range(0, len(compressed), 255):
            part = compressed[i:i + 255]
            data += bytes((len(part),)) + part
        data += b"\x00"
    data += b"\x3B"
    return bytes(data)


def verify_gif(blob, frames, size, rect):
    """Parse back the GIF and compare every decoded frame with the source."""
    left, top, width, height = rect
    assert blob[:6] == b"GIF89a"
    gif_w, gif_h = struct.unpack("<HH", blob[6:10])
    assert (gif_w, gif_h) == (size, size)
    pos = 13 + 3 * len(PALETTE)
    assert blob[pos:pos + 3] == b"\x21\xFF\x0B"
    assert blob[pos + 3:pos + 14] == b"NETSCAPE2.0"
    pos += 3 + 11
    pos += blob[pos] + 2
    for expected in frames:
        assert blob[pos:pos + 2] == b"\x21\xF9", "missing graphic control"
        packed = blob[pos + 3]
        assert packed == 0x05, "disposal/transparency mismatch"
        assert blob[pos + 6] == TRANSPARENT
        pos += 8
        assert blob[pos] == 0x2C, "missing image descriptor"
        fx, fy, fw, fh = struct.unpack("<HHHH", blob[pos + 1:pos + 9])
        assert (fx, fy, fw, fh) == (left, top, width, height)
        pos += 10
        assert blob[pos] == MIN_CODE_SIZE
        pos += 1
        stream = bytearray()
        while blob[pos]:
            length = blob[pos]
            stream += blob[pos + 1:pos + 1 + length]
            pos += 1 + length
        pos += 1
        decoded = gifdec_decode(bytes(stream), MIN_CODE_SIZE, width * height,
                                width, height)
        assert decoded == expected, "gifdec LZW round-trip mismatch"
    assert blob[pos] == 0x3B, "missing trailer"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=128, choices=(64, 128),
                        help="Canvas edge in pixels (default: 128)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="Directory to write <emotion>.gif into")
    args = parser.parse_args()

    size = args.size
    ratio = size / 128.0
    rect = tuple(int(round(v * ratio)) for v in
                 (RECT_RATIO[0] * 128, RECT_RATIO[1] * 128,
                  RECT_RATIO[2] * 128, RECT_RATIO[3] * 128))
    radius = int(round(BALL_RADIUS_RATIO * size))
    assert radius + rect[0] <= size and radius + rect[1] <= size

    args.output.mkdir(parents=True, exist_ok=True)
    total = 0
    for name, style in STYLES.items():
        frames = [render(style, size, phase, rect)
                  for phase in range(FRAME_COUNT)]
        blob = pack_gif(frames, size, rect, FRAME_DELAY, PALETTE)
        verify_gif(blob, frames, size, rect)
        (args.output / f"{name}.gif").write_bytes(blob)
        total += len(blob)
    print(f"Generated {len(STYLES)} animated orb emojis "
          f"({size}x{size}, {FRAME_COUNT} frames, {FRAME_DELAY * 10} ms/frame) "
          f"in {args.output}; total {total / 1024:.1f} KiB")


if __name__ == "__main__":
    main()
