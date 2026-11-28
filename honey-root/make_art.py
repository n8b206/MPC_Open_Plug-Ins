"""Artwork for Honey Root: the PLAY panel (with the chord-key map), the browser tile, and a mock-up.
    python make_art.py <skins dir> <tile path> [mockup path]
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

INK, HONEY, CREAM, TAN, BROWN = (40, 29, 19), (230, 177, 75), (243, 231, 208), (176, 140, 92), (129, 72, 30)
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SERIF = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"
SS = 2   # supersample

def font(size, serif=False): return ImageFont.truetype(SERIF if serif else BOLD, int(size * SS))

def hexagon(d, cx, cy, r, fill, outline=None, width=1):
    import math
    pts = [(cx + r * math.cos(math.radians(60 * i + 30)), cy + r * math.sin(math.radians(60 * i + 30))) for i in range(6)]
    d.polygon(pts, fill=fill, outline=outline)
    if outline and width > 1: d.line(pts + [pts[0]], fill=outline, width=width)

def centred(d, box, text, f, fill):
    x0, y0, x1, y1 = box
    w, h = d.textbbox((0, 0), text, font=f)[2:]
    d.text(((x0 + x1 - w) / 2, (y0 + y1 - h) / 2 - f.size * 0.08), text, font=f, fill=fill)

def panel(path, W=918, H=560):
    im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, W * SS - 1, H * SS - 1), radius=26 * SS, fill=INK + (255,), outline=HONEY + (255,), width=4 * SS)
    # faint honeycomb in the top right
    for i, (cx, cy) in enumerate([(840, 70), (880, 116), (800, 116), (840, 162), (760, 70)]):
        hexagon(d, cx * SS, cy * SS, 26 * SS, (60, 44, 28, 255), (90, 66, 40, 255))
    d.line((30 * SS, 300 * SS, (W - 30) * SS, 300 * SS), fill=(90, 66, 40, 255), width=2 * SS)
    # the chord-key map: one octave from Block Start (F) as drawn on a 37-key board
    top, kh = 318, 210
    names = ["F", "G", "A", "B", "C", "D", "E", "F", "G", "A"]
    jobs = {"F": "6", "G": "m7", "A": "M7", "B": "9"}
    x0, kw = 40, 70
    for i, n in enumerate(names):
        x = x0 + i * kw
        chord = i < 4
        d.rounded_rectangle((x * SS, top * SS, (x + kw - 6) * SS, (top + kh - 40) * SS), radius=8 * SS,
                            fill=(CREAM if chord else (214, 196, 160)) + (255,), outline=BROWN + (255,), width=2 * SS)
        if chord: centred(d, (x * SS, (top + 112) * SS, (x + kw - 6) * SS, (top + 160) * SS), jobs[n], font(24), BROWN)
    blacks = [(0, "DIM"), (1, "MIN"), (2, "MAJ"), (4, None), (5, None), (7, None), (8, None)]
    for i, job in blacks:
        x = x0 + i * kw + kw * 0.62
        d.rounded_rectangle((x * SS, top * SS, (x + kw * 0.66) * SS, (top + 96) * SS), radius=6 * SS,
                            fill=(HONEY if job else (70, 52, 34)) + (255,), outline=INK + (255,), width=2 * SS)
        if job: centred(d, (x * SS, (top + 54) * SS, (x + kw * 0.66) * SS, (top + 90) * SS), job, font(15), INK)
    d.text((40 * SS, (top + kh - 30) * SS), "CHORD KEYS", font=font(20), fill=HONEY)
    d.text(((x0 + 4 * kw + 4) * SS, (top + kh - 30) * SS), "ROOTS  →", font=font(20), fill=CREAM)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 8) * SS), "MIN + MAJ", font=font(18), fill=HONEY)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 34) * SS), "= SUS", font=font(18), fill=CREAM)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 76) * SS), "Keys layout,", font=font(16), fill=TAN)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 98) * SS), "Block Start F", font=font(16), fill=TAN)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 132) * SS), "Pads: A01-A08", font=font(16), fill=TAN)
    d.text(((x0 + 10 * kw + 14) * SS, (top + 154) * SS), "= chord keys", font=font(16), fill=TAN)
    im.resize((W, H), Image.LANCZOS).save(path)

def tile(path, W=270, H=110):
    im = Image.new("RGBA", (W * SS, H * SS), CREAM + (255,))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((2 * SS, 2 * SS, (W - 3) * SS, (H - 3) * SS), radius=14 * SS, outline=HONEY + (255,), width=3 * SS)
    hexagon(d, 52 * SS, 55 * SS, 38 * SS, HONEY + (255,), INK + (255,), 3 * SS)
    # a root growing into a chord: three dots stacked on a stem
    d.line((52 * SS, 84 * SS, 52 * SS, 40 * SS), fill=INK + (255,), width=4 * SS)
    for y, x in ((40, 52), (52, 40), (52, 64), (64, 52)):
        d.ellipse(((x - 6) * SS, (y - 6) * SS, (x + 6) * SS, (y + 6) * SS), fill=INK + (255,))
    d.text((102 * SS, 22 * SS), "Honey Root", font=font(25, serif=True), fill=INK)
    d.text((104 * SS, 62 * SS), "chord machine  ·  N8b", font=font(13), fill=BROWN)
    im.resize((W, H), Image.LANCZOS).convert("RGB").save(path)

def mockup(panel_path, path, name="Fmaj7", notes="F3  A3  C4  E4", status="Key C Major   |   Lock   |   Arp   |   Bass"):
    W, H = 1280, 640
    im = Image.new("RGB", (W * SS, H * SS), CREAM)
    d = ImageDraw.Draw(im)
    p = Image.open(panel_path).resize((918 * SS, 560 * SS), Image.LANCZOS)
    im.paste(p, (343 * SS, 45 * SS), p)
    centred(d, (343 * SS, 55 * SS, 1261 * SS, 100 * SS), status, font(22), CREAM)
    centred(d, (343 * SS, 100 * SS, 1261 * SS, 245 * SS), name, font(104), HONEY)
    centred(d, (343 * SS, 245 * SS, 1261 * SS, 295 * SS), notes, font(30), CREAM)
    for i, (lab, val) in enumerate([("Voicing", "+0"), ("Perform", "Arp"), ("Bass Level", "70%"), ("Reverb", "20%")]):
        y = 45 + i * 151
        d.rounded_rectangle((23 * SS, y * SS, 301 * SS, (y + 107) * SS), radius=12 * SS, fill=(235, 220, 190), outline=TAN, width=2 * SS)
        d.text((40 * SS, (y + 14) * SS), lab, font=font(20), fill=INK)
        d.text((40 * SS, (y + 58) * SS), val, font=font(26), fill=BROWN)
        d.ellipse((215 * SS, (y + 20) * SS, 280 * SS, (y + 85) * SS), outline=HONEY, width=6 * SS)
    tabs = ["PLAY", "CHORD", "PERFORM", "BASS", "SOUND", "FX"]
    for i, t in enumerate(tabs):
        x = 20 + i * 206
        d.rounded_rectangle((x * SS, 612 * SS, (x + 196) * SS, 638 * SS), radius=6 * SS, fill=HONEY if i == 0 else TAN)
        centred(d, (x * SS, 612 * SS, (x + 196) * SS, 638 * SS), t, font(16), INK)
    im.resize((W, H), Image.LANCZOS).save(path)

if __name__ == "__main__":
    skins, tile_path = sys.argv[1], sys.argv[2]
    panel(os.path.join(skins, "hr_panel.png"))
    tile(tile_path)
    if len(sys.argv) > 3:
        mockup(os.path.join(skins, "hr_panel.png"), sys.argv[3])
