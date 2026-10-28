"""The PC app's icon (the user 2026-10-28: a green tag drawn on a floppy disk - the maps to carry): app.ico, all the
Windows sizes. Our own drawing (not OpenStreetMap's logo).

    python make_icon.py
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = (16, 24, 32, 48, 64, 128, 256)
DISK, SHUTTER, LABEL, TAG, HOLE = (35, 39, 46), (170, 176, 184), (245, 243, 238), (46, 160, 67), (245, 243, 238)


def draw(size=256):
    """The icon at one size (drawn 4 times larger and scaled down: smooth edges)."""
    k = size * 4 / 256
    im = Image.new('RGBA', (size * 4, size * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s = lambda *v: [int(x * k) for x in v]   # noqa: E731 - the 256 px drawing's coordinates scaled
    d.polygon([tuple(s(16, 16)), tuple(s(208, 16)), tuple(s(240, 48)), tuple(s(240, 240)), tuple(s(16, 240))], fill=DISK)
    d.rectangle(s(64, 16, 184, 84), fill=SHUTTER)            # the metal shutter
    d.rectangle(s(142, 26, 166, 74), fill=DISK)               # its window
    d.rectangle(s(44, 112, 212, 228), fill=LABEL)             # the label
    tag = [s(76, 136), s(150, 136), s(184, 170), s(150, 204), s(76, 204)]   # a tag pointing right
    d.polygon([tuple(p) for p in tag], fill=TAG)
    d.ellipse(s(136, 160, 156, 180), fill=HOLE)               # its hole
    return im.resize((size, size), Image.LANCZOS)


def main(out=os.path.join(HERE, 'app.ico')):
    draw(256).save(out, sizes=[(n, n) for n in SIZES])
    return out


if __name__ == '__main__':
    print(main())
