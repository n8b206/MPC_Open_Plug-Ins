# Honey Root

A two-handed chord machine for Akai MPC standalone hardware, by N8b. Version 1.0.0.
Inspired by chord-machine keyboards (the Orchid, and ChoralRoot on the M-Vave FM-1 / SMK-37):
your left hand holds chord keys at the bottom of the keyboard, your right hand plays roots, and
every root becomes a named, voiced chord, with a bass line under it and a performance mode on top.
Chord Bloom's sibling: Chord Bloom is built for the pads, Honey Root for a keyboard.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta): turn on **Enable Enhancements** and **External Plugin Support**.
- A MIDI keyboard (made for 37-key boards like the M-Vave SMK-37) or the MPC's pads.

## Playing on a keyboard (Layout = Keys, Block Start = F, note 53)

```
black:        DIM      MIN      MAJ              (MIN + MAJ together = SUS)
white:    6        m7       M7        9      |  roots: every key from the C above
          F   F#   G   G#   A    A#   B      |  C ...
```

- Hold a chord type and any extensions, then play a root: MIN + m7 + 9 = m9, MAJ + M7 = maj7,
  DIM + 6 = dim7, DIM + m7 = m7b5, 6 + m7 = 13. Extensions stack freely.
- A root with no chord keys plays a single note (or, with **Key Mode** on, the chord of the scale).
- **Play Style**: Simple fixes the chord when the root goes down; Live lets the chord keys change
  the chord you are holding. MIN and MAJ pressed with one finger always land together as SUS.
- **Lock**: chord keys stay on after you let go. A type key replaces the locked chord, an extension
  key switches its extension on and off, and the same lone type again goes back to single notes.
- **Voicing** (-12..+12): each step up moves the lowest note up an octave, each step down moves
  the highest note down. **Octave** moves everything.
- **Secret Chords**: DIM + MAJ = augmented, DIM + MIN = m(add4), DIM + MIN + MAJ = power chord.
- **6th with 7th** = Oct Up plays the 6 an octave higher whenever a seventh is held (a real 13th).
- **Classic** layout: the same, plus LOCK on B, the 9 on C and SUS on C#; roots start at D.
- Set **Block Start** to the F your keyboard sends if your board is transposed.
- **Split Bass**: keys below Block Start play the bass by hand.

## Playing on the MPC pads (Layout = Pads)

A01 Maj, A02 Sus4, A03 Min, A04 Dim, A05 add9, A06 add maj7, A07 add m7, A08 add 6 (Chord Bloom's
order), A09-A16 = scale degrees 1-8 of Key / Scale. Honey Root reads Chord Bloom's Pad Learn if you
did it there; otherwise turn on **Pad Learn** and hit A01 to A16 once.

## Key Mode

Key + Scale (16 scales: Major, Minor, the modes, harmonic and melodic minor, pentatonics, Blues,
Phrygian Dominant, Hungarian, Whole Tone, Diminished 6th). A lone root plays the scale's own chord
(in C major, D = Dm, B = Bdim); a root outside the scale plays a sus4. Held chord keys still win.
**Transpose** moves everything by semitones.

## Perform (the chords)

| Mode | What it does |
|---|---|
| Off | block chords |
| Strum | the notes one after another (Perf Rate, in ms at 120 BPM, follows the tempo) |
| Slop | a strum with uneven timing and touch (Slop amount) |
| Arp | the notes in turn, synced to the MPC tempo (Perf Div, Dir, Gate, Swing, Range) |
| Pattern | twelve rhythmic figures over the chord (Pulse, Rock, Waltz, Cascade, Bounce, Offbeat, Gallop, Skip, Ballad, Stride, Drive, Ladder) |
| Harp | a fast run over two or three octaves |

Up-Dn and Dn-Up strums alternate direction on every chord, like a guitar. **Latch** keeps the
chord (and its arp) going after you let go; a sustain pedal does the same while it is down.

## Bass

**Bass Mode**: Chords (the root under every chord), Unison (under everything, at the pitch you
played), Singles (also plays single notes, on the bass alone), Solo (only the bass).
**Bass Perform**: Off, Follow (does what the chords do), Arp, Octaves, Pulse, Walk, with its own
Div / Dir / Gate. Four bass sounds (Sub, Saw, FM, Square), Bass Tone, Decay, Glide, Register.

## Sound

Four engines for the chords, each with a Tone control:
**Warm** (two detuned saws, Tone turns one into a pulse), **Glass** (two-operator FM bell),
**Phase** (phase distortion: Tone and Env Amount bend a sine into a saw), **Reed** (six harmonics).
Then Brightness, Resonance, Env Amount, ADSR, chorus, drive, tempo-synced ping-pong delay and reverb.
Mod wheel opens the filter; pitch bend is ±2 semitones.

## Pages

| Page | Controls |
|---|---|
| PLAY | the chord display (name, notes, key and modes) and Voicing, Perform, Bass Level, Reverb |
| CHORD | Lock, Latch, Play Style, Secret Chords, Voicing, Octave, 6th with 7th, Velocity, Key Mode, Key, Scale, Transpose, Layout, Block Start, Split Bass, Pad Learn |
| PERFORM | Perform, Perf Rate, Div, Dir, Gate, Swing, Range, Slop, Pattern |
| BASS | Bass Mode, Register, Level, Sound, Bass Perform, Div, Dir, Gate, Tone, Decay, Glide |
| SOUND | Engine, Tone, Detune, Env Amount, Brightness, Resonance, Attack, Decay, Sustain, Release |
| FX | Chorus, Drive, Volume, Delay, Delay Time, Feedback, Reverb, Room Size |

Presets: Honey Keys, Glass Strum, Phase Arp, Hive Pattern, Slop Guitar, Harp Rain, Lock and Key,
Bass Solo, Warm Pad.

Q-Links in the MPC's Program mode: Voicing, Perform, Perf Div, Perf Rate, Bass Level, Bass Tone,
Bass Register, Bass Perform, Brightness, Env Amount, Tone, Release, Chorus, Delay, Reverb, Volume.

## Honest notes

- 16 chord voices + 1 bass voice. On a 32-bit MPC (One, Live II, Key 61...) watch the CPU meter with
  Harp and three-octave ranges.
- No looper: record the chord keys and roots into an MPC sequence instead; Honey Root plays them back
  exactly, since it rebuilds every chord from the incoming notes.
- Tested in Akai's host simulator and a scripted test of every chord rule; not yet tried on a real MPC.

## Build

```
python tools/build.py "plugins/Honey Root"
python "plugins/Honey Root/post_build.py"
python tools/package.py "plugins/Honey Root"
```
`make_ui.py` writes `ui.json`; `make_art.py` draws the PLAY panel and the browser tile (needs Pillow).

Original code, MIT licence. Not affiliated with Telepathic Instruments, M-VAVE or ChoralRoot.
