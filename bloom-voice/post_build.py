"""Post-build step for Bloom Voice (after tools/build.py, before tools/package.py):
the Instruments/effects browser tile and the musicwithmrb.com light-theme kit colours.
Uses ../ChordBloom/make_bee.py for the artwork."""
import os, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
out = [d for d in os.listdir(os.path.join(HERE, "build")) if " - VST - " in d][0]
skins = os.path.join(HERE, "build", out, "Plugin Skins")
tile = os.path.join(skins, "browser_images", "soundsmode.png")
os.makedirs(os.path.dirname(tile), exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    subprocess.check_call([sys.executable, os.path.join(HERE, "..", "ChordBloom", "make_bee.py"), tmp,
                           os.path.join(tmp, "unused.png"), tile], stdout=subprocess.DEVNULL)

TAN, CREAM = (176, 140, 92), (254, 248, 236)
for f in os.listdir(skins):
    if not f.endswith(".png") or not f.startswith(("knob", "fader", "seg", "toggle", "button")):
        continue
    path = os.path.join(skins, f)
    im = Image.open(path).convert("RGBA"); px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0 or max(r, g, b) - min(r, g, b) > 24: continue
            if max(r, g, b) > 140:
                if f.startswith("knob") and min(r, g, b) > 180: px[x, y] = (40, 29, 19, a)
                continue
            t = min(1.0, max(0.0, ((r + g + b) / 3 - 25) / 45.0))
            px[x, y] = tuple(int(TAN[i] + (CREAM[i] - TAN[i]) * t) for i in range(3)) + (a,)
    im.save(path)
print("Bloom Voice: tile + light theme")
