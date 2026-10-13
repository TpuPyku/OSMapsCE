"""Draws the skin in the VestaHome style: Images/MapGL.png (800 x 480 background) and the 50 x 50 buttons
Images/B<name>_N.png (normal) / _P.png (pressed).

The buttons are opaque and blitted over MapGL.png, so their rounded corners are drawn in the colour of the
background right behind them (the plain right strip). Normal: a dark tile with a 2 px border and a light line
sign; a mode that is on (traffic, night, GPS follow): the sign in the orange accent; pressed: an orange tile
with a dark sign. Drawn 4x and scaled down on the opaque background, so the edges are smooth with no alpha.
YaMapsCE and OSMapsCE keep a copy each; APP picks the buttons (OSMapsCE: traffic source instead of layers).

    python make_images.py        -> ..\\YaMapsCE\\Images (or ..\\OSMapsCE\\Images)
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

APP = 'OSMapsCE'                              # 'YaMapsCE' or 'OSMapsCE'

INK, PANEL, LINE = (0x0b, 0x0c, 0x0e), (0x17, 0x19, 0x1c), (0x34, 0x38, 0x3f)   # VestaHome palette
TEXT, MUTED, DIM, ORANGE = (0xf3, 0xf1, 0xec), (0xa3, 0xa8, 0xb0), (0x5d, 0x63, 0x6c), (0xff, 0x6b, 0x1a)
MAP = (2, 28, 600, 450)                       # default map_x, map_y, map_w, map_h (util.cpp)
B, K = 50, 4                                  # button size, drawing scale
FONT = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'tahomabd.ttf')


def tile(fill, border):
    im = Image.new('RGB', (B * K, B * K), INK)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([K, K, (B - 1) * K - 1, (B - 1) * K - 1], radius=12 * K, fill=fill,
                        outline=border, width=2 * K if border else 0)
    return im, d


def P(*pts):                                  # button coordinates (0..50) -> canvas
    return [(x * K, y * K) for x, y in pts]


def line(d, c, pts, w=3.2):
    wp = int(round(w * K))
    d.line(P(*pts), fill=c, width=wp, joint='curve')
    r = wp / 2.0 - 0.5                        # round caps no wider than the line (wider ones showed as knobs)
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x * K - r, y * K - r, x * K + r, y * K + r], fill=c)


def bar(d, c, x0, y0, x1, y1, w=3.2):         # a straight stroke as one rounded rectangle: clean ends
    h = w * K / 2
    d.rounded_rectangle([min(x0, x1) * K - h, min(y0, y1) * K - h, max(x0, x1) * K + h, max(y0, y1) * K + h],
                        radius=h, fill=c)


def arrow(d, c, ang, cx=25, cy=25, filled=True):    # navigation arrow pointing at ang (degrees, 0 = up)
    pts = [(0, -11), (8, 10), (0, 5), (-8, 10)]
    a = math.radians(ang)
    q = [(cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a)) for x, y in pts]
    if filled:
        d.polygon(P(*q), fill=c)
    else:
        line(d, c, q + [q[0]], 2.6)


def sign(d, name, c, bg):
    if name == 'plus':
        bar(d, c, 15, 25, 35, 25)
        bar(d, c, 25, 15, 25, 35)
    elif name == 'minus':
        bar(d, c, 15, 25, 35, 25)
    elif name in ('left', 'right', 'up', 'down'):
        a = {'up': 0, 'right': 90, 'down': 180, 'left': 270}[name]
        r = math.radians(a)
        rot = lambda x, y: (25 + x * math.cos(r) - y * math.sin(r), 25 + x * math.sin(r) + y * math.cos(r))
        line(d, c, [rot(0, 11), rot(0, -10)])
        line(d, c, [rot(-8, -2), rot(0, -10), rot(8, -2)])
    elif name == 'set':                       # gear: ring with 8 teeth and a hole
        cx = cy = 25 * K
        for i in range(8):
            a = math.radians(i * 45)
            x, y = cx + math.cos(a) * 11.5 * K, cy + math.sin(a) * 11.5 * K
            d.ellipse([x - 3.4 * K, y - 3.4 * K, x + 3.4 * K, y + 3.4 * K], fill=c)
        d.ellipse([cx - 11 * K, cy - 11 * K, cx + 11 * K, cy + 11 * K], fill=c)
        d.ellipse([cx - 4.6 * K, cy - 4.6 * K, cx + 4.6 * K, cy + 4.6 * K], fill=bg)
    elif name == 'home':                      # a house, as VestaHome's home
        line(d, c, [(12, 26), (25, 13), (38, 26)])
        line(d, c, [(17, 22), (17, 37), (33, 37), (33, 22)])
    elif name == 'exit':                      # power sign
        d.arc([13 * K, 14 * K, 37 * K, 38 * K], start=-50, end=230, fill=c, width=int(3.2 * K))
        line(d, c, [(25, 11), (25, 24)])
    elif name == 'night':                     # crescent
        d.ellipse([13 * K, 13 * K, 37 * K, 37 * K], fill=c)
        d.ellipse([20 * K, 8 * K, 42 * K, 30 * K], fill=bg)
    elif name == 'trf':                       # traffic light
        d.rounded_rectangle([17 * K, 10 * K, 33 * K, 40 * K], radius=6 * K, outline=c, width=int(2.8 * K))
        for y in (17, 25, 33):
            d.ellipse([22 * K, (y - 3) * K, 28 * K, (y + 3) * K], fill=c)
    elif name == 'layers':                    # two stacked sheets
        for dy, fill in ((6, False), (0, True)):
            q = [(25, 12 + dy), (38, 20 + dy), (25, 28 + dy), (12, 20 + dy)]
            if fill:
                d.polygon(P(*q), fill=bg)
            line(d, c, q + [q[0]], 2.8)
    elif name == 'gps_on':                    # follow with the course: the arrow points ahead
        arrow(d, c, 0)
    elif name == 'gps_off':                   # follow, north up
        arrow(d, c, 35, filled=False)
    elif name == 'gps_none':                  # not following
        arrow(d, c, 35, filled=False)
        line(d, c, [(13, 37), (37, 13)], 2.8)
    elif name in ('tsrc_ya', 'tsrc_2g'):      # traffic source: Yandex / 2GIS
        f = ImageFont.truetype(FONT, 21 * K)
        t = 'Я' if name == 'tsrc_ya' else '2Г'
        d.text((25 * K, 26 * K), t, fill=c, font=f, anchor='mm')


# (file name, sign, the mode is "on" in this state)
BUTTONS = [('Bset', 'set', False), ('Bexit', 'exit', False), ('Bnight_on', 'night', True),
           ('Bnight_off', 'night', False), ('Btrf_on', 'trf', True), ('Btrf_off', 'trf', False),
           ('Bgps_on', 'gps_on', True), ('Bgps_off', 'gps_off', False), ('Bgps_none', 'gps_none', False),
           ('Bleft', 'left', False), ('Bright', 'right', False), ('Bup', 'up', False), ('Bdown', 'down', False),
           ('Bplus', 'plus', False), ('Bminus', 'minus', False)]
BUTTONS += [('Blayers', 'layers', False)] if APP == 'YaMapsCE' else \
           [('Btsrc_ya', 'tsrc_ya', False), ('Btsrc_2g', 'tsrc_2g', False)]


def background():
    im = Image.new('RGB', (800, 480), INK)
    d = ImageDraw.Draw(im)
    x, y, w, h = MAP
    d.rectangle([0, y - 2, 799, y - 1], fill=LINE)            # under the status line
    d.rectangle([x - 1, y - 1, x + w, y + h], outline=LINE)    # the map frame
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=PANEL)      # seen until the map is loaded
    d.rectangle([0, y - 2, 120, y - 1], fill=ORANGE)           # the one accent, as in VestaHome's top bar
    return im


# OSMapsCE draws its map over the whole screen, the bars and the tiles over it are see-through (the program
# darkens the map under them and draws their borders): the pictures are only the signs, with alpha, blended by
# hand. _N: the sign in its colour (a mode that is on: orange), _P: dark, for the orange fill of a press.
SIGNS = [('Bplus', 'plus', False), ('Bminus', 'minus', False), ('Bgps_on', 'gps_on', True),
         ('Bgps_off', 'gps_off', False), ('Bgps_none', 'gps_none', False),
         ('Bhome', 'home', False), ('Bset', 'set', False), ('Bnight_on', 'night', True),
         ('Bnight_off', 'night', False), ('Btrf_on', 'trf', True), ('Btrf_off', 'trf', False),
         ('Btsrc_ya', 'tsrc_ya', False), ('Btsrc_2g', 'tsrc_2g', False)]
SMALL = [('Sleft', 'left', False), ('Sright', 'right', False)]   # settings: back, the steppers' arrows
SIGN_SIZE, SMALL_SIZE = 48, 32


def sign_image(sg, on, pressed, size):
    c = INK if pressed else ORANGE if on else MUTED if sg == 'gps_none' else TEXT
    im = Image.new('RGBA', (B * K, B * K), (0, 0, 0, 0))
    sign(ImageDraw.Draw(im), sg, c + (255,), (0, 0, 0, 0))   # cut-outs (gear hole, crescent) see-through
    return im.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', APP, 'Images')
    if APP == 'OSMapsCE':
        Image.new('RGB', (800, 480), INK).save(os.path.join(out, 'MapGL.png'))   # covered by the map
        n = 1
        for group, size in ((SIGNS, SIGN_SIZE), (SMALL, SMALL_SIZE)):
            for name, sg, on in group:
                for pressed in (False, True):
                    sign_image(sg, on, pressed, size).save(
                        os.path.join(out, '%s_%s.png' % (name, 'P' if pressed else 'N')))
                    n += 1
        for old in ('Bexit_N.png', 'Bexit_P.png'):   # exit moved to the settings screen
            if os.path.exists(os.path.join(out, old)):
                os.remove(os.path.join(out, old))
        print(out, n, 'files')
        return
    background().save(os.path.join(out, 'MapGL.png'))
    for name, sg, on in BUTTONS:
        for pressed in (False, True):
            if pressed:
                im, d = tile(ORANGE, None)
                c, bg = INK, ORANGE
            else:
                im, d = tile(PANEL, LINE)
                c, bg = (ORANGE if on else MUTED if sg == 'gps_none' else TEXT), PANEL
            sign(d, sg, c, bg)
            im.resize((B, B), Image.LANCZOS).save(os.path.join(out, '%s_%s.png' % (name, 'P' if pressed else 'N')))
    print(out, len(BUTTONS) * 2 + 1, 'files')


if __name__ == '__main__':
    main()
