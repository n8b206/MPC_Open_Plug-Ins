# Chord Bloom

A one-finger chord instrument for Akai MPC standalone hardware, by N8b. Version 1.4.1. Inspired by
chord-synth instruments where one hand picks the chord type and the other plays roots.

Hold a chord type and play a root note: you get a full, voiced chord through a built-in
pad / keys synth with chorus. Block chords, strums or a tempo-synced arpeggio.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta): turn on **Enable Enhancements** and **External Plugin Support**.

## Playing on pads (default layout: 16 Pads, Chord Mode = In Key)

Turn on **Pad Learn** (CHORD page) and tap your 16 pads once so Chord Bloom knows which notes they send. It remembers this for every preset and project.

| Pads | What they do |
|---|---|
| A01 to A04 | Maj, Sus4, Min, Dim (hold to change the chord type) |
| A05 to A08 | add 9, add maj7, add m7, add 6 (hold to add the note, let go to remove it) |
| A09 to A16 | Scale degrees 1 to 8 of the chosen Key/Scale (or plain notes, with **Top Pads** = Notes) |

Tap a degree pad alone for the in-key chord; hold chord pads to change it. The PADS page shows the chord names live.

## Playing on a keyboard (Chord Keys = Keys)

**Chord keys** (CHORD page → Chord Keys = White Keys, Zone Start = C1):

| Key | C1 | D1 | E1 | F1 | G1 | A1 | B1 | C2 |
|---|---|---|---|---|---|---|---|---|
| Does | Dim | Min | Maj | Sus4 | add 6 | add m7 | add maj7 | add 9 |

Hold one or more of them with your left hand and play roots anywhere above C2 with the right.
Combine: Min + m7 = m7, Min + m7 + 9 = m9, Maj + maj7 = maj7, Dim + m7 = m7b5, Dim + 6 = dim7.
With no chord key held, the CHORD page's Chord and Add switches decide. Black keys in the
zone are silent. "Chrom" puts the 8 chord keys on 8 adjacent notes (handy on pads).
**Key Latch** makes the last chord-key combination stick after you let go.

**Chord Mode = In Key**: every root in the chosen Key/Scale plays its diatonic chord
(e.g. in C major: D → Dm, G → G, B → Bdim; the Add 6/7/9 switches add diatonic notes).
Holding a triad chord key still overrides the quality.

## Pages

| Page | Controls |
|---|---|
| PERFORM | Filter, Delay, Reverb, Drive, plus the bee pad (Riser / Dive / Space / Wub moves, Hold) |
| PADS | Live chord names for each pad, Key, Scale, Top Pads, Play Mode |
| CHORD | Chord, Chord Mode (Manual / In Key), Key, Scale, Add 6 / m7 / maj7 / 9, Chord Keys, Zone Start, Pad Learn, Key Latch |
| VOICING | Inversion, Voicing (Close / Drop 2 / Open / Wide), Voice Lead, Octave, Bass Note |
| PLAY | Play Mode (Chord / Strum / Arp), strum and arp settings, Velocity, Vel Curve, Aftertouch, AT Target |
| SOUND | Engine (Synth / Organ / Piano / Keys), filter, envelope and tone controls |
| KEYS | Organ drawbars, percussion and click, Piano Blend (Airy / Piano / Strings), Rotary speaker |
| MIX | Volume, Output (Stereo or Vocoder L for Bloom Voice), MIDI Out |

Presets: Bloom Pad, Strum Harp, Neo Soul Keys, In-Key Arp, Glass Keys, Dark Strings, Gospel Organ, Rock Organ, Grand Piano, Airy Piano, String Piano.

## Q-Link knobs

The PERFORM page opens first. In the MPC's Program Q-Link mode the 16 knobs always control, in order: Filter, Delay, Reverb, Drive, Bee Fly, Feedback, Room Size, Chorus, Delay Time, Verb Tone, Brightness, Rotor Speed, Rotary, Release, Piano Blend, Volume (set by `post_build.py` after the build).

## Honest notes

- 16 voices. On a 32-bit MPC (One, Live II, Key 61...) watch the CPU meter with big, long chords.
- The MPC ignores normal plugin MIDI output, so **MIDI Out** sends through Chord Bloom's own MIDI port instead. Turn on Track for it in Preferences > MIDI.
- A plugin can't light the MPC's hardware pads, so the PADS page shows what's held instead.
- Loads and plays on an MPC Live III (MPC OS 3.10.1).

## Build

```
python tools/build.py plugins/ChordBloom
python plugins/ChordBloom/post_build.py
python tools/package.py plugins/ChordBloom
```
MIT licence.

