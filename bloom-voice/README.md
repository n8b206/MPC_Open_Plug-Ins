# Bloom Voice

A 16-band vocoder by N8b, made to go with Chord Bloom: sing or talk into a mic and your voice plays the chords.

The MPC gives plugin instruments no audio input and plugin effects no MIDI, so the two meet on a submix:

1. Chord Bloom: on its MIX page set **Output** to **Vocoder L** (its sound goes to the left side only). Route the Chord Bloom track to **Submix 1**.
2. Your mic: an audio track with the mic input monitored, panned hard **right**, routed to **Submix 1**.
3. Put **Bloom Voice** on Submix 1. The left side is the carrier (the chords), the right side is your voice.

If it sounds inside out, flip **Inputs**. Controls: Mix (dry voice to vocoded), Mic Gain, Formant, Release, Sibilance (lets "s" and "t" through), Breath (adds noise for consonants), Synth Thru, Mic Gate, Output.

## Build

Built with Akai's [MPC Plugin Builder SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). Copy this folder into the SDK's `plugins/BloomVoice`, then:

```
python tools/build.py plugins/BloomVoice
python plugins/BloomVoice/post_build.py
python tools/package.py plugins/BloomVoice
```
