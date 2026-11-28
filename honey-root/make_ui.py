"""Writes ui.json. Each page is given as up to four COLUMNS of four controls (one Q-Link group each)."""
import json, os
K = lambda p, **kw: dict(param=p, widget="knob", **kw)
M = lambda p: dict(param=p, widget="knob", overlay="menu")
S = lambda p: dict(param=p, widget="switch")
F = lambda p: dict(param=p, widget="fader")
E = "empty"
pages = [
    ("PLAY",    [[K("Voicing", polarity="bipolar"), M("Perform"), F("Bass Level"), F("Reverb")]]),
    ("CHORD",   [[S("Lock"), S("Latch"), S("Play Style"), S("Secret Chords")],
                 [K("Voicing", polarity="bipolar"), K("Octave", polarity="bipolar"), S("6th with 7th"), F("Velocity")],
                 [S("Key Mode"), M("Key"), M("Scale"), K("Transpose", polarity="bipolar")],
                 [S("Layout"), M("Block Start"), S("Split Bass"), S("Pad Learn")]]),
    ("PERFORM", [[M("Perform"), K("Perf Rate"), M("Perf Div"), M("Perf Dir")],
                 [K("Perf Gate"), K("Perf Swing"), K("Perf Range"), K("Slop")],
                 [M("Pattern"), E, E, E]]),
    ("BASS",    [[M("Bass Mode"), K("Bass Register", polarity="bipolar"), F("Bass Level"), S("Bass Sound")],
                 [M("Bass Perform"), M("Bass Div"), S("Bass Dir"), K("Bass Gate")],
                 [K("Bass Tone"), K("Bass Decay"), K("Bass Glide"), E]]),
    ("SOUND",   [[S("Engine"), K("Tone"), K("Detune"), K("Env Amount")],
                 [K("Brightness"), K("Resonance"), E, E],
                 [F("Attack"), F("Decay"), F("Sustain"), F("Release")]]),
    ("FX",      [[F("Chorus"), K("Drive"), E, F("Volume")],
                 [F("Delay"), M("Delay Time"), K("Feedback"), E],
                 [F("Reverb"), K("Room Size"), E, E]]),
]
out = []
for name, cols in pages:
    slots = [E] * 16
    for c, col in enumerate(cols):
        for r, ctl in enumerate(col):
            slots[r * 4 + c] = ctl
    while slots and slots[-1] == E: slots.pop()
    out.append({"name": name, "controls": slots})
ui = {"kit": {"theme": "amber", "accent": "ffe6b14b"}, "pages": out,
      "style": {"background": "fff3e7d0", "text": "ff140d07", "muted": "ff4e2a0e", "accent": "ffc98a1e"}}
json.dump(ui, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui.json"), "w"), indent=2)
print("ui.json written")
