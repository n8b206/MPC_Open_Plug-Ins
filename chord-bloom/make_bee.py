"""Draws the Chord Bloom DJ bee as film strips for the PERFORM page (Pillow).

Styled after musicwithmrb.com (light theme): cream panel, honeycomb with dark-brown
outlines, a side-on bee with a black head, yellow/black stripes, pale-blue wings and a
white sticker outline. This bee wears headphones and scratches a turntable.

PERFORM page (v2): one big "bee pad". bee_panel.png is the backdrop; bee_fly.png (440x560, 17 frames)
is a film-strip KNOB you drag up and down (Bee Fly): the bee rises from the turntable to the hive, gets
hyped, grows honey rings and trails echo bees. bee_deck.png (278x200, 8 frames, animated) scratches.
Also draws the Bee Mode buttons, the PADS page tiles and the 270x110 Instruments-browser tiles.

Building blocks below (v1 layers, still used to draw the bee):
  bee_bg.png      column 278x560, static        - cream panel, honeycomb, dashed flight path
  bee_reverb.png  column 278x560, 8 frames      - sound rings around the bee            (Reverb)
  bee_delay.png   column 278x560, 5 frames      - 0..4 echo bees along the flight path   (Delay)
  bee_deck.png    278x200, 8 frames, animated   - turntable: record scratching back and forth + the bee's arm
  bee_wings.png   220x220, 6 frames, animated   - flapping wings
  bee_body.png    220x220, 16 frames            - body + headphones; yellow -> orange -> red, brows, steam (Drive)
  bee_face.png    220x220, 17 frames            - sleepy (low-pass) .. grin .. hyped (high-pass)     (Filter)
"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1] if len(sys.argv) > 1 else "bee"
os.makedirs(OUT, exist_ok=True)
SS = 3
COL_W, COL_H = 278, 560
BEE = 220
BEE_X, BEE_Y = 22, 200            # bee box inside the column
DECK_Y, DECK_H = 350, 200          # deck strip inside the column

CREAM = (243, 231, 208); CREAM2 = (250, 242, 226); WHITE = (254, 250, 241)
GREEN = (74, 150, 78); MINT = (222, 240, 214); DEEPGREEN = (40, 104, 48)
INK = (40, 29, 19); BROWN = (129, 72, 30); HONEY = (230, 177, 75); PALE = (242, 221, 167)
YELLOW = (246, 196, 60); WING = (214, 234, 246); WING_EDGE = (60, 70, 80)

def lerp(a, b, t): return a + (b - a) * t
def lerpc(a, b, t): return tuple(int(round(lerp(x, y, t))) for x, y in zip(a, b))
def canvas(w, h): return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
def down(im, w, h): return im.resize((w, h), Image.LANCZOS)
def S(*v): return [x * SS for x in v]
def A(c, a=255): return tuple(c[:3]) + (a,)

def sticker(im, px=5):
    """White sticker outline behind everything drawn so far (like the site's bee)."""
    a = im.split()[3].point(lambda p: 255 if p > 20 else 0)
    grown = a.filter(ImageFilter.MaxFilter(px * SS * 2 + 1)).filter(ImageFilter.GaussianBlur(SS))
    base = Image.composite(Image.new("RGBA", im.size, A(WHITE)), Image.new("RGBA", im.size, (0, 0, 0, 0)), grown)
    base.alpha_composite(im)
    return base

def strip(frames, w, h, name):
    out = Image.new("RGBA", (w, h * len(frames)), (0, 0, 0, 0))
    for i, f in enumerate(frames): out.paste(f, (0, i * h))
    out.save(os.path.join(OUT, name), optimize=True)

# ---------------------------------------------------------------- bee geometry (in the 220 box)
HX, HY, HR = 62, 104, 33           # head
BX, BY, RX, RY = 130, 116, 58, 43  # body (abdomen)

def body(t):
    """Drive 0..1: colour heats up, brows get angry, steam from the headphones."""
    im = canvas(BEE, BEE); d = ImageDraw.Draw(im)
    col = lerpc(YELLOW, (247, 128, 40), min(t / 0.6, 1)) if t <= 0.6 else lerpc((247, 128, 40), (226, 52, 44), (t - 0.6) / 0.4)
    d.polygon(S(BX + RX - 4, BY - 7, BX + RX + 16 + 8 * t, BY, BX + RX - 4, BY + 7), fill=A(INK))   # stinger
    for lx in (-22, 0, 22):                                                                         # legs
        d.line(S(BX + lx, BY + RY - 6, BX + lx - 6, BY + RY + 14), fill=A(INK), width=4 * SS)
    d.ellipse(S(BX - RX, BY - RY, BX + RX, BY + RY), fill=A(col))
    stripes = canvas(BEE, BEE); sd = ImageDraw.Draw(stripes)
    for sx in (-14, 14, 40):
        sd.rectangle(S(BX + sx, BY - RY - 5, BX + sx + 13, BY + RY + 5), fill=A(INK))
    mask = canvas(BEE, BEE); ImageDraw.Draw(mask).ellipse(S(BX - RX, BY - RY, BX + RX, BY + RY), fill=(255, 255, 255, 255))
    im.alpha_composite(Image.composite(stripes, Image.new("RGBA", im.size, (0, 0, 0, 0)), mask))
    d = ImageDraw.Draw(im)
    d.ellipse(S(BX - RX, BY - RY, BX + RX, BY + RY), outline=A(INK), width=4 * SS)
    d.ellipse(S(BX - 34, BY - 30, BX - 14, BY - 20), fill=(255, 255, 255, 150))                     # shine
    d.ellipse(S(HX - HR, HY - HR, HX + HR, HY + HR), fill=A(INK))                                   # head
    # headphones: band over the head, cup on the back of the head
    d.arc(S(HX - HR - 6, HY - HR - 12, HX + HR + 6, HY + HR + 4), 200, 340, fill=A(BROWN), width=7 * SS)
    d.rounded_rectangle(S(HX + 6, HY - 20, HX + 30, HY + 16), 9 * SS, fill=A(HONEY), outline=A(INK), width=3 * SS)
    d.ellipse(S(HX + 12, HY - 10, HX + 24, HY + 6), fill=A(BROWN))
    if t > 0.25:                                                                                    # angry brow
        a = (t - 0.25) / 0.75
        d.line(S(HX - 26, HY - 22 + 7 * a, HX - 8, HY - 22 - 3 * a), fill=A(WHITE), width=4 * SS)
    if t > 0.6:                                                                                     # steam
        a = (t - 0.6) / 0.4
        for i, (px, py) in enumerate(((24, -40), (36, -58), (20, -70))):
            r = 5 + 7 * a * (1 + i * 0.2)
            d.ellipse(S(HX + px - r, HY + py - r, HX + px + r, HY + py + r), fill=(215, 210, 205, int(210 * a)))
    return down(sticker(im), BEE, BEE)

def face(v):
    """Filter -1..1: sleepy with Zzz on a closed low-pass, grin in the middle, hyped on high-pass."""
    im = canvas(BEE, BEE); d = ImageDraw.Draw(im)
    ex, ey = HX - 14, HY - 8
    for ax, lean in ((-16, -1), (-2, 1)):                       # antennae: droop .. perky
        bx, by = HX + ax, HY - HR + 4
        tipx = bx + lean * 8 - 10 + 4 * v
        tipy = by - 26 - 12 * v
        d.line(S(bx, by, tipx, tipy), fill=A(INK), width=4 * SS)
        d.ellipse(S(tipx - 6, tipy - 6, tipx + 6, tipy + 6), fill=A(INK))
    if v < -0.15:
        k = min((-v - 0.15) / 0.85, 1)
        if k > 0.45:
            d.arc(S(ex - 9, ey - 6, ex + 9, ey + 8), 10, 170, fill=A(WHITE), width=4 * SS)
        else:
            d.chord(S(ex - 9, ey - 8, ex + 9, ey + 10), 0, 180, fill=A(WHITE))
            d.ellipse(S(ex - 5, ey + 1, ex + 1, ey + 7), fill=A(INK))
        d.arc(S(ex - 14, ey + 12, ex + 2, ey + 22), 20, 160, fill=A(WHITE), width=3 * SS)
        if k > 0.4:
            for i in range(1 + int(k * 2.5)):
                zx, zy, zs = HX - 40 - i * 4, HY - 52 - i * 18, 8 + i * 3
                d.line(S(zx, zy, zx + zs, zy, zx, zy + zs, zx + zs, zy + zs), fill=A(BROWN), width=3 * SS)
    else:
        k = max(v, 0)
        r = 8 + 5 * k
        d.ellipse(S(ex - r, ey - r, ex + r, ey + r), fill=A(WHITE))
        pr = 4.5 + 1.5 * k
        d.ellipse(S(ex - pr - 3, ey - pr + 1, ex + pr - 3, ey + pr + 1), fill=A(INK))
        d.ellipse(S(ex - 3, ey - 4, ex, ey - 1), fill=A(WHITE))
        if v > 0.3:   # hyped: open mouth + sparkles
            g = (v - 0.3) / 0.7
            d.chord(S(ex - 20, ey + 12 - 4 * g, ex + 4, ey + 24 + 6 * g), 0, 180, fill=A(WHITE))
            for sx, sy in ((-44, -40), (-52, -14)):
                s = 5 + 5 * k
                cx, cy = HX + sx, HY + sy
                d.polygon(S(cx, cy - s, cx + s * 0.3, cy - s * 0.3, cx + s, cy, cx + s * 0.3, cy + s * 0.3,
                            cx, cy + s, cx - s * 0.3, cy + s * 0.3, cx - s, cy, cx - s * 0.3, cy - s * 0.3), fill=A(HONEY))
        else:
            d.arc(S(ex - 16, ey + 6, ex + 4, ey + 24), 25, 155, fill=A(WHITE), width=3 * SS)
    return down(im, BEE, BEE)

def wings(phase):
    im = canvas(BEE, BEE); d = ImageDraw.Draw(im)
    a = math.sin(phase * 2 * math.pi)
    for ox, sc in ((14, 1.0), (38, 0.9)):
        h = (56 + 18 * a) * sc
        cx = BX - 30 + ox
        d.ellipse(S(cx - 20 * sc, BY - RY - h + 8, cx + 20 * sc, BY - RY + 14), fill=A(WING, 235), outline=A(WING_EDGE), width=3 * SS)
        d.line(S(cx, BY - RY - h + 20, cx + 2, BY - RY + 6), fill=A(WING_EDGE, 160), width=2 * SS)
    return down(sticker(im, 4), BEE, BEE)

def deck(phase, arm=True):
    """Turntable + the bee's arm scratching the record back and forth."""
    im = canvas(COL_W, DECK_H); d = ImageDraw.Draw(im)
    x0, y0, x1, y1 = 22, 96, 256, 190
    d.rounded_rectangle(S(x0, y0, x1, y1), 14 * SS, fill=A(INK))
    d.rounded_rectangle(S(x0 + 5, y0 + 5, x1 - 5, y1 - 5), 10 * SS, fill=A((70, 52, 36)))
    cx, cy, R = 118, 143, 40
    d.ellipse(S(cx - R, cy - R, cx + R, cy + R), fill=A((24, 18, 14)))
    for r in (34, 28, 22):
        d.ellipse(S(cx - r, cy - r, cx + r, cy + r), outline=A((60, 50, 42)), width=SS)
    d.ellipse(S(cx - 12, cy - 12, cx + 12, cy + 12), fill=A(HONEY))
    s = math.sin(phase * 2 * math.pi)
    ang = s * 0.9                                      # scratch: back and forth
    mx, my = cx + 8 * math.cos(ang - 1.2), cy + 8 * math.sin(ang - 1.2)
    d.ellipse(S(mx - 3, my - 3, mx + 3, my + 3), fill=A(INK))
    d.line(S(222, 110, 206, 162, 170, 168), fill=A((200, 190, 175)), width=4 * SS)   # tone arm
    d.ellipse(S(216, 104, 230, 118), fill=A((200, 190, 175)))
    d.rectangle(S(36, 114, 44, 174), fill=A((40, 30, 22)))                           # crossfader-ish slider
    fy = 122 + 36 * (0.5 + 0.5 * s)
    d.rectangle(S(32, fy, 48, fy + 9), fill=A(HONEY))
    hx, hy = cx - 22 + 14 * s, cy - 26                                               # the bee's arm + glove
    if arm: d.line(S(132, 0, 120, 40, hx + 4, hy), fill=A(INK), width=5 * SS)
    if arm: d.ellipse(S(hx - 8, hy - 7, hx + 8, hy + 7), fill=A(WHITE), outline=A(INK), width=2 * SS)
    return down(im, COL_W, DECK_H)

def flight_path():
    pts = []
    for i in range(60):
        t = i / 59
        pts.append((150 - 30 * t - 26 * math.sin(t * 7), BEE_Y + 70 - 170 * t))
    return pts

def delay(n):
    im = Image.new("RGBA", (COL_W, COL_H), (0, 0, 0, 0))
    spots = [(0.42, 0.55, 0.5), (0.62, 0.45, 0.36), (0.8, 0.36, 0.25), (0.96, 0.29, 0.16)]
    path = flight_path()
    ghost = body(0.0); ghost.alpha_composite(wings(0.25)); ghost.alpha_composite(face(0.0))
    for i in range(n):
        t, sc, al = spots[i]
        x, y = path[int(t * (len(path) - 1))]
        g = ghost.resize((int(BEE * sc), int(BEE * sc)), Image.LANCZOS)
        g.putalpha(g.split()[3].point(lambda p: int(p * al)))
        im.alpha_composite(g, (int(x - g.width / 2), int(y - g.height / 2)))
    return im

def reverb(t):
    im = canvas(COL_W, COL_H); d = ImageDraw.Draw(im)
    cx, cy = BEE_X + HX + 10, BEE_Y + HY
    for i in range(4):
        r = 70 + i * 26 + 30 * t
        a = int(170 * t * (1 - i / 4.5))
        if a <= 0: continue
        d.ellipse(S(cx - r, cy - r, cx + r, cy + r), outline=A(HONEY, a), width=int((3 + 3 * t) * SS))
    im = down(im, COL_W, COL_H)
    if t > 0: im = Image.alpha_composite(im.filter(ImageFilter.GaussianBlur(2 + 4 * t)), im)
    return im

def hexagon(d, x, y, r, fill):
    pts = [(x + r * math.cos(math.radians(60 * k)), y + r * math.sin(math.radians(60 * k))) for k in range(6)]
    d.polygon([(px * SS, py * SS) for px, py in pts], fill=A(fill), outline=A(INK), width=3 * SS)

def background():
    im = canvas(COL_W, COL_H); d = ImageDraw.Draw(im)
    d.rounded_rectangle(S(0, 0, COL_W - 1, COL_H - 1), 22 * SS, fill=A(CREAM2), outline=A(HONEY), width=3 * SS)
    for i in range(0, COL_H - 20, 7):                       # faint wood grain
        a = 18 + (i * 37) % 22
        d.line(S(12, i + 10, COL_W - 12, i + 10 + ((i * 13) % 5) - 2), fill=A((214, 196, 160), a), width=SS)
    r, ox, oy = 22, 92, 42                                  # honeycomb cluster (site motif)
    cells = [(0, 0, PALE), (1, 0, HONEY), (2, 0, WHITE), (-0.5, 1, WHITE), (0.5, 1, HONEY), (1.5, 1, HONEY),
             (2.5, 1, PALE), (0, 2, PALE), (1, 2, HONEY), (2, 2, WHITE)]
    for cx, cy, c in cells:
        hexagon(d, ox + cx * r * 1.5 * 1.15, oy + cy * r * 0.87 * 1.15, r, c)
    path = flight_path()                                    # dashed flight path
    for i in range(0, len(path) - 1, 2):
        (x0, y0), (x1, y1) = path[i], path[i + 1]
        d.line(S(x0, y0, x1, y1), fill=A(INK, 200), width=2 * SS)
    return down(im, COL_W, COL_H)

# ---------------------------------------------------------------- PERFORM v2: the bee pad
PANEL_W, PANEL_H = 918, 560          # the whole bee area (tab x 343..1261, y 45..605)
LANE_X, LANE_W = 240, 440            # the draggable lane inside it (stays left of the Q-Link sidebar)
FLY_FRAMES = 17
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"
FONT_SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
from PIL import ImageFont
def font(size, serif=True): return ImageFont.truetype(FONT if serif else FONT_SANS, size * SS)

def lane_pos(t):
    """Bee centre in lane coordinates for flight 0..1 (bottom, near the deck, up to the hive)."""
    return (LANE_W / 2 - 40 + 70 * t + 22 * math.sin(t * 5.5), 450 - 330 * t)

def bee_sprite(t):
    b = body(0.15 * t)
    b.alpha_composite(wings(0.15 + 0.5 * t))
    b.alpha_composite(face(-0.2 + 1.2 * t if t > 0.02 else 0.0))
    return b

def bee_fly(t):
    """One frame of the draggable bee: trail, honey rings, the bee itself."""
    im = Image.new("RGBA", (LANE_W, PANEL_H), (0, 0, 0, 0))
    x, y = lane_pos(t)
    rings = canvas(LANE_W, PANEL_H); d = ImageDraw.Draw(rings)
    for i in range(4):                                                   # honey rings grow as it rises
        r = 80 + i * 24 + 40 * t
        a = int(180 * t * (1 - i / 4.5))
        if a > 0:
            d.ellipse(S(x - r, y - r, x + r, y + r), outline=A(HONEY, a), width=int((3 + 3 * t) * SS))
    rings = down(rings, LANE_W, PANEL_H)
    if t > 0:
        im.alpha_composite(rings.filter(ImageFilter.GaussianBlur(2 + 4 * t)))
        im.alpha_composite(rings)
    ghost = bee_sprite(0.0)
    n = int(round(t * 4))
    for k in range(n, 0, -1):                                            # echo bees trail behind
        tt = max(t - k * 0.11, 0.0)
        gx, gy = lane_pos(tt)
        sc = 0.75 - 0.1 * k
        g = ghost.resize((int(BEE * sc), int(BEE * sc)), Image.LANCZOS)
        g.putalpha(g.split()[3].point(lambda p, al=0.55 - 0.1 * k: int(p * al)))
        im.alpha_composite(g, (int(gx - g.width / 2), int(gy - g.height / 2)))
    b = bee_sprite(t)
    im.alpha_composite(b, (int(x - BEE / 2 - 10), int(y - BEE / 2 - 6)))
    return im

def panel_bg():
    W, H = PANEL_W, PANEL_H
    im = canvas(W, H); d = ImageDraw.Draw(im)
    d.rounded_rectangle(S(0, 0, W - 1, H - 1), 22 * SS, fill=A(CREAM2), outline=A(HONEY), width=3 * SS)
    for i in range(0, H - 20, 7):
        a = 18 + (i * 37) % 22
        d.line(S(12, i + 10, W - 12, i + 10 + ((i * 13) % 5) - 2), fill=A((214, 196, 160), a), width=SS)
    # the lane: a soft honey track where the finger goes
    d.rounded_rectangle(S(LANE_X + 30, 18, LANE_X + LANE_W - 30, H - 18), 40 * SS, fill=A(PALE, 90), outline=A(HONEY, 160), width=2 * SS)
    for k in range(5):                                                   # height ticks 0..100
        ty = 450 - 330 * k / 4
        d.line(S(LANE_X + 34, ty, LANE_X + 52, ty), fill=A(BROWN, 150), width=2 * SS)
    # dashed flight path up the lane
    pts = [lane_pos(i / 80) for i in range(81)]
    for i in range(0, 80, 2):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        d.line(S(LANE_X + x0, y0, LANE_X + x1, y1), fill=A(INK, 170), width=2 * SS)
    # hive at the top right of the lane
    r, ox, oy = 22, LANE_X + LANE_W + 40, 44
    cells = [(0, 0, PALE), (1, 0, HONEY), (2, 0, WHITE), (-0.5, 1, WHITE), (0.5, 1, HONEY), (1.5, 1, HONEY),
             (2.5, 1, PALE), (0, 2, PALE), (1, 2, HONEY), (2, 2, WHITE)]
    for cx, cy, c in cells:
        hexagon(d, ox + cx * r * 1.5 * 1.15, oy + cy * r * 0.87 * 1.15, r, c)
    # hint, right side
    d.text(S(LANE_X + LANE_W + 30, 200), "Drag the bee", font=font(26), fill=A(INK))
    d.text(S(LANE_X + LANE_W + 30, 236), "up to fly", font=font(26), fill=A(INK))
    ax = LANE_X + LANE_W + 120
    d.line(S(ax, 330, ax, 282), fill=A(BROWN), width=4 * SS)
    d.polygon(S(ax - 12, 290, ax, 272, ax + 12, 290), fill=A(BROWN))
    d.text(S(LANE_X + LANE_W + 30, 352), "Mode", font=font(18, False), fill=A(BROWN))
    d.text(S(LANE_X + LANE_W + 30, 432), "Fly", font=font(18, False), fill=A(BROWN))
    d.text(S(18, 22), "BEE MODE", font=font(16, False), fill=A(BROWN))
    return down(im, W, H)

def button_img(text, on, w=200, h=52):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    if on:
        d.rounded_rectangle(S(2, 2, w - 2, h - 2), 14 * SS, fill=A(HONEY), outline=A(INK), width=3 * SS)
    else:
        d.rounded_rectangle(S(2, 2, w - 2, h - 2), 14 * SS, fill=A(WHITE), outline=A(BROWN, 200), width=2 * SS)
    f = font(22 if h >= 44 else 18)
    tw = d.textlength(text, font=f) / SS
    d.text(S((w - tw) / 2, h / 2 - (15 if h >= 44 else 12)), text, font=f, fill=A(INK))
    return down(im, w, h)

# ---------------------------------------------------------------- PADS page tiles
PAD_W, PAD_H = 216, 124
CARAMEL = (214, 176, 128); DARKBROWN = (96, 52, 20)
def pad_tile(kind, title=None, sub=None):
    """kind: chord (top 8: brown), add (extensions: cream), tri (triad pads: honey). Text is baked in,
    because static Text labels don't draw on the MPC."""
    im = canvas(PAD_W, PAD_H); d = ImageDraw.Draw(im)
    fill = {"chord": CARAMEL, "add": CREAM2, "tri": (244, 214, 150)}[kind]
    edge = {"chord": DARKBROWN, "add": BROWN, "tri": BROWN}[kind]
    d.rounded_rectangle(S(3, 5, PAD_W - 3, PAD_H - 1), 16 * SS, fill=A((170, 140, 100)))          # shadow
    d.rounded_rectangle(S(3, 2, PAD_W - 3, PAD_H - 5), 16 * SS, fill=A(fill), outline=A(edge, 240), width=3 * SS)
    if title:
        f = font(34); tw = d.textlength(title, font=f) / SS
        d.text(S((PAD_W - tw) / 2, 22), title, font=f, fill=A(INK))
    if sub:
        d.text(S(14, 88), sub, font=font(16, False), fill=A(INK))
    return down(im, PAD_W, PAD_H)

def pad_lit(alpha=90, colour=None):
    im = canvas(PAD_W, PAD_H); d = ImageDraw.Draw(im)
    d.rounded_rectangle(S(3, 2, PAD_W - 3, PAD_H - 5), 16 * SS, fill=A(colour or DARKBROWN, alpha), outline=A(INK), width=6 * SS)
    return down(im, PAD_W, PAD_H)

def blank(w, h): return Image.new("RGBA", (w, h), (0, 0, 0, 0))

# ---------------------------------------------------------------- Instruments-browser tiles (270x110)
def tile(title, sub, mic=False):
    W, H = 270, 110
    im = canvas(W, H); d = ImageDraw.Draw(im)
    for y in range(H):                                                   # honey -> cream
        d.line(S(0, y, W, y), fill=A(lerpc(HONEY, CREAM2, y / H)), width=SS)
    r = 15
    for cx in range(-1, 10):
        for cy in range(0, 5):
            x = 150 + cx * r * 1.5 * 1.15
            yy = cy * r * 0.87 * 2.3 + (cx % 2) * r * 0.87 * 1.15
            pts = [(x + r * math.cos(math.radians(60 * k)), yy + r * math.sin(math.radians(60 * k))) for k in range(6)]
            d.polygon([(px * SS, py * SS) for px, py in pts], outline=A(WHITE, 110), width=2 * SS)
    d.text(S(12, 18), title[0], font=font(26), fill=A(INK))
    d.text(S(12, 50), title[1], font=font(26), fill=A(INK))
    d.text(S(14, 86), sub, font=font(12, False), fill=A(BROWN))
    im = down(im, W, H)
    b = bee_sprite(0.7 if not mic else 0.35).resize((118, 118), Image.LANCZOS)
    im.alpha_composite(b, (160, -4))
    if mic:                                                              # a little microphone in front of the bee
        m = canvas(60, 90); md = ImageDraw.Draw(m)
        md.rounded_rectangle(S(18, 6, 42, 46), 12 * SS, fill=A((70, 70, 76)), outline=A(INK), width=2 * SS)
        for yy in range(12, 44, 6): md.line(S(20, yy, 40, yy), fill=A((140, 140, 150)), width=SS)
        md.line(S(30, 46, 30, 76), fill=A(INK), width=4 * SS)
        md.line(S(18, 78, 42, 78), fill=A(INK), width=4 * SS)
        m = down(sticker(m, 3), 60, 90)
        im.alpha_composite(m, (150, 22))
    return im

if __name__ == "__main__":
    strip([bee_fly(i / (FLY_FRAMES - 1)) for i in range(FLY_FRAMES)], LANE_W, PANEL_H, "bee_fly.png")
    panel_bg().save(os.path.join(OUT, "bee_panel.png"), optimize=True)
    strip([deck(i / 8, arm=False) for i in range(8)], COL_W, DECK_H, "bee_deck.png")
    for name in ("Riser", "Dive", "Space", "Wub", "Hold"):
        button_img(name, True).save(os.path.join(OUT, "btn_%s_on.png" % name.lower()), optimize=True)
        button_img(name, False).save(os.path.join(OUT, "btn_%s_off.png" % name.lower()), optimize=True)
    roman = ["I", "II", "III", "IV", "V", "VI", "VII", "I+"]
    for deg in range(1, 9):
        pad_tile("chord", sub="A%02d  \u00b7  %s" % (8 + deg, roman[deg - 1])).save(os.path.join(OUT, "pad_chord_%d.png" % deg), optimize=True)
    for c, t in enumerate(["add 9", "add maj7", "add m7", "add 6"]):
        pad_tile("add", t, "A%02d  \u00b7  hold" % (5 + c)).save(os.path.join(OUT, "pad_add_%d.png" % c), optimize=True)
    for c, t in enumerate(["Maj", "Sus4", "Min", "Dim"]):
        pad_tile("tri", t, "A%02d  \u00b7  hold" % (1 + c)).save(os.path.join(OUT, "pad_tri_%d.png" % c), optimize=True)
    pad_lit().save(os.path.join(OUT, "pad_lit.png"), optimize=True)
    pad_lit(70, HONEY).save(os.path.join(OUT, "pad_add_on.png"), optimize=True)
    strip([blank(PAD_W, PAD_H), pad_lit(45, DARKBROWN)], PAD_W, PAD_H, "pad_held.png")   # lit while held on the MPC
    blank(PAD_W, PAD_H).save(os.path.join(OUT, "pad_off.png"), optimize=True)
    button_img("Pad Learn", True, 180, 36).save(os.path.join(OUT, "btn_learn_on.png"), optimize=True)
    button_img("Pad Learn", False, 180, 36).save(os.path.join(OUT, "btn_learn_off.png"), optimize=True)
    if len(sys.argv) > 2:   # browser tiles go to their own folders
        tile(("Chord", "Bloom"), "N8b  ·  chords on the pads").save(sys.argv[2], optimize=True)
    if len(sys.argv) > 3:
        tile(("Bloom", "Voice"), "N8b  ·  sing the chords", mic=True).save(sys.argv[3], optimize=True)
    print("ok")
