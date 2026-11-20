"""Post-build step for Chord Bloom. Run after tools/build.py and before tools/package.py:
    python plugins/ChordBloom/post_build.py

1. PERFORM: the bee pad. A big film-strip knob you drag up and down (Bee Fly) shows the DJ bee flying
   from its turntable up to the hive; Bee Mode buttons pick what flying does (Riser / Dive / Space / Wub)
   and Bee Hold keeps it up when you let go (otherwise it drops back when your finger lifts).
2. PADS: a 4x4 grid laid out like pad bank A. The top two rows show the chords of your key
   (live names, tap to play one for a beat, they light while a pad plays them); the third row
   toggles the extensions; the bottom row shows the triad pads you hold on the hardware.
3. Recolours the kit graphics to the musicwithmrb.com light theme (cream, tan, honey).
4. Program-mode Q-Links: the performance FX first.
5. The Instruments-browser tile: Plugin Skins/browser_images/soundsmode.png (270x110).
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ["Filter", "Delay", "Reverb", "Drive", "Bee Fly", "Feedback", "Room Size", "Chorus",
         "Delay Time", "Verb Tone", "Brightness", "Rotor Speed", "Rotary", "Release", "Piano Blend", "Volume"]

params = json.load(open(os.path.join(HERE, "build", "params.json")))
index = {p["name"]: p["index"] for p in params["params"]}
out = [d for d in os.listdir(os.path.join(HERE, "build")) if " - VST - " in d][0]
skins = os.path.join(HERE, "build", out, "Plugin Skins")
tile_path = os.path.join(skins, "browser_images", "soundsmode.png")
os.makedirs(os.path.dirname(tile_path), exist_ok=True)

for f in os.listdir(skins):   # art from earlier versions
    if f.startswith(("bee_", "btn_", "pad_")):
        os.remove(os.path.join(skins, f))
subprocess.check_call([sys.executable, os.path.join(HERE, "make_bee.py"), skins, tile_path], stdout=subprocess.DEVNULL)

P = lambda name: "Parameter %d" % index[name]
INK, MUTED = "ff281d13", "ff81481e"

def entry(name, ctype, data, rect, focus="No", show_if=None):
    b = {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
         "boundsType": "Absolute", "bounds": "%d %d %d %d" % rect}
    if show_if:
        b = {"version": 2, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Hide", "whenVisible": "Always",
             "boundsType": "Absolute", "bounds": "%d %d %d %d" % rect, "additionalInvalidatingHandles": [show_if]}
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": data},
            "handle remapping": {"version": 1, "map": []}, "bounds": b}

def image(name, img, rect, frames=None, handle=None, animated=False):
    if frames is None:
        return entry(name, "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": img}, rect)
    d = {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": img, "numFrames": frames, "handleName": handle}
    if animated: d["animated"] = True
    return entry(name, "Image", d, rect)

def label(name, rect, size, colour, kind="Text", handle="Invalid", text=None, just="horizontallyCentred verticallyCentred", style="Bold"):
    d = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style, "height": float(size)},
                                     "colour": colour, "justification": just, "case": "Original"},
         "type": kind, "handleName": handle}
    if text is not None: d["text"] = text
    return entry(name, "Label", d, rect)

def button(name, on, off, rect, handle, bid, n):
    return entry(name, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": bid, "numButtonsInGroup": n,
                                  "handleName": handle, "gestureBehaviour": "Instant"}, rect, focus="Yes")

tui_path = os.path.join(skins, "TUI.json")
tui = json.load(open(tui_path))
tabs = {t["tabName"]: t for t in tui["pageData"]["tabs"]}

# ---- 1. PERFORM: the bee pad (tab coordinates; the panel covers columns 2-4)
PX, PY = 343, 45
bee = [image("bee panel", "bee_panel.png", (PX, PY, 918, 560)),
       image("bee deck", "bee_deck.png", (PX, PY + 350, 278, 200), 8, P("Volume"), animated=True)]
fly = P("Bee Fly")
for hold, handle in ((0, "ResetOnGestureEnd/" + fly), (1, fly)):   # let go = land, unless Bee Hold is on
    bee.append(entry("bee fly %d" % hold, "Knob",
                     {"version": 3, "knobType": "FilmStrip", "filmStrip": "bee_fly.png", "numFrames": 17, "invert": False,
                      "dragOrientation": "Vertical", "handleName": handle},
                     (PX + 240, PY, 440, 560), focus="Yes", show_if="IndexedEnabling/%d/2/%s" % (hold, P("Bee Hold"))))
for i, m in enumerate(("riser", "dive", "space", "wub")):
    bee.append(button("bee mode %s" % m, "btn_%s_on.png" % m, "btn_%s_off.png" % m, (PX + 18, PY + 48 + 58 * i, 200, 52), P("Bee Mode"), i, 4))
bee.append(button("bee hold", "btn_hold_on.png", "btn_hold_off.png", (PX + 18, PY + 48 + 58 * 4 + 12, 200, 52), P("Bee Hold"), 1, 1))
bee.append(label("bee mode value", (PX + 710, PY + 374, 200, 40), 30, INK, "Value", P("Bee Mode"), just="left verticallyCentred"))
bee.append(label("bee fly value", (PX + 710, PY + 454, 200, 40), 30, INK, "Value", fly, just="left verticallyCentred"))
comps = tabs["PERFORM"]["componentDefinition"]["componentsData"]
comps[:] = [c for c in comps if not c["componentData"]["name"].startswith("bee ")]
comps[0:0] = bee

# ---- 2. PADS: the 4x4 chord grid, laid out like pad bank A (A01 bottom left)
pads = [button("pad learn", "btn_learn_on.png", "btn_learn_off.png", (23, 5, 180, 36), P("Pad Learn"), 1, 1),
        label("pad zone", (215, 4, 722, 38), 24, INK, "Value", P("Pad Zone"), just="right verticallyCentred")]
XS = [23 + c * 234 for c in range(4)]
YS = [45 + r * 136 for r in range(4)]
W, H = 216, 124
for c in range(4):
    # top two rows: chord pads (scale degrees 5-8 on top, 1-4 below); the chord name is live text
    for r, deg in ((0, 5 + c), (1, 1 + c)):
        x, y = XS[c], YS[r]
        pads += [image("pad base %d" % deg, "pad_chord_%d.png" % deg, (x, y, W, H)),
                 label("pad name %d" % deg, (x, y + 14, W, 70), 44, INK, "Value", P("Pad Chord %d" % deg)),
                 button("pad tap %d" % deg, "pad_lit.png", "pad_off.png", (x, y, W, H), P("Pad Lit"), deg, 9)]
    # third row: add pads (tap = the CHORD page switches; light up while held on the MPC)
    x, y = XS[c], YS[2]
    ext = ["Add 9", "Add maj7", "Add m7", "Add 6"][c]
    pads += [image("pad base add %d" % c, "pad_add_%d.png" % c, (x, y, W, H)),
             image("pad held add %d" % c, "pad_held.png", (x, y, W, H), 2, P("Pad Held %d" % (5 + c))),
             button("pad add tap %d" % c, "pad_add_on.png", "pad_off.png", (x, y, W, H), P(ext), 1, 1)]
    # bottom row: chord type pads (hold on the hardware)
    x, y = XS[c], YS[3]
    pads += [image("pad base tri %d" % c, "pad_tri_%d.png" % c, (x, y, W, H)),
             image("pad held tri %d" % c, "pad_held.png", (x, y, W, H), 2, P("Pad Held %d" % (1 + c)))]
comps = tabs["PADS"]["componentDefinition"]["componentsData"]
comps[:] = [c for c in comps if not c["componentData"]["name"].startswith("pad ")]
comps[0:0] = pads
json.dump(tui, open(tui_path, "w"), indent=1)

# ---- 3. light theme: warm the kit's dark greys into cream/tan so the dark-brown text reads on them
from PIL import Image
TAN, CREAM = (176, 140, 92), (254, 248, 236)
KIT = ("knob", "fader", "seg", "toggle", "button")
for f in os.listdir(skins):
    if not f.endswith(".png") or not f.startswith(KIT):
        continue
    path = os.path.join(skins, f)
    im = Image.open(path).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0 or max(r, g, b) - min(r, g, b) > 24:
                continue
            if max(r, g, b) > 140:
                if f.startswith("knob") and min(r, g, b) > 180:   # white pointer -> dark brown
                    px[x, y] = (40, 29, 19, a)
                continue
            t = min(1.0, max(0.0, ((r + g + b) / 3 - 25) / 45.0))
            px[x, y] = tuple(int(TAN[i] + (CREAM[i] - TAN[i]) * t) for i in range(3)) + (a,)
    im.save(path)

# ---- 4. Program-mode Q-Links
qpath = os.path.join(skins, "Q-Links.json")
q = json.load(open(qpath))
q["Program Mode Q-Links"] = {"Q-Link %d" % (i + 1): index[n] for i, n in enumerate(ORDER)}
json.dump(q, open(qpath, "w"), indent=2)

# ---- film strips must stay under 16384 px tall (the MPC resamples taller ones and they drift)
for f in os.listdir(skins):
    if f.endswith(".png"):
        h = Image.open(os.path.join(skins, f)).height
        if h > 16384: sys.exit("%s is %d px tall; the MPC limit is 16384" % (f, h))
print("bee pad + PADS grid + tile added; Program-mode Q-Links ->", ", ".join(ORDER))
