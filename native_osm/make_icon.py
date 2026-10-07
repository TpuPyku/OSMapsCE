"""Draws the program icon and writes app.ico (16, 32, 48, 64 px, 32-bit with AND mask).

Windows CE can't read PNG-compressed icon entries, so every size is a plain BMP entry; where CE ignores
the alpha channel, the AND mask (alpha < 50 %) still cuts the shape out.

    python make_icon.py            -> app.ico next to this script
"""
import os
import struct
from PIL import Image, ImageDraw

TOP, BOTTOM = (126, 196, 104), (52, 138, 62)   # background gradient: green for OSMapsCE
SIZES = (16, 32, 48, 64)
S = 512                                       # drawing canvas, downscaled for every size


def draw():
    k = S / 256
    bg = Image.new('RGBA', (S, S))
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        ImageDraw.Draw(bg).line([(0, y), (S, y)], fill=c + (255,))
    # faint roads of a map
    roads = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(roads)
    w = int(14 * k)
    d.line([(0, 70 * k), (256 * k, 190 * k)], fill=(255, 255, 255, 70), width=w)
    d.line([(60 * k, 256 * k), (200 * k, 0)], fill=(255, 255, 255, 70), width=w)
    bg = Image.alpha_composite(bg, roads)
    # rounded square
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([8 * k, 8 * k, 248 * k, 248 * k], radius=52 * k, fill=255)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(bg, (0, 0), mask)
    # white map pin with a hole: circle and a point below it
    d = ImageDraw.Draw(img)
    cx, cy, r = 128 * k, 104 * k, 62 * k
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255, 255))
    d.polygon([(cx - r * 0.86, cy + r * 0.5), (cx + r * 0.86, cy + r * 0.5), (cx, 226 * k)], fill=(255, 255, 255, 255))
    h = 25 * k
    hole = tuple(int((TOP[i] + BOTTOM[i]) / 2) for i in range(3)) + (255,)
    d.ellipse([cx - h, cy - h, cx + h, cy + h], fill=hole)
    return img


def entry(img, n):
    im = img.resize((n, n), Image.LANCZOS)
    px = im.load()
    xor = bytearray()
    for y in range(n - 1, -1, -1):            # bottom-up
        for x in range(n):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if px[x, y][3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        andm += row
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return hdr + xor + andm


def main():
    img = draw()
    data = [entry(img, n) for n in SIZES]
    out = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for n, e in zip(SIZES, data):
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(e), offset)
        offset += len(e)
    for e in data:
        out += e
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.ico')
    open(path, 'wb').write(out)
    print(path, len(out))


if __name__ == '__main__':
    main()
