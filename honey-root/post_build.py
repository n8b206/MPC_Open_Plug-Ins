"""Post-build step for Honey Root. Run after tools/build.py and before tools/package.py:
    python "plugins/Honey Root/post_build.py"

1. PLAY: a big chord display (dark honey panel) beside the four knobs: the status line, the chord
   name and its notes are live Value labels; the chord-key map is drawn into the panel.
2. Recolours the kit graphics to the musicwithmrb.com light theme (cream, tan, honey), like Chord Bloom.
3. Program-mode Q-Links.
4. The Instruments-browser tile: Plugin Skins/browser_images/soundsmode.png (270x110).
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ["Voicing", "Perform", "Perf Div", "Perf Rate", "Bass Level", "Bass Tone", "Bass Register", "Bass Perform",
         "Brightness", "Env Amount", "Tone", "Release", "Chorus", "Delay", "Reverb", "Volume"]

params = json.load(open(os.path.join(HERE, "build", "params.json")))
index = {p["name"]: p["index"] for p in params["params"]}
out = [d for d in os.listdir(os.path.join(HERE, "build")) if " - VST - " in d][0]
skins = os.path.join(HERE, "build", out, "Plugin Skins")
tile_path = os.path.join(skins, "browser_images", "soundsmode.png")
os.makedirs(os.path.dirname(tile_path), exist_ok=True)
subprocess.check_call([sys.executable, os.path.join(HERE, "make_art.py"), skins, tile_path])

P = lambda name: "Parameter %d" % index[name]
HONEY, CREAM = "ffe6b14b", "fff3e7d0"

def entry(name, ctype, data, rect):
    b = {"version": 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
         "boundsType": "Absolute", "bounds": "%d %d %d %d" % rect}
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": data},
            "handle remapping": {"version": 1, "map": []}, "bounds": b}

def image(name, img, rect):
    return entry(name, "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": img}, rect)

def value_label(name, rect, size, colour, handle):
    d = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Bold", "height": float(size)},
                                     "colour": colour, "justification": "horizontallyCentred verticallyCentred", "case": "Original"},
         "type": "Value", "handleName": handle}
    return entry(name, "Label", d, rect)

tui_path = os.path.join(skins, "TUI.json")
tui = json.load(open(tui_path))
tabs = {t["tabName"]: t for t in tui["pageData"]["tabs"]}

# ---- 1. PLAY: the chord display (tab coordinates; the panel covers columns 2-4)
PX, PY, PW = 343, 45, 918
play = [image("hr panel", "hr_panel.png", (PX, PY, PW, 560)),
        value_label("hr status", (PX + 20, PY + 10, PW - 40, 48), 26, CREAM, P("Status")),
        value_label("hr chord", (PX + 20, PY + 60, PW - 40, 150), 104, HONEY, P("Chord Name")),
        value_label("hr notes", (PX + 20, PY + 205, PW - 40, 52), 32, CREAM, P("Chord Notes"))]
comps = tabs["PLAY"]["componentDefinition"]["componentsData"]
comps[:] = [c for c in comps if not c["componentData"]["name"].startswith("hr ")]
comps[0:0] = play
json.dump(tui, open(tui_path, "w"), indent=1)

# ---- 2. light theme: warm the kit's dark greys into cream/tan so the dark-brown text reads on them
from PIL import Image
TAN, LIGHT = (176, 140, 92), (254, 248, 236)
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
            px[x, y] = tuple(int(TAN[i] + (LIGHT[i] - TAN[i]) * t) for i in range(3)) + (a,)
    im.save(path)

# ---- 3. Program-mode Q-Links
qpath = os.path.join(skins, "Q-Links.json")
q = json.load(open(qpath))
q["Program Mode Q-Links"] = {"Q-Link %d" % (i + 1): index[n] for i, n in enumerate(ORDER)}
json.dump(q, open(qpath, "w"), indent=2)

for f in os.listdir(skins):
    if f.endswith(".png"):
        h = Image.open(os.path.join(skins, f)).height
        if h > 16384: sys.exit("%s is %d px tall; the MPC limit is 16384" % (f, h))
print("PLAY display + light theme + tile added; Program-mode Q-Links ->", ", ".join(ORDER))
