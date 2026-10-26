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

INK, PANEL, LINE = (0x0e, 0x10, 0x13), (0x1a, 0x1d, 0x22), (0x2a, 0x2f, 0x37)   # VestaHome palette (design 0.6)
TEXT, MUTED, DIM, ORANGE = (0xf5, 0xf3, 0xee), (0xaa, 0xb0, 0xb8), (0x6c, 0x72, 0x7c), (0xff, 0x6b, 0x1a)
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


# VestaHome's icons (home.cpp PaintIcon): a 48-unit grid drawn 30 px, stroke 3.5 units; here inside the 44 px
# dock picture (this canvas: 50 units), centred - the same shapes and sizes as VestaHome's bottom bar
VH_O, VH_K = 7 * 50.0 / 44, 30.0 / 48 * 50 / 44
VH_W = 3.5 * VH_K


def vh(x, y):
    return (VH_O + x * VH_K, VH_O + y * VH_K)


def vh_arc(cx, cy, rad, a0, a1, n=24):
    return [vh(cx + rad * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
               cy + rad * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def sign(d, name, c, bg):
    if name == 'vh_home':
        line(d, c, [vh(6, 23), vh(24, 7), vh(42, 23)], VH_W)
        line(d, c, [vh(11, 19), vh(11, 41), vh(37, 41), vh(37, 19)], VH_W)
    elif name == 'vh_set':
        pts = []
        for i in range(8):
            for da, rad in ((-15, 14), (-9, 20), (9, 20), (15, 14)):
                a = math.radians(i * 45 + da)
                pts.append(vh(24 + rad * math.cos(a), 24 + rad * math.sin(a)))
        line(d, c, pts + [pts[0]], VH_W)
        line(d, c, vh_arc(24, 24, 6, 0, 360), VH_W)
    elif name == 'vh_sun':
        line(d, c, vh_arc(24, 24, 8, 0, 360), VH_W)
        for a, b in (((24, 4), (24, 10)), ((24, 38), (24, 44)), ((4, 24), (10, 24)), ((38, 24), (44, 24)),
                     ((10, 10), (13.5, 13.5)), ((34.5, 34.5), (38, 38)), ((10, 38), (13.5, 34.5)),
                     ((34.5, 13.5), (38, 10))):
            line(d, c, [vh(*a), vh(*b)], VH_W)
    elif name == 'vh_night':
        line(d, c, vh_arc(24, 24, 16, 8.6, 261.4), VH_W)
        line(d, c, vh_arc(32, 16, 13, 53, 217), VH_W)
    elif name == 'vh_search':                # a magnifier, as VestaHome's search
        line(d, c, vh_arc(21, 21, 12, 0, 360), VH_W)
        line(d, c, [vh(30, 30), vh(41, 41)], VH_W)
    elif name == 'vh_routeoff':              # the route off: a cross
        line(d, c, [vh(12, 12), vh(36, 36)], VH_W)
        line(d, c, [vh(36, 12), vh(12, 36)], VH_W)
    elif name == 'plus':
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
    elif name == 'set':                       # gear as VestaHome's IC_GEAR: an outlined cog, 8 teeth, a hole
        s = 0.62                              # VestaHome's 48-unit icon -> this button
        pts = []
        for i in range(8):
            for da, r in ((-15, 14), (-9, 20), (9, 20), (15, 14)):
                a = math.radians(i * 45 + da)
                pts.append((25 + r * s * math.cos(a), 25 + r * s * math.sin(a)))
        line(d, c, pts + [pts[0]], 2.6)
        r = 6 * s * K
        d.ellipse([25 * K - r, 25 * K - r, 25 * K + r, 25 * K + r], outline=c, width=int(2.6 * K))
    elif name == 'home':                      # a house, as VestaHome's home
        line(d, c, [(12, 26), (25, 13), (38, 26)])
        line(d, c, [(17, 22), (17, 37), (33, 37), (33, 22)])
    elif name == 'exit':                      # power sign
        d.arc([13 * K, 14 * K, 37 * K, 38 * K], start=-50, end=230, fill=c, width=int(3.2 * K))
        line(d, c, [(25, 11), (25, 24)])
    elif name == 'night':                     # crescent
        d.ellipse([13 * K, 13 * K, 37 * K, 37 * K], fill=c)
        d.ellipse([20 * K, 8 * K, 42 * K, 30 * K], fill=bg)
    elif name == 'trf':                       # traffic: two cars one behind the other, seen from the front
        def car(cx, top, s, cover):           # a car's front: windscreen on the body, lights, wheels
            body = [(cx - 13 * s, top + 8 * s), (cx + 13 * s, top + 8 * s), (cx + 13 * s, top + 17 * s),
                    (cx - 13 * s, top + 17 * s)]
            roof = [(cx - 13 * s, top + 8 * s), (cx - 9 * s, top), (cx + 9 * s, top), (cx + 13 * s, top + 8 * s)]
            if cover:                         # the front car hides the one behind it
                d.polygon(P(*(roof + body[2:])), fill=bg)
                d.rectangle([(cx - 15 * s) * K, (top + 17 * s) * K, (cx + 15 * s) * K, (top + 21 * s) * K], fill=bg)
            line(d, c, roof, 2.6 * s)
            line(d, c, body + [body[0]], 2.6 * s)
            for x in (cx - 8 * s, cx + 8 * s):
                r = 2.1 * s
                d.ellipse([(x - r) * K, (top + 12.5 * s - r) * K, (x + r) * K, (top + 12.5 * s + r) * K], fill=c)
            for x in (cx - 10 * s, cx + 10 * s):
                bar(d, c, x - 1.6 * s, top + 19.5 * s, x + 1.6 * s, top + 19.5 * s, 2.6 * s)
        car(31, 9, 0.72, False)
        car(21, 20, 1.0, True)
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
         ('Bgps_off', 'gps_off', False), ('Bgps_none', 'gps_none', False)]           # the tiles over the map
DOCK = [('Bhome', 'vh_home', False), ('Bset', 'vh_set', False), ('Bnight_on', 'vh_sun', False),
        ('Bnight_off', 'vh_night', False), ('Btrf_on', 'trf', True), ('Btrf_off', 'trf', False),
        ('Btsrc_ya', 'tsrc_ya', False), ('Btsrc_2g', 'tsrc_2g', False), ('Bsearch', 'vh_search', False),
        ('Broff', 'vh_routeoff', False)]   # the bottom bar: smaller, a caption under
SMALL = [('Sleft', 'left', False), ('Sright', 'right', False)]   # settings: back, the steppers' arrows
SIGN_SIZE, DOCK_SIZE, SMALL_SIZE = 48, 44, 32   # the bar's signs as large as VestaHome's (30 px of 48 units)


def sign_image(sg, on, pressed, size, dock=False):
    # the bottom bar's signs in VestaHome's colours: muted, a mode that is on orange
    c = INK if pressed else ORANGE if on else MUTED if sg == 'gps_none' or dock else TEXT
    im = Image.new('RGBA', (B * K, B * K), (0, 0, 0, 0))
    sign(ImageDraw.Draw(im), sg, c + (255,), (0, 0, 0, 0))   # cut-outs (gear hole, crescent) see-through
    return im.resize((size, size), Image.LANCZOS)


# ---- OSMapsCE: VestaHome's icons (the user 2026-10-26: the icons as VestaHome's) - Lucide (lucide.dev, ISC licence,
# the SVGs cached in lucide/ with it), drawn as VestaHome/skin/make_icons.py does: stroke 2 of 24, round ends, its
# own antialiasing; the traffic source stays a letter in VestaHome's Golos. Orange (the "on" state) is the design's:
# OSMapsCE recolours it into the theme's accent when it loads the pictures.
LUCIDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lucide')
LUCIDE_URL = 'https://unpkg.com/lucide-static@1.61.0/'
GOLOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'OSMapsCE', 'Fonts', 'GolosText-Bold.ttf')
# picture name -> (Lucide icon or 'T:text', on: drawn in the accent, filled)
LUCIDE_SIGNS = {'Bplus': ('plus', False, False), 'Bminus': ('minus', False, False),
                'Bgps_on': ('navigation', True, True), 'Bgps_off': ('locate-fixed', False, False),
                'Bgps_none': ('locate', False, False)}
LUCIDE_DOCK = {'Bhome': ('house', False, False), 'Bset': ('settings', False, False), 'Bnight_on': ('sun', False, False),
               'Bnight_off': ('moon', False, False), 'Btrf_on': ('car-front', True, False),
               'Btrf_off': ('car-front', False, False), 'Btsrc_ya': ('T:Я', False, False),
               'Btsrc_2g': ('T:2Г', False, False), 'Bsearch': ('search', False, False),
               'Broff': ('route-off', False, False)}
LUCIDE_SMALL = {'Sleft': ('chevron-left', False, False), 'Sright': ('chevron-right', False, False),
                'Sback': ('undo-2', False, False)}


def lucide_svg(name):
    import urllib.request
    path = os.path.join(LUCIDE, name + '.svg')
    if not os.path.exists(path):
        os.makedirs(LUCIDE, exist_ok=True)
        open(path, 'wb').write(urllib.request.urlopen(LUCIDE_URL + 'icons/%s.svg' % name, timeout=30).read())
        lic = os.path.join(LUCIDE, 'LICENSE')
        if not os.path.exists(lic):
            open(lic, 'wb').write(urllib.request.urlopen(LUCIDE_URL + 'LICENSE', timeout=30).read())
    svg = open(path, encoding='utf-8').read()
    import re
    body = svg[svg.index('>', svg.index('<svg')) + 1:svg.rindex('</svg>')]
    return re.sub(r'<!--.*?-->', '', body, flags=re.S)


def lucide_image(spec, color, canvas, icon):
    name, _, filled = spec
    im = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
    if name.startswith('T:'):   # a letter: VestaHome's font
        f = ImageFont.truetype(GOLOS, int(icon * 0.72))
        ImageDraw.Draw(im).text((canvas / 2, canvas / 2), name[2:], font=f, fill=color + (255,), anchor='mm')
        return im
    import pymupdf
    hexc = '#%02x%02x%02x' % color
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="%s" stroke="%s" '
           'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">%s</svg>'
           % (hexc if filled else 'none', hexc, lucide_svg(name)))
    doc = pymupdf.open(stream=svg.encode('utf-8'), filetype='svg')
    pix = doc[0].get_pixmap(matrix=pymupdf.Matrix(icon / 24.0, icon / 24.0), alpha=True)
    ic = Image.frombytes('RGBA', (pix.width, pix.height), pix.samples)
    im.paste(ic, ((canvas - ic.width) // 2, (canvas - ic.height) // 2), ic)
    return im


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', APP, 'Images')
    if APP == 'OSMapsCE':
        Image.new('RGB', (800, 480), INK).save(os.path.join(out, 'MapGL.png'))   # covered by the map
        n = 1
        # the round buttons: 28 px icons on 48; the bottom bar: 28 px on 44 (as VestaHome's bar); small: 28 on 32
        for group, canvas, muted in ((LUCIDE_SIGNS, 48, False), (LUCIDE_DOCK, 44, True), (LUCIDE_SMALL, 32, False)):
            for name, spec in group.items():
                for pressed in (False, True):
                    c = INK if pressed else ORANGE if spec[1] else MUTED if muted or name == 'Bgps_none' else TEXT
                    lucide_image(spec, c, canvas, 28).save(
                        os.path.join(out, '%s_%s.png' % (name, 'P' if pressed else 'N')))
                    n += 1
        print(out, n, 'files (Lucide)')
        return
    if APP == 'OSMapsCE_old':
        Image.new('RGB', (800, 480), INK).save(os.path.join(out, 'MapGL.png'))   # covered by the map
        n = 1
        for group, size in ((SIGNS, SIGN_SIZE), (DOCK, DOCK_SIZE), (SMALL, SMALL_SIZE)):
            for name, sg, on in group:
                for pressed in (False, True):
                    sign_image(sg, on, pressed, size, group is DOCK).save(
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
